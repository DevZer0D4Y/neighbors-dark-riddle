"""
Dark Riddle -> Nintendo Switch port: asset converter (streaming, low-memory).

Extracts the original Unity 2018.1 (Mono) content from the APK and packs it into
one DRPK file: meshes, textures, materials, levels, colliders, game markers,
audio and a font atlas.
"""
import os, sys, io, json, struct, math, collections, gc
import numpy as np
import UnityPy
# (name, x, z, radius): objects left out of the converted levels, matched by name
# and position (an oil drum outside the neighbour's front wall, a small propane tank
# by the garage in level 2)
EXTRA_PROPS = [('Oil_Drum', -107.57, -59.02, 0.5),
               ('Propane_Tank_Small (2)', -108.5, -18.42, 0.5)]
# name -> (x, z) world position override: the vase sits in the inner corner of the
# neighbour's front wall and the jutting room (walls at x -109.67 / z -57.95)
PROP_MOVES = {'Vase_1 (8)': (-109.25, -58.37)}
def is_extra(name, tr):
    if tr is None:
        return False
    p = tr.get('m_LocalPosition', {})
    for n, x, z, r in EXTRA_PROPS:
        if name == n and abs(p.get('x', 0) - x) <= r and abs(p.get('z', 0) - z) <= r:
            return True
    return False
from UnityPy.helpers.MeshHelper import MeshHandler
from PIL import Image, ImageDraw, ImageFont

sys.stdout.reconfigure(encoding='utf-8')

DATA_DIR = 'merged'
OUT_DIR = '../game/romfs'
BUILD_DIR = 'build'
MAX_TEX = 512
os.makedirs(OUT_DIR, exist_ok=True)
os.makedirs(BUILD_DIR, exist_ok=True)

TAG_TAKE, TAG_DOOR, TAG_NEIGHBOR, TAG_LOCK = 20000, 20001, 20002, 20003

print('Loading Unity data...', flush=True)
env = UnityPy.load(DATA_DIR)

def fname_of(o):
    return os.path.basename(o.assets_file.name)

# --------------------------------------------------------------- streaming
tex_file = open(os.path.join(BUILD_DIR, 'tex.bin'), 'wb+')
mesh_file = open(os.path.join(BUILD_DIR, 'mesh.bin'), 'wb+')
audio_file = open(os.path.join(BUILD_DIR, 'audio.bin'), 'wb+')

tex_meta = []       # (id, w, h, offset, size)
_tex_by_key = {}
mesh_meta = []      # (id, size)
audio_meta = []     # (name, ch, freq, frames, offset, size)

def export_texture(obj):
    orr = getattr(obj, 'object_reader', obj)
    key = (fname_of(orr), orr.path_id)
    if key in _tex_by_key:
        return _tex_by_key[key]
    _tex_by_key[key] = None
    try:
        d = obj.read() if hasattr(obj, 'read') else obj
        img = d.image
        if img is None:
            return None
        img = img.convert('RGBA')
        w, h = img.size
        # power-of-two sizes so the GLES2 renderer can tile (REPEAT) and mipmap them
        def pot(v):
            p = 1
            while p * 2 <= v:
                p *= 2
            if v - p > p * 2 - v:   # nearest power of two
                p *= 2
            return max(4, min(MAX_TEX, p))
        nw, nh = pot(w), pot(h)
        if (nw, nh) != (w, h):
            # resize colour and alpha separately: Pillow resizes RGBA premultiplied, which
            # zeroes the colour of fully transparent texels (skybox sides keep their sky
            # colour in alpha-0 texels; cutout foliage would get black fringes)
            flt = Image.LANCZOS if nw < w or nh < h else Image.BILINEAR
            rgb = img.convert('RGB').resize((nw, nh), flt)
            alpha = img.getchannel('A').resize((nw, nh), flt)
            img = rgb.convert('RGBA')
            img.putalpha(alpha)
            w, h = img.size
        if w == 0 or h == 0:
            return None
        data = img.tobytes()
        tid = len(tex_meta) + 1
        off = tex_file.seek(0, 2)
        tex_file.write(data)
        tex_meta.append((tid, w, h, off, len(data), d.m_Name))
        _tex_by_key[key] = tid
        return tid
    except Exception:
        return None

_mat_cache = {}
_materials = []
_mat_ids = {}

def mat_key(info):
    if info in _mat_ids:
        return _mat_ids[info]
    mid = len(_materials) + 1
    _materials.append(info)
    _mat_ids[info] = mid
    return mid

def material_info(mat):
    orr = getattr(mat, 'object_reader', None)
    key = (fname_of(orr), orr.path_id) if orr else None
    if key and key in _mat_cache:
        return _mat_cache[key]
    texid = 0
    r = g = b = a = 1.0
    tx = ty = 1.0
    ox = oy = 0.0
    cutout = 0
    cutoff = 0.5
    try:
        props = mat.m_SavedProperties
        for k, v in props.m_TexEnvs:
            if k == '_MainTex' and v.m_Texture:
                try:
                    texid = export_texture(v.m_Texture.read()) or 0
                except Exception:
                    texid = 0
                tx, ty = v.m_Scale.x, v.m_Scale.y
                ox, oy = v.m_Offset.x, v.m_Offset.y
        for k, v in props.m_Colors:
            if k == '_Color':
                r, g, b, a = v.r, v.g, v.b, v.a
        sh = ''
        try:
            shd = mat.m_Shader.read()
            try:
                sh = shd.m_ParsedForm.m_Name or ''
            except Exception:
                sh = ''
            sh = sh or shd.m_Name or ''
        except Exception:
            pass
        # Mobile/Diffuse, Mobile/Bumped Diffuse, Unlit/Texture have no _Color: a value
        # stored in the material (e.g. Explosives' red) is never used
        if sh.startswith('Mobile/') or sh in ('Unlit/Texture', 'Unlit/Transparent', 'Unlit/Transparent Cutout'):
            r = g = b = a = 1.0
        # Shader names are not always readable in this build, so also use the
        # material's own render state: Standard _Mode (1 cutout, 2 fade, 3 transparent),
        # render queue (2450 alpha test, >= 2900 transparent), keywords and RenderType tag.
        tt = {}
        try:
            tt = mat.object_reader.read_typetree()
        except Exception:
            pass
        floats = dict(tt.get('m_SavedProperties', {}).get('m_Floats', []))
        kw = tt.get('m_ShaderKeywords', '') or ''
        tags = dict(tt.get('stringTagMap', []) or [])
        q = tt.get('m_CustomRenderQueue', -1)
        mode = floats.get('_Mode', 0.0)
        cutoff = float(floats.get('_Cutoff', 0.5))
        if ('Cutout' in sh or 'Foliage' in sh or 'Leaf' in sh or mode == 1.0 or
                '_ALPHATEST_ON' in kw or tags.get('RenderType') == 'TransparentCutout' or 2400 <= q < 2900):
            cutout = 1
        if ('Transparent' in sh or 'Alpha Blended' in sh or mode in (2.0, 3.0) or
                tags.get('RenderType') == 'Transparent' or q >= 2900) and cutout != 1:
            cutout = 2
        if 'Particles' in sh and cutout == 0:
            cutout = 2
    except Exception:
        cutoff = 0.5
    info = (texid, round(r, 3), round(g, 3), round(b, 3), round(a, 3),
            round(tx, 3), round(ty, 3), round(ox, 3), round(oy, 3), cutout,
            round(cutoff if cutout == 1 else 0.0, 3))
    if key:
        _mat_cache[key] = info
    return info

class Writer:
    def __init__(self):
        self.buf = bytearray()
    def u32(self, v): self.buf += struct.pack('<I', int(v))
    def u16(self, v): self.buf += struct.pack('<H', int(v))
    def u8(self, v): self.buf += struct.pack('<B', int(v))
    def f32(self, v): self.buf += struct.pack('<f', float(v))
    def raw(self, b): self.buf += b
    def vec3(self, v): self.buf += struct.pack('<fff', float(v[0]), float(v[1]), float(v[2]))
    def quat(self, q): self.buf += struct.pack('<ffff', float(q[0]), float(q[1]), float(q[2]), float(q[3]))
    def name(self, s, n=40):
        b = s.encode('utf-8', 'replace')[:n - 1]
        self.raw(b + b'\0' * (n - len(b)))

def add_mesh(blob):
    mid = len(mesh_meta) + 1
    mesh_file.write(blob)
    mesh_meta.append((mid, len(blob)))
    return mid

_VT = np.dtype([('p', '<f4', (3,)), ('uv', '<f4', (2,)), ('c', 'u1', (4,))])

def mesh_blob(matid, verts, uvs, idx, colors=None):
    n = len(verts)
    rec = np.zeros(n, dtype=_VT)
    rec['p'] = np.asarray(verts, np.float32)
    rec['uv'] = np.asarray(uvs, np.float32)
    # vertex colour = baked light, stored at half scale so 0..255 covers 0..2
    # (the shader multiplies by 2), letting several lights brighten past albedo
    if colors is None:
        rec['c'][:, :3] = 128
    else:
        colors = np.asarray(colors, np.float32)
        rec['c'][:, :3] = (np.clip(colors[:, :3] * 0.5, 0, 1) * 255 + 0.5).astype(np.uint8)
    # alpha = 1 - N.L of the level's switchable sun (houseLights); the shader adds
    # uSwitch * (1 - a) while that light is on
    rec['c'][:, 3] = 255
    if colors is not None and colors.shape[1] == 4:
        rec['c'][:, 3] = (255 - np.clip(colors[:, 3], 0, 1) * 255 + 0.5).astype(np.uint8)
    return struct.pack('<III', int(matid), n, len(idx)) + rec.tobytes() + np.asarray(idx, np.uint32).tobytes()

# --------------------------------------------------------------- math
def quat_matrix(q):
    x, y, z, w = q
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    x, y, z, w = x / n, y / n, z / n, w / n
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 0],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 0],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), 0],
        [0, 0, 0, 1]], dtype=np.float32)

def trs_matrix(p, q, s):
    m = quat_matrix(q)
    # Unity local matrix = T * R * S: scale the rotation's columns, not its rows
    m[:3, 0] *= s[0]; m[:3, 1] *= s[1]; m[:3, 2] *= s[2]
    m[0, 3], m[1, 3], m[2, 3] = p
    return m

_geo_cache = {}

def mesh_geometry_cached(msh):
    orr = getattr(msh, 'object_reader', None)
    key = (fname_of(orr), orr.path_id) if orr else None
    if key and key in _geo_cache:
        return _geo_cache[key]
    g = mesh_geometry(msh)
    subs = msh.m_SubMeshes or []
    aabb = msh.m_LocalAABB
    if key:
        _geo_cache[key] = (g, subs, aabb)
    return (g, subs, aabb)

def mesh_geometry(msh):
    h = MeshHandler(msh)
    h.process()
    verts = np.array(h.m_Vertices, dtype=np.float32) if h.m_Vertices else np.zeros((0, 3), np.float32)
    normals = np.array(h.m_Normals, dtype=np.float32) if h.m_Normals else None
    if normals is not None and normals.shape[1] == 4:
        normals = normals[:, :3]
    uvs = np.array(h.m_UV0, dtype=np.float32) if h.m_UV0 else None
    if uvs is not None and len(uvs) < len(verts):
        uvs = None
    idx = np.array(h.m_IndexBuffer, dtype=np.int64) if h.m_IndexBuffer else np.zeros(0, np.int64)
    return verts, normals, uvs, idx

def sm_first(sm):
    return getattr(sm, 'm_FirstByte', getattr(sm, 'firstByte', 0))

def sm_icount(sm):
    return getattr(sm, 'm_IndexCount', getattr(sm, 'indexCount', 0))

def aabb_parts(bb):
    c = getattr(bb, 'm_Center', getattr(bb, 'center', None))
    e = getattr(bb, 'm_Extent', getattr(bb, 'extent', None))
    def get(o, k, d):
        if o is None: return d
        if isinstance(o, dict): return o.get(k, d)
        return getattr(o, k, d)
    return [get(c, 'x', 0), get(c, 'y', 0), get(c, 'z', 0)], [get(e, 'x', .5), get(e, 'y', .5), get(e, 'z', .5)]

# --------------------------------------------------------------- scene build
SCENES = [('level3', 1), ('level4', 2), ('level5', 3), ('level6', 4),
          ('level7', 5), ('level8', 6), ('level9', 7)]

def scene_objects(scene_file):
    return [o for o in env.objects if fname_of(o) == scene_file]

def build_scene(scene_file, level_no):
    print('  level', level_no, scene_file, flush=True)
    objs = scene_objects(scene_file)
    gos, trs = {}, {}
    for o in objs:
        if o.type.name == 'GameObject':
            gos[o.path_id] = o.read_typetree()
        elif o.type.name in ('Transform', 'RectTransform'):
            try:
                trs[o.path_id] = o.read_typetree()
            except Exception:
                pass
    tr2go = {tid: t['m_GameObject']['m_PathID'] for tid, t in trs.items()}
    go2tr = {}
    for k, v in tr2go.items():
        go2tr[v] = k

    # FirstPersonController.houseLights (PPtr at byte 208 of its data): the object
    # CutTheWires() deactivates. Levels 1-6: a directional light; level 7: the dirt
    # mounds over the grave.
    house_go = 0
    for o in objs:
        if o.type.name != 'MonoBehaviour':
            continue
        try:
            if o.read(check_read=False).m_Script.read().m_Name != 'FirstPersonController':
                continue
            fid, pid = struct.unpack_from('<iq', o.get_raw_data(), 208)
            if fid == 0 and pid in gos:
                house_go = pid
        except Exception:
            pass
    print('    houseLights', gos[house_go]['m_Name'] if house_go else None, flush=True)

    wcache = {}
    def world(tid, depth=0):
        if tid in wcache:
            return wcache[tid]
        t = trs.get(tid)
        if t is None or depth > 64:
            return np.eye(4, dtype=np.float32)
        lp, lr, ls = t['m_LocalPosition'], t['m_LocalRotation'], t['m_LocalScale']
        local = trs_matrix((lp['x'], lp['y'], lp['z']), (lr['x'], lr['y'], lr['z'], lr['w']),
                           (ls['x'], ls['y'], ls['z']))
        f = t.get('m_Father', {}).get('m_PathID', 0)
        m = (world(f, depth + 1) @ local) if (f and f in trs) else local
        wcache[tid] = m
        return m

    lights = []
    switch_light = None
    ambient_set = False
    ambient = np.array([0.25, 0.25, 0.3], np.float32)
    fog = (0.05, 0.05, 0.07)
    for o in objs:
        if o.type.name == 'Light':
            try:
                d = o.read_typetree()
                tid = go2tr.get(d['m_GameObject']['m_PathID'])
                if tid is None or not d.get('m_Enabled', True):
                    continue
                w = world(tid)
                col = d.get('m_Color', {'r': 1, 'g': 1, 'b': 1})
                # a Unity light shines along its transform's forward (+Z) axis
                fwd = w[:3, 2].copy()
                n = np.linalg.norm(fwd) or 1
                lt = (w[:3, 3].astype(np.float32), (fwd / n).astype(np.float32),
                      np.array([col['r'], col['g'], col['b']], np.float32),
                      float(d.get('m_Intensity', 1.0)), float(d.get('m_Range', 10.0)),
                      int(d.get('m_Type', 2)))
                if d['m_GameObject']['m_PathID'] == house_go and lt[5] == 1:
                    switch_light = lt
                else:
                    lights.append(lt)
            except Exception:
                pass
        elif o.type.name == 'RenderSettings':
            try:
                d = o.read_typetree()
                am = d.get('m_AmbientSkyColor') or {}
                probe = d.get('m_AmbientProbe') or {}
                if d.get('m_AmbientMode', 0) == 0 and probe and all(abs(v) < 1e-6 for v in probe.values()):
                    # Skybox ambient that was never baked: the player build gets none
                    ambient = np.zeros(3, np.float32)
                    ambient_set = True
                elif am:
                    k = float(d.get('m_AmbientIntensity', 1.0))
                    ambient = np.array([am['r'], am['g'], am['b']], np.float32) * k
                    ambient_set = True
                fc = d.get('m_FogColor') or {}
                if fc:
                    fog = (fc['r'], fc['g'], fc['b'])
            except Exception:
                pass
    if ambient.max() < 0.06 and not ambient_set:
        ambient = np.array([0.18, 0.18, 0.22], np.float32)
    print('    ambient', ambient.round(3), 'lights', [(lt, round(li, 2), lc.round(2).tolist(), round(lr, 1))
                                                   for (_, _, lc, li, lr, lt) in lights][:12],
          'total', len(lights), flush=True)

    # QualitySettings "Very Low" has pixelLightCount 0: every directional light reaches the
    # Mobile / Legacy / Standard shaders as spherical harmonics (ShadeSH9), i.e. the L2
    # approximation of the clamped cosine instead of a hard max(0, N.L)
    def sh_lobe(c):
        return np.clip(0.25 + 0.5 * c + 0.3125 * (1.5 * c * c - 0.5), 0.0, None)

    # returns Nx4: baked rgb without the switchable light, and that light's N.L in [:, 3]
    def bake(verts, normals):
        col = np.tile(ambient, (len(verts), 1)).astype(np.float32)
        sw = np.zeros((len(verts), 1), np.float32)
        if normals is not None and len(normals) == len(verts) and switch_light is not None:
            sw[:, 0] = np.clip(sh_lobe((normals * (-switch_light[1])).sum(1)), 0, 1)
        if normals is not None and len(normals) == len(verts):
            for (lp, ld, lc, li, lr, lt) in lights:
                if lt == 1:
                    ndl = sh_lobe((normals * (-ld)).sum(1))
                    col += ndl[:, None] * lc * li
                else:
                    dv = lp[None, :] - verts
                    dist = np.linalg.norm(dv, axis=1) + 1e-4
                    att = np.clip(1.0 - dist / max(lr, 0.01), 0, 1) ** 1.5
                    ndl = np.clip((dv / dist[:, None] * normals).sum(1), 0, 1)
                    if lt == 0:
                        ca = (dv / dist[:, None] * (-ld)).sum(1)
                        att = att * np.clip((ca - 0.45) * 2, 0, 1)
                    col += (ndl * att)[:, None] * lc * (li * 1.7)
        return np.concatenate([np.clip(col, 0.0, 2.0), sw], 1)

    batches = {}
    mm_batches = {}
    col_v, col_i, col_n = [], [], [0]   # static collision geometry (Unity colliders)
    def add_geom(matid, verts, uvs, idx, cols):
        b = batches.get(matid)
        if b is None:
            b = batches[matid] = [[], [], [], [], 0]
        base = b[4]
        b[0].append(verts); b[1].append(uvs); b[2].append(cols)
        b[3].append(np.asarray(idx, np.uint32).reshape(-1) + base)
        b[4] = base + len(verts)

    markers, props, colliders = [], [], []
    n_sprites = [0]
    # pickups the player collects by walking into them (FirstPersonController
    # OnTriggerEnter destroys them): exported as props so they can disappear
    ITEMS = ('Key', 'Scissors', 'MoneyLoot')
    item_cache = {}
    def item_root(gid, depth=0):
        if gid in item_cache:
            return item_cache[gid]
        g = gos.get(gid)
        res = ''
        if g is not None and depth < 64:
            if g.get('m_Name') in ITEMS:
                res = g['m_Name']
            else:
                tid = go2tr.get(gid)
                f = trs.get(tid, {}).get('m_Father', {}).get('m_PathID', 0) if tid else 0
                res = item_root(tr2go[f], depth + 1) if (f and f in tr2go) else ''
        item_cache[gid] = res
        return res

    def under(gid, root, depth=0):
        while gid and depth < 64:
            if gid == root:
                return True
            tid = go2tr.get(gid)
            f = trs.get(tid, {}).get('m_Father', {}).get('m_PathID', 0) if tid else 0
            gid = tr2go.get(f, 0) if f else 0
            depth += 1
        return False
    house_group = set()
    if house_go and switch_light is None:
        house_group = {gid for gid in gos if under(gid, house_go)}

    # BreakableGlass windows: Start() hides brokenGlass; a thrown "Take" object entering
    # the window's trigger hides solidGlass and shows brokenGlass (rigidbody shards)
    glass_role = {}      # gid -> ('solid' | 'broken', window index)
    glass_bg = []        # window index -> BreakableGlass gid
    for o in objs:
        if o.type.name != 'MonoBehaviour':
            continue
        try:
            if o.read(check_read=False).m_Script.read().m_Name != 'BreakableGlass':
                continue
            raw = o.get_raw_data()
            bg_gid = struct.unpack_from('<q', raw, 4)[0]
            q = 28
            ln = struct.unpack_from('<i', raw, q)[0]; q += 4 + ((ln + 3) & ~3)
            f1, solid = struct.unpack_from('<iq', raw, q)
            f2, broken = struct.unpack_from('<iq', raw, q + 12)
        except Exception:
            continue
        if bg_gid not in gos or not gos[bg_gid].get('m_IsActive', 1):
            continue
        k = len(glass_bg)
        glass_bg.append(bg_gid)
        for gid in gos:
            if solid and under(gid, solid):
                glass_role[gid] = ('solid', k)
            elif broken and under(gid, broken):
                glass_role[gid] = ('broken', k)
    print('    windows', len(glass_bg), 'glass objects', len(glass_role), flush=True)

    dynamic = set(house_group) | set(glass_role)
    for gid, g in gos.items():
        if g.get('m_Tag', 0) in (TAG_DOOR, TAG_TAKE, TAG_NEIGHBOR, TAG_LOCK) or item_root(gid):
            dynamic.add(gid)

    comps = collections.defaultdict(list)
    for o in objs:
        if o.type.name == 'GameObject':
            continue
        try:
            d = o.read_typetree()
        except Exception:
            continue
        g = d.get('m_GameObject', {}).get('m_PathID')
        if g:
            comps[g].append(o)

    # active-in-hierarchy: a GameObject renders only if it and all its parents are active
    act_cache = {}
    def active(gid, depth=0):
        if gid in act_cache:
            return act_cache[gid]
        g = gos.get(gid)
        if g is None or depth > 64:
            return True
        ok = bool(g.get('m_IsActive', 1)) and not is_extra(g.get('m_Name', ''), trs.get(go2tr.get(gid)))
        if ok:
            tid = go2tr.get(gid)
            f = trs.get(tid, {}).get('m_Father', {}).get('m_PathID', 0) if tid else 0
            if f and f in tr2go:
                ok = active(tr2go[f], depth + 1)
        act_cache[gid] = ok
        return ok

    # LODGroup: only LOD0 renderers are drawn (the original uses them at close range)
    lod_skip = set()
    for o in objs:
        if o.type.name != 'LODGroup':
            continue
        try:
            d = o.read_typetree()
            for li, lod in enumerate(d.get('m_LODs', [])):
                if li == 0:
                    continue
                for rr in lod.get('renderers', []):
                    pid = rr.get('renderer', {}).get('m_PathID', 0)
                    if pid:
                        lod_skip.add(pid)
        except Exception:
            pass

    # the neighbour is an animated character exported by make_char.py (char.pak);
    # nothing under NeighborNew belongs to the static level
    nb_cache = {}
    def under_neighbor(gid, depth=0):
        if gid in nb_cache:
            return nb_cache[gid]
        g = gos.get(gid)
        res = False
        if g is not None and depth < 64:
            if g.get('m_Name') == 'NeighborNew':
                res = True
            else:
                tid = go2tr.get(gid)
                f = trs.get(tid, {}).get('m_Father', {}).get('m_PathID', 0) if tid else 0
                res = bool(f and f in tr2go and under_neighbor(tr2go[f], depth + 1))
        nb_cache[gid] = res
        return res

    # animated trees (legacy Animation + skinned mesh) are drawn from trees.pak
    anim_roots = {gid for gid in gos if any(c.type.name == 'Animation' for c in comps.get(gid, []))}
    def under_anim(gid, depth=0):
        while gid in gos and depth < 64:
            if gid in anim_roots:
                return True
            tid_ = go2tr.get(gid)
            f = trs.get(tid_, {}).get('m_Father', {}).get('m_PathID', 0) if tid_ else 0
            if not (f and f in tr2go):
                return False
            gid = tr2go[f]; depth += 1
        return False

    for gid, g in gos.items():
        name = g.get('m_Name', '')
        tag = g.get('m_Tag', 0)
        tid = go2tr.get(gid)
        if tid is None:
            continue
        if not active(gid):
            continue
        if under_anim(gid) and any(c.type.name == 'SkinnedMeshRenderer' for c in comps.get(gid, [])):
            continue
        if under_neighbor(gid) and name != 'NeighborNew':
            continue
        w = world(tid)
        if name in PROP_MOVES and trs.get(tid, {}).get('m_Father', {}).get('m_PathID', 0) == 0:
            w = w.copy(); w[0, 3], w[2, 3] = PROP_MOVES[name]
        cl = comps.get(gid, [])
        is_dyn = gid in dynamic
        mr_c = next((c for c in cl if c.type.name == 'MeshRenderer'), None)
        mr_tt = None
        if mr_c is not None:
            try:
                mr_tt = mr_c.read_typetree()
            except Exception:
                mr_tt = None
        render_ok = True
        if mr_c is not None and (mr_c.path_id in lod_skip or (mr_tt is not None and not mr_tt.get('m_Enabled', 1))):
            render_ok = False

        def typed(tname):
            c = next((c for c in cl if c.type.name == tname), None)
            if c is None:
                return None
            try:
                return c.read()
            except Exception:
                return None

        mf = typed('MeshFilter')
        mr = typed('MeshRenderer')
        smr = typed('SkinnedMeshRenderer')

        mesh_obj = None
        mats = []
        if mf is not None and mr is not None:
            try:
                mesh_obj = mf.m_Mesh.read() if mf.m_Mesh else None
            except Exception:
                mesh_obj = None
            mats = list(getattr(mr, 'm_Materials', []) or [])
        elif smr is not None:
            try:
                mesh_obj = smr.m_Mesh.read() if smr.m_Mesh else None
            except Exception:
                mesh_obj = None
            mats = list(getattr(smr, 'm_Materials', []) or [])

        if mesh_obj is not None and render_ok:
            try:
                (verts, normals, uvs, idx), subs_cached, aabb_cached = mesh_geometry_cached(mesh_obj)
            except Exception:
                verts = None; subs_cached = []; aabb_cached = None
            if verts is not None and len(verts) and len(idx):
                subs = subs_cached or []
                # Unity static batching: each renderer owns one slice of a shared combined mesh
                # whose vertices are already in world space.
                first_sub, sub_count = 0, 1
                have_sbi = False
                sbi = (mr_tt or {}).get('m_StaticBatchInfo')
                if sbi and sbi.get('subMeshCount', 0) > 0:
                    first_sub = int(sbi['firstSubMesh'])
                    sub_count = int(sbi['subMeshCount'])
                    have_sbi = True
                rw = np.eye(4, dtype=np.float32) if have_sbi else w
                if not subs:
                    subs = [None]
                if not have_sbi:
                    first_sub = 0
                    sub_count = max(1, min(len(mats) if mats else 1, len(subs)))
                rng = range(first_sub, min(first_sub + sub_count, len(subs)))
                dyn_parent = None
                for si in rng:
                    sm = subs[si]
                    if sm is None:
                        sub_idx = idx
                    else:
                        fb = sm_first(sm) // 2
                        sub_idx = idx[fb:fb + sm_icount(sm)]
                    if len(sub_idx) == 0:
                        continue
                    uniq, inv = np.unique(np.asarray(sub_idx, np.int64), return_inverse=True)
                    if len(uniq) == 0:
                        continue
                    v = verts[uniq]
                    n = normals[uniq] if (normals is not None and len(normals) == len(verts)) else None
                    u = uvs[uniq] if uvs is not None and len(uvs) == len(verts) else np.zeros((len(uniq), 2), np.float32)
                    vw = (rw @ np.concatenate([v, np.ones((len(v), 1), np.float32)], 1).T).T[:, :3]
                    if n is not None:
                        nm = (np.linalg.inv(rw[:3, :3].astype(np.float64)).T @ n.astype(np.float64).T).T
                        ln = np.linalg.norm(nm, axis=1, keepdims=True); ln[ln == 0] = 1
                        nm = (nm / ln).astype(np.float32)
                    else:
                        nm = None
                    cols = bake(vw, nm)
                    if is_dyn:
                        mi_ = si - first_sub
                        mp = mats[mi_] if mi_ < len(mats) else (mats[-1] if mats else None)
                        try:
                            info = material_info(mp.read()) if mp is not None else (0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0.0)
                        except Exception:
                            info = (0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0.0)
                        # props move at runtime; bake them at their start pose, which is
                        # where they spend most of the time
                        # pivot = the transform's origin (door hinge, prop origin); for a
                        # static-batch slice (already world space) the slice's centre
                        pivot = ((vw.min(0) + vw.max(0)) / 2) if have_sbi else w[:3, 3].copy()
                        if si != first_sub and dyn_parent is not None:
                            pivot = props[dyn_parent]['world'][:3, 3].copy()   # parts share the parent's pivot
                        vv = (vw - pivot).astype(np.float32)
                        pw = np.eye(4, dtype=np.float32); pw[:3, 3] = pivot
                        rest = np.eye(4, dtype=np.float32) if have_sbi else w
                        c0 = ((vv.min(0) + vv.max(0)) / 2).tolist(); e = ((vv.max(0) - vv.min(0)) / 2).tolist()
                        mid = add_mesh(mesh_blob(mat_key(info), vv, u, inv.astype(np.uint32), cols))
                        if si != first_sub and dyn_parent is not None:
                            props.append({'follow': dyn_parent, 'name': props[dyn_parent]['name'], 'item': '', 'tag': 0,
                                          'world': pw, 'mesh': mid, 'rest': rest,
                                          'half': [max(e[0], .02), max(e[1], .02), max(e[2], .02)], 'center': c0})
                            continue
                        dyn_parent = len(props)
                        props.append({
                            'house': gid in house_group, 'glass': glass_role.get(gid), 'rest': rest,
                            'name': ('G%d' % glass_role[gid][1]) if gid in glass_role else (item_root(gid) or name),
                            'item': '' if gid in glass_role else item_root(gid), 'tag': tag, 'world': pw, 'mesh': mid,
                            'half': [max(e[0], .02), max(e[1], .02), max(e[2], .02)], 'center': c0,
                        })
                        continue
                    mi = si - first_sub
                    mp = mats[mi] if mi < len(mats) else (mats[-1] if mats else None)
                    if mp is None:
                        continue
                    try:
                        info = material_info(mp.read())
                    except Exception:
                        info = (0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0.0)
                    add_geom(mat_key(info), vw, u, inv.astype(np.uint32), cols)
                    if g.get('m_Layer', 0) in (3,) or g.get('m_Layer', 0) >= 6 and g.get('m_Layer', 0) != 8:
                        mm = mm_batches.setdefault(mat_key(info), [[], [], [], [], 0])
                        base = mm[4]
                        mm[0].append(vw); mm[1].append(u); mm[2].append(cols)
                        mm[3].append(inv.astype(np.uint32).reshape(-1) + base); mm[4] = base + len(vw)
            del verts

        # SpriteRenderers in the world (the road is 157 flat Road_2lane_dark02 sprites;
        # the minimap icons on layer 8 are drawn by the minimap itself). Sprites-Default is
        # unlit, tinted by the renderer colour, double sided.
        spr_c = next((c for c in cl if c.type.name == 'SpriteRenderer'), None)
        if spr_c is not None and g.get('m_Layer', 0) != 8:
            try:
                sd = spr_c.read_typetree()
                if sd.get('m_Enabled', 1) and sd['m_Sprite']['m_PathID']:
                    so = spr_c.read().m_Sprite.read()
                    st = so.object_reader.read_typetree()
                    tex_o = so.m_RD.texture.read()
                    tr_ = st['m_RD']['textureRect']; rc = st['m_Rect']; pv = st['m_Pivot']; ppu = st['m_PixelsToUnits'] or 100.0
                    tw, th = tex_o.m_Width, tex_o.m_Height
                    sw_, sh_ = rc['width'] / ppu, rc['height'] / ppu
                    x0, y0 = -pv['x'] * sw_, -pv['y'] * sh_
                    quad = np.array([[x0, y0, 0], [x0 + sw_, y0, 0], [x0 + sw_, y0 + sh_, 0], [x0, y0 + sh_, 0]], np.float32)
                    u0, v0 = tr_['x'] / tw, tr_['y'] / th
                    u1, v1 = (tr_['x'] + tr_['width']) / tw, (tr_['y'] + tr_['height']) / th
                    uv = np.array([[u0, v0], [u1, v0], [u1, v1], [u0, v1]], np.float32)
                    vw = (w @ np.concatenate([quad, np.ones((4, 1), np.float32)], 1).T).T[:, :3].astype(np.float32)
                    texid = export_texture(tex_o) or 0
                    img_a = tex_o.image.getchannel('A') if tex_o.image.mode == 'RGBA' else None
                    transparent = img_a is not None and img_a.getextrema()[0] < 250
                    cc = sd.get('m_Color', {'r': 1, 'g': 1, 'b': 1, 'a': 1})
                    info = (texid, round(cc['r'], 3), round(cc['g'], 3), round(cc['b'], 3), round(cc['a'], 3),
                            1.0, 1.0, 0.0, 0.0, 2 if transparent else 0, 0.0)
                    cols = np.ones((4, 4), np.float32)   # unlit: light 1, no switch-light lobe
                    # both faces (Sprites-Default has Cull Off)
                    add_geom(mat_key(info), vw, uv, np.array([0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2], np.uint32), cols)
                    n_sprites[0] += 1
            except Exception as ex:
                print('   sprite renderer fail', name, ex)

        # colliders (static blockers)
        for c in cl:
            if c.type.name not in ('BoxCollider', 'MeshCollider', 'SphereCollider', 'CapsuleCollider'):
                continue
            if is_dyn or tag in (TAG_DOOR, TAG_TAKE, TAG_NEIGHBOR, TAG_LOCK):
                continue
            # the truck (Car / Car1) and the Wires box are solid in the original; only the
            # player, the neighbour and the walk-in pickups stay out of the static set
            if name in ('Key', 'Scissors', 'MoneyLoot', 'FPSController', 'NeighborNew'):
                continue
            try:
                cd = c.read_typetree()
                if cd.get('m_IsTrigger', False) or not cd.get('m_Enabled', 1):
                    continue
                if c.type.name == 'MeshCollider':
                    # the real collision triangles (what the CharacterController walks on)
                    try:
                        cmesh = c.read().m_Mesh.read()
                        (cv, _cn, _cu, ci), _cs, _ca = mesh_geometry_cached(cmesh)
                        if cv is not None and len(cv) and len(ci):
                            cvw = (w @ np.concatenate([cv, np.ones((len(cv), 1), np.float32)], 1).T).T[:, :3]
                            col_v.append(cvw.astype(np.float32)); col_i.append(np.asarray(ci, np.uint32).reshape(-1) + col_n[0])
                            col_n[0] += len(cvw)
                    except Exception:
                        pass
                if c.type.name == 'BoxCollider':
                    ctr, sz = cd['m_Center'], cd['m_Size']
                    hx, hy, hz = sz['x'] / 2, sz['y'] / 2, sz['z'] / 2
                    pts = np.array([[x, y, z, 1] for x in (-hx, hx) for y in (-hy, hy) for z in (-hz, hz)], np.float32)
                    pts[:, 0] += ctr['x']; pts[:, 1] += ctr['y']; pts[:, 2] += ctr['z']
                    wp = (w @ pts.T).T[:, :3]
                elif c.type.name == 'MeshCollider':
                    # the collider's own mesh (not the renderer's, which may be the
                    # scene-wide static batch)
                    try:
                        cm = c.read().m_Mesh.read()
                    except Exception:
                        continue
                    c0, e = aabb_parts(cm.m_LocalAABB)
                    pts = np.array([[x, y, z, 1] for x in (c0[0] - e[0], c0[0] + e[0])
                                    for y in (c0[1] - e[1], c0[1] + e[1])
                                    for z in (c0[2] - e[2], c0[2] + e[2])], np.float32)
                    wp = (w @ pts.T).T[:, :3]
                else:
                    r = float(cd.get('m_Radius', 0.5) or 0.5)
                    ctr = cd.get('m_Center', {'x': 0, 'y': 0, 'z': 0})
                    pts = np.array([[x, y, z, 1] for x in (-r, r) for y in (-1, 1) for z in (-r, r)], np.float32)
                    pts[:, 0] += ctr['x']; pts[:, 1] += ctr['y']; pts[:, 2] += ctr['z']
                    wp = (w @ pts.T).T[:, :3]
                if c.type.name != 'MeshCollider':
                    # oriented box (sphere / capsule approximated) as 12 triangles
                    col_v.append(wp.astype(np.float32))
                    col_i.append(np.array([(0, 1, 3), (0, 3, 2), (4, 6, 7), (4, 7, 5), (0, 4, 5), (0, 5, 1), (2, 3, 7), (2, 7, 6), (0, 2, 6), (0, 6, 4), (1, 5, 7), (1, 7, 3)], np.uint32).reshape(-1) + col_n[0])
                    col_n[0] += 8
                mn, mx = wp.min(0), wp.max(0)
                if (mx - mn).max() > 0.03 and (mx - mn).min() >= 0:
                    colliders.append(([float(v) for v in mn], [float(v) for v in mx]))
            except Exception:
                pass

        p = w[:3, 3]
        Z3 = [0.0, 0.0, 0.0]
        for key, mt in (('Key', 7), ('Scissors', 8), ('MoneyLoot', 9)):
            if name == key:
                markers.append((mt, [float(v) for v in p], 2.0, name, Z3))
        # FirstPersonController.OnTriggerEnter matches other.name == "Home" / "Car" /
        # "Wires": export those colliders' world boxes (only the levels whose object
        # carries exactly that name have the action; elsewhere it is "Car1"/"Wires1")
        for key, mt in (('Home', 4), ('Car', 5), ('Wires', 6)):
            if name != key:
                continue
            bc = next((c for c in cl if c.type.name == 'BoxCollider'), None)
            if bc is None:
                markers.append((mt, [float(v) for v in p], 2.0, name, [2.0, 1.0, 2.0]))
                continue
            cd = bc.read_typetree()
            ctr, sz = cd['m_Center'], cd['m_Size']
            hx, hy, hz = sz['x'] / 2, sz['y'] / 2, sz['z'] / 2
            pts = np.array([[x, y, z, 1] for x in (-hx, hx) for y in (-hy, hy) for z in (-hz, hz)], np.float32)
            pts[:, 0] += ctr['x']; pts[:, 1] += ctr['y']; pts[:, 2] += ctr['z']
            wp = (w @ pts.T).T[:, :3]
            mn, mx = wp.min(0), wp.max(0)
            markers.append((mt, [float(v) for v in (mn + mx) / 2], 0.0, name, [float(v) for v in (mx - mn) / 2]))
        if gid in glass_bg:
            bc = next((c for c in cl if c.type.name == 'BoxCollider'), None)
            if bc is not None:
                cd = bc.read_typetree()
                ctr, sz = cd['m_Center'], cd['m_Size']
                hx, hy, hz = sz['x'] / 2, sz['y'] / 2, sz['z'] / 2
                pts = np.array([[x, y, z, 1] for x in (-hx, hx) for y in (-hy, hy) for z in (-hz, hz)], np.float32)
                pts[:, 0] += ctr['x']; pts[:, 1] += ctr['y']; pts[:, 2] += ctr['z']
                wp = (w @ pts.T).T[:, :3]
                mn, mx = wp.min(0), wp.max(0)
                markers.append((10, [float(v) for v in (mn + mx) / 2], 0.0, 'G%d' % glass_bg.index(gid),
                                [float(v) for v in (mx - mn) / 2]))
        if 'MovePoint' in name and 'HoldPlace' not in name:
            markers.append((3, [float(v) for v in p], 1.0, name, [0.0, 0.0, 0.0]))
        if name == 'NeighborNew':
            markers.append((2, [float(v) for v in p], 1.0, name, [0.0, 0.0, 0.0]))
        if name == 'FPSController':
            markers.append((1, [float(v) for v in p], 1.0, name, [0.0, 0.0, 0.0]))

    total_v = sum(len(x) for b in batches.values() for x in b[0])
    print('    verts', total_v, 'batches', len(batches), flush=True)
    scene_meshes = []
    for matid, b in batches.items():
        if not b[0]:
            continue
        v = np.concatenate(b[0]); u = np.concatenate(b[1]); c = np.concatenate(b[2]); i = np.concatenate(b[3])
        scene_meshes.append(add_mesh(mesh_blob(matid, v, u, i, c)))
        del v, u, c, i
    batches.clear()
    for matid, b in mm_batches.items():
        v = np.concatenate(b[0]); u = np.concatenate(b[1]); c = np.concatenate(b[2]); i = np.concatenate(b[3])
        props.append({'minimap': True, 'name': 'minimap', 'item': '', 'tag': 0, 'world': np.eye(4, dtype=np.float32),
                      'mesh': add_mesh(mesh_blob(matid, v, u, i, c)),
                      'half': ((v.max(0) - v.min(0)) / 2).tolist(), 'center': ((v.max(0) + v.min(0)) / 2).tolist()})
    print('    sprites', n_sprites[0], flush=True)
    print('    minimap meshes', len(mm_batches), flush=True)
    if col_v:
        v = np.concatenate(col_v); i = np.concatenate(col_i)
        props.append({'collision': True, 'name': 'collision', 'item': '', 'tag': 0, 'world': np.eye(4, dtype=np.float32),
                      'mesh': add_mesh(mesh_blob(0, v, np.zeros((len(v), 2), np.float32), i)),
                      'half': ((v.max(0) - v.min(0)) / 2).tolist(), 'center': ((v.max(0) + v.min(0)) / 2).tolist()})
        print('    collision tris', len(i) // 3, flush=True)
    mm_batches.clear()
    gc.collect()

    if switch_light is not None:
        # the switchable sun travels as a light of type 100
        sl = switch_light
        lights = lights + [(sl[0], sl[1], sl[2], sl[3], sl[4], 100)]
    return {'level': level_no, 'meshes': scene_meshes, 'props': props, 'colliders': colliders,
            'markers': markers, 'lights': lights, 'ambient': [float(x) for x in ambient], 'fog': fog}

scenes = []
for sf, lv in SCENES:
    scenes.append(build_scene(sf, lv))
    gc.collect()

# --------------------------------------------------------------- skybox
# every gameplay scene uses the same 6-sided "sky,startday" material (Skybox/6 Sided,
# _Tint 0.5 = neutral, _Exposure 1). Exported as sky_front/back/left/right/up/down.
print('Skybox...', flush=True)
_sky_done = False
for o in env.objects:
    if _sky_done or o.type.name != 'RenderSettings' or fname_of(o) != SCENES[0][0]:
        continue
    try:
        mat = o.read().m_SkyboxMaterial.read()
        for k, v in mat.m_SavedProperties.m_TexEnvs:
            side = {'_FrontTex': 'front', '_BackTex': 'back', '_LeftTex': 'left',
                    '_RightTex': 'right', '_UpTex': 'up', '_DownTex': 'down'}.get(k)
            if side and v.m_Texture:
                tid = export_texture(v.m_Texture.read())
                if tid:
                    t = tex_meta[tid - 1]
                    tex_meta[tid - 1] = (t[0], t[1], t[2], t[3], t[4], 'sky_' + side)
                    print('  sky', side, t[1], t[2], flush=True)
        _sky_done = True
    except Exception as e:
        print('  skybox failed', e, flush=True)

print('textures:', len(tex_meta), 'materials:', len(_materials),
      'meshes:', len(mesh_meta), 'colliders:', sum(len(s['colliders']) for s in scenes), flush=True)

# --------------------------------------------------------------- audio
print('Exporting audio...', flush=True)
for o in env.objects:
    if o.type.name != 'AudioClip':
        continue
    try:
        d = o.read()
        samples = d.samples
        if not samples:
            continue
        sd = list(samples.values())[0]
        raw = sd[1] if isinstance(sd, tuple) else sd
        ch = getattr(d, 'm_Channels', 1) or 1
        freq = getattr(d, 'm_Frequency', 44100) or 44100
        if raw[:4] == b'RIFF' and raw[8:12] == b'WAVE':
            # a decoded WAV: take the format from its header and only the data chunk
            q = 12
            while q + 8 <= len(raw):
                cid = raw[q:q + 4]; csz = struct.unpack_from('<I', raw, q + 4)[0]
                if cid == b'fmt ':
                    _fmt, ch, freq, _br, _ba, bits = struct.unpack_from('<HHIIHH', raw, q + 8)
                elif cid == b'data':
                    raw = raw[q + 8:q + 8 + csz]
                    break
                q += 8 + csz + (csz & 1)
        if d.m_Name == 'СкрипДвери' and len(raw) >= 2 * ch:
            # the door creak starts after 0.78 s of silence, i.e. after the door has
            # finished opening (0.55 s): start it 30 ms before the sound
            a16 = np.frombuffer(raw[:len(raw) // (2 * ch) * 2 * ch], np.int16).reshape(-1, ch)
            on = int(np.argmax(np.abs(a16).max(1) > 650))
            on = max(0, on - int(freq * 0.03))
            raw = raw[on * 2 * ch:]
        off = audio_file.seek(0, 2)
        audio_file.write(raw)
        audio_meta.append((d.m_Name, ch, freq, len(raw) // (2 * ch), off, len(raw)))
        print('  ', d.m_Name, len(raw), flush=True)
    except Exception as e:
        print('  audio fail', e, flush=True)

# simple synthesised sfx (the apk only ships music/voice/footsteps)
def sfx(freq, dur, kind, vol=0.3, rate=44100, seed=7):
    n = int(dur * rate)
    t = np.arange(n) / rate
    if kind == 'sine':
        s = np.sin(2 * np.pi * freq * t); env = np.exp(-t * 4)
    elif kind == 'noise':
        s = np.random.RandomState(seed).randn(n) * .5; env = np.exp(-t * 7)
    else:
        s = np.sin(2 * np.pi * freq * t) * (t < 0.12); env = np.exp(-t * 3)
    return (np.clip(s * env * vol, -1, 1) * 32767).astype(np.int16).tobytes()

for nm, fr, du, kd, vol in (('door', 180, .45, 'sine', .5), ('glass', 1200, .5, 'noise', .6),
                            ('step', 90, .12, 'noise', .35), ('throw', 300, .18, 'sine', .4),
                            ('pickup', 700, .16, 'sine', .4), ('win', 880, .7, 'sine', .45),
                            ('lose', 90, 1.0, 'sine', .55), ('hit', 150, .25, 'noise', .5)):
    raw = sfx(fr, du, kd, vol)
    off = audio_file.seek(0, 2)
    audio_file.write(raw)
    audio_meta.append((nm, 1, 44100, len(raw) // 2, off, len(raw)))

# --------------------------------------------------------------- font
print('Font atlas...', flush=True)
FONT_SIZE = 26
font = None
for p in ('C:/Windows/Fonts/arialbd.ttf', 'C:/Windows/Fonts/arial.ttf'):
    try:
        font = ImageFont.truetype(p, FONT_SIZE); break
    except Exception:
        pass
if font is None:
    font = ImageFont.load_default()
chars = [chr(c) for c in range(32, 127)]
atlas = Image.new('RGBA', (1024, 512), (255, 255, 255, 0))
dr = ImageDraw.Draw(atlas)
x = y = 1
row_h = FONT_SIZE + 8
glyphs = {}
for ch in chars:
    bbox = dr.textbbox((0, 0), ch, font=font)
    gw = max(1, bbox[2] - bbox[0]); gh = max(1, bbox[3] - bbox[1])
    if x + gw + 3 >= 1024:
        x = 1; y += row_h
    if y + row_h >= 512:
        break
    # ink lands at y + bbox[1] .. y + bbox[3]; store the inked rect, not the pen origin
    dr.text((x - bbox[0], y - bbox[1]), ch, font=font, fill=(255, 255, 255, 255))
    glyphs[ch] = (x, y, gw, gh, bbox[0], bbox[1], y + FONT_SIZE)
    x += gw + 3
font_tex = len(tex_meta) + 1
off = tex_file.seek(0, 2)
tex_file.write(atlas.tobytes())
tex_meta.append((font_tex, 1024, 512, off, 1024 * 512 * 4, 'font'))

# --------------------------------------------------------------- write pak
print('Writing pak...', flush=True)
out = open(os.path.join(OUT_DIR, 'data.pak'), 'wb')
w = Writer()
w.raw(b'DRPK'); w.u32(4)
w.u32(len(tex_meta)); w.u32(len(_materials)); w.u32(len(mesh_meta))
w.u32(len(scenes)); w.u32(len(audio_meta)); w.u32(font_tex)
out.write(bytes(w.buf))

# textures
for tid, tw, th, o, sz, tname in tex_meta:
    hdr = Writer(); hdr.u32(tid); hdr.u16(tw); hdr.u16(th); hdr.u8(0); hdr.u8(0); hdr.u32(sz); hdr.name(tname, 40)
    out.write(bytes(hdr.buf))
    tex_file.seek(o); out.write(tex_file.read(sz))

# materials
mb = Writer()
for info in _materials:
    texid, r, g, b, a, tx, ty, ox, oy, cut, cutoff = info
    mb.u32(texid); mb.f32(r); mb.f32(g); mb.f32(b); mb.f32(a)
    mb.f32(tx); mb.f32(ty); mb.f32(ox); mb.f32(oy); mb.u32(cut); mb.f32(cutoff)
out.write(bytes(mb.buf))

# meshes
mesh_file.seek(0)
for mid, sz in mesh_meta:
    hdr = Writer(); hdr.u32(mid); hdr.u32(sz)
    out.write(bytes(hdr.buf))
    out.write(mesh_file.read(sz))

# scenes
for s in scenes:
    sw = Writer()
    sw.u32(s['level'])
    sp = next((m for m in s['markers'] if m[0] == 1), None)
    pos = sp[1] if sp else [0, 2, 0]
    sw.vec3([pos[0], pos[1] + 0.9, pos[2]])
    sw.f32(0.0)
    sw.vec3(s['ambient']); sw.vec3(s['fog'])
    sw.u32(len(s['meshes']))
    for mid in s['meshes']:
        sw.u32(mid)
    sw.u32(len(s['colliders']))
    for mn, mx in s['colliders']:
        sw.vec3(mn); sw.vec3(mx)
    sw.u32(len(s['props']))
    for p in s['props']:
        m = p['world']
        pos = m[:3, 3]
        sx = float(np.linalg.norm(m[:3, 0])); sy = float(np.linalg.norm(m[:3, 1])); sz = float(np.linalg.norm(m[:3, 2]))
        R = m[:3, :3].astype(np.float64)
        if sx > 1e-6: R[:, 0] /= sx
        if sy > 1e-6: R[:, 1] /= sy
        if sz > 1e-6: R[:, 2] /= sz
        tr = R[0, 0] + R[1, 1] + R[2, 2]
        if tr > 0:
            qw = math.sqrt(1 + tr) / 2; qx = (R[2, 1] - R[1, 2]) / (4 * qw)
            qy = (R[0, 2] - R[2, 0]) / (4 * qw); qz = (R[1, 0] - R[0, 1]) / (4 * qw)
        else:
            if R[0, 0] > R[1, 1] and R[0, 0] > R[2, 2]:
                qx = math.sqrt(max(0, 1 + R[0, 0] - R[1, 1] - R[2, 2])) / 2
                qw = (R[2, 1] - R[1, 2]) / (4 * (qx or 1e-9)); qy = (R[0, 1] + R[1, 0]) / (4 * (qx or 1e-9)); qz = (R[0, 2] + R[2, 0]) / (4 * (qx or 1e-9))
            elif R[1, 1] > R[2, 2]:
                qy = math.sqrt(max(0, 1 + R[1, 1] - R[0, 0] - R[2, 2])) / 2
                qw = (R[0, 2] - R[2, 0]) / (4 * (qy or 1e-9)); qx = (R[0, 1] + R[1, 0]) / (4 * (qy or 1e-9)); qz = (R[1, 2] + R[2, 1]) / (4 * (qy or 1e-9))
            else:
                qz = math.sqrt(max(0, 1 + R[2, 2] - R[0, 0] - R[1, 1])) / 2
                qw = (R[1, 0] - R[0, 1]) / (4 * (qz or 1e-9)); qx = (R[0, 2] + R[2, 0]) / (4 * (qz or 1e-9)); qy = (R[1, 2] + R[2, 1]) / (4 * (qz or 1e-9))
        gl = p.get('glass')
        typ = 13 if p.get('follow') is not None else 12 if p.get('collision') else 11 if p.get('minimap') else (8 if gl[0] == 'solid' else 9) if gl else 6 if p.get('house') else 5 if p.get('item') else (1 if p['tag'] == TAG_DOOR else (2 if p['tag'] == TAG_TAKE else (4 if p['tag'] == TAG_LOCK else 3)))
        sw.u32(typ)
        sw.name(p['name'], 40)
        sw.vec3(pos); sw.quat((qx, qy, qz, qw)); sw.vec3((sx, sy, sz))
        sw.u32(p['mesh'])
        sw.vec3(p['half']); sw.vec3(p['center'])
        # reserved floats: the original world rotation (the mesh is baked with it)
        rm = p.get('rest', np.eye(4))[:3, :3].astype(np.float64)
        for k in range(3):
            n = np.linalg.norm(rm[:, k])
            if n > 1e-9: rm[:, k] /= n
        u_, _, vt_ = np.linalg.svd(rm); rm = u_ @ vt_          # nearest pure rotation
        if np.linalg.det(rm) < 0: rm[:, 2] *= -1
        tr = rm[0, 0] + rm[1, 1] + rm[2, 2]
        if tr > 0:
            rw_ = math.sqrt(1 + tr) / 2; rx_ = (rm[2, 1] - rm[1, 2]) / (4 * rw_)
            ry_ = (rm[0, 2] - rm[2, 0]) / (4 * rw_); rz_ = (rm[1, 0] - rm[0, 1]) / (4 * rw_)
        elif rm[0, 0] > rm[1, 1] and rm[0, 0] > rm[2, 2]:
            rx_ = math.sqrt(max(0, 1 + rm[0, 0] - rm[1, 1] - rm[2, 2])) / 2
            rw_ = (rm[2, 1] - rm[1, 2]) / (4 * rx_); ry_ = (rm[0, 1] + rm[1, 0]) / (4 * rx_); rz_ = (rm[0, 2] + rm[2, 0]) / (4 * rx_)
        elif rm[1, 1] > rm[2, 2]:
            ry_ = math.sqrt(max(0, 1 + rm[1, 1] - rm[0, 0] - rm[2, 2])) / 2
            rw_ = (rm[0, 2] - rm[2, 0]) / (4 * ry_); rx_ = (rm[0, 1] + rm[1, 0]) / (4 * ry_); rz_ = (rm[1, 2] + rm[2, 1]) / (4 * ry_)
        else:
            rz_ = math.sqrt(max(0, 1 + rm[2, 2] - rm[0, 0] - rm[1, 1])) / 2
            rw_ = (rm[1, 0] - rm[0, 1]) / (4 * rz_); rx_ = (rm[0, 2] + rm[2, 0]) / (4 * rz_); ry_ = (rm[1, 2] + rm[2, 1]) / (4 * rz_)
        sw.f32(rx_); sw.f32(ry_); sw.f32(rz_); sw.f32(rw_); sw.u32(p['follow'] if p.get('follow') is not None else 0xffffffff)
    sw.u32(len(s['markers']))
    for mt, mp, rad, nm, half in s['markers']:
        sw.u32(mt); sw.vec3(mp); sw.f32(rad); sw.name(nm, 40); sw.vec3(half)
    sw.u32(len(s['lights']))
    for lp, ld, lc, li, lr, lt in s['lights']:
        sw.vec3(lp); sw.vec3(ld); sw.vec3(lc); sw.f32(li); sw.f32(lr); sw.u32(lt)
    out.write(bytes(sw.buf))

# audio
audio_file.seek(0)
for name, ch, freq, frames, o, sz in audio_meta:
    ab = Writer(); ab.name(name, 32); ab.u32(ch); ab.u32(freq); ab.u32(frames)
    out.write(bytes(ab.buf))
    out.write(audio_file.read(sz))

# font
fb = Writer(); fb.u32(font_tex); fb.u32(len(glyphs))
for ch, (gx, gy, gw, gh, bx, by, base) in glyphs.items():
    fb.u32(ord(ch)); fb.u32(gx); fb.u32(gy); fb.u32(gw); fb.u32(gh); fb.f32(bx); fb.f32(by); fb.f32(base)
out.write(bytes(fb.buf))
out.close()

print('wrote', os.path.join(OUT_DIR, 'data.pak'),
      round(os.path.getsize(os.path.join(OUT_DIR, 'data.pak')) / 1e6, 2), 'MB')
print('summary: textures', len(tex_meta), 'materials', len(_materials), 'meshes', len(mesh_meta),
      'audio', len(audio_meta))
for s in scenes:
    print('  level', s['level'], 'meshes', len(s['meshes']), 'props', len(s['props']),
          'colliders', len(s['colliders']), 'markers', [m[3] for m in s['markers']][:12])
