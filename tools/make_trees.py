"""Animated trees (legacy Animation + SkinnedMeshRenderer) -> romfs/trees.pak.

The level trees are 2-bone skinned meshes swayed by a legacy Animation clip
(TreeSmall_Anim, wrap Loop). For each tree type: skinned mesh (mesh space), its
textures (alpha cutout), bind poses, bone hierarchy and the clip sampled at 30 fps
(local TRS per bone). Per level: the tree instances (root world matrix).

DRTR v1 layout (little endian):
  u32 nTypes; per type:
    u32 nv, verts nv * 16 f32 (pos3 nrm3 uv2 boneIdx4 weight4)
    u32 nSub; per sub: u32 ni, idx ni*u32, tex (u32 w, u32 h, w*h*4 RGBA), f32 r g b a, f32 cutoff
    u32 nb; per bone: i32 parent (-1 = animation root), f32 bind[16] (column-major)
    u32 nf, f32 fps; per frame per bone: f32 t3 q4 s3
  u32 nLevels; per level: u32 level, u32 nInst; per inst: u32 type, f32 world[16] (column-major)
"""
import io, os, sys, math, struct, collections
import numpy as np
import UnityPy
from UnityPy.helpers.MeshHelper import MeshHandler
from PIL import Image

sys.stdout.reconfigure(encoding='utf-8')
OUT = '../game/romfs/trees.pak'
FPS = 30.0
TEX_MAX = 512
SCENES = [('level3', 1), ('level4', 2), ('level5', 3), ('level6', 4),
          ('level7', 5), ('level8', 6), ('level9', 7)]

env = UnityPy.load('merged')
by_file = collections.defaultdict(dict)
for o in env.objects:
    by_file[o.assets_file.name][o.path_id] = o


def q2m(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def trs(t, q, s):
    m = np.eye(4)
    m[:3, :3] = q2m(q) @ np.diag(s)
    m[:3, 3] = t
    return m


def hermite(keys, t, n):
    """Unity AnimationCurve evaluation (Hermite with in/out slopes) for n components."""
    if not keys:
        return None
    comp = ['x', 'y', 'z', 'w'][:n]
    def vec(k, f):
        v = k[f]
        return np.array([v[c] for c in comp], np.float64) if isinstance(v, dict) else np.array([v], np.float64)
    if t <= keys[0]['time']:
        return vec(keys[0], 'value')
    if t >= keys[-1]['time']:
        return vec(keys[-1], 'value')
    for a, b in zip(keys, keys[1:]):
        if a['time'] <= t <= b['time']:
            dt = b['time'] - a['time']
            if dt <= 1e-9:
                return vec(b, 'value')
            s = (t - a['time']) / dt
            p0, p1 = vec(a, 'value'), vec(b, 'value')
            m0, m1 = vec(a, 'outSlope') * dt, vec(b, 'inSlope') * dt
            out = np.empty_like(p0)
            for i in range(len(p0)):
                if not (math.isfinite(m0[i]) and math.isfinite(m1[i])):
                    out[i] = p0[i]           # stepped tangent
                    continue
                h00 = 2 * s ** 3 - 3 * s ** 2 + 1; h10 = s ** 3 - 2 * s ** 2 + s
                h01 = -2 * s ** 3 + 3 * s ** 2; h11 = s ** 3 - s ** 2
                out[i] = h00 * p0[i] + h10 * m0[i] + h01 * p1[i] + h11 * m1[i]
            return out
    return vec(keys[-1], 'value')


def tex_rgba(tex_obj):
    img = tex_obj.read().image.convert('RGBA')
    if img.width > TEX_MAX or img.height > TEX_MAX:
        f = TEX_MAX / max(img.width, img.height)
        img = img.resize((max(1, int(img.width * f)), max(1, int(img.height * f))), Image.LANCZOS)
    # power of two for mipmaps
    pw = 1 << (img.width - 1).bit_length(); ph = 1 << (img.height - 1).bit_length()
    if (pw, ph) != img.size:
        img = img.resize((pw, ph), Image.LANCZOS)
    return img


types = []              # exported tree types
type_index = {}         # (mesh file, mesh pid, clip pid) -> type id
levels = []

for scene, level in SCENES:
    objs = by_file[scene]
    gos = {pid: o.read_typetree() for pid, o in objs.items() if o.type.name == 'GameObject'}
    trs_ = {pid: o.read_typetree() for pid, o in objs.items() if o.type.name == 'Transform'}
    go2tr = {t['m_GameObject']['m_PathID']: pid for pid, t in trs_.items()}
    comps = collections.defaultdict(list)
    for pid, o in objs.items():
        if o.type.name in ('GameObject', 'Transform', 'MonoBehaviour'):
            continue
        try:
            comps[o.read_typetree()['m_GameObject']['m_PathID']].append(o)
        except Exception:
            pass

    def local_of(tid):
        t = trs_[tid]; p, r, s = t['m_LocalPosition'], t['m_LocalRotation'], t['m_LocalScale']
        return trs((p['x'], p['y'], p['z']), (r['x'], r['y'], r['z'], r['w']), (s['x'], s['y'], s['z']))

    def world_of(tid, depth=0):
        if tid not in trs_ or depth > 64:
            return np.eye(4)
        f = trs_[tid]['m_Father']['m_PathID']
        m = local_of(tid)
        return world_of(f, depth + 1) @ m if f in trs_ else m

    def active(gid):
        while gid in gos:
            if not gos[gid].get('m_IsActive', 1):
                return False
            f = trs_[go2tr[gid]]['m_Father']['m_PathID']
            gid = trs_[f]['m_GameObject']['m_PathID'] if f in trs_ else 0
        return True

    insts = []
    for gid, g in gos.items():
        anim = next((c for c in comps[gid] if c.type.name == 'Animation'), None)
        if anim is None or not active(gid):
            continue
        a = anim.read()
        try:
            clip = a.m_Animation.read()
        except Exception:
            continue
        # the skinned mesh somewhere under this root
        root_tid = go2tr[gid]
        stack = [root_tid]; smr_o = None; smr_gid = None
        while stack:
            tid = stack.pop()
            g2 = trs_[tid]['m_GameObject']['m_PathID']
            s_ = next((c for c in comps[g2] if c.type.name == 'SkinnedMeshRenderer'), None)
            if s_ is not None and active(g2):
                smr_o, smr_gid = s_, g2
                break
            stack.extend(c['m_PathID'] for c in trs_[tid].get('m_Children', []) if c['m_PathID'] in trs_)
        if smr_o is None:
            continue
        smr = smr_o.read(); smr_t = smr_o.read_typetree()
        mesh = smr.m_Mesh.read()
        key = (mesh.object_reader.assets_file.name, mesh.object_reader.path_id, clip.object_reader.path_id)
        if key not in type_index:
            h = MeshHandler(mesh); h.process()
            V = np.array(h.m_Vertices, np.float32)[:, :3]
            N = np.array(h.m_Normals, np.float32)[:, :3] if h.m_Normals else np.tile([0, 1, 0], (len(V), 1)).astype(np.float32)
            UV = np.array(h.m_UV0, np.float32)[:, :2] if h.m_UV0 else np.zeros((len(V), 2), np.float32)
            BI = np.array(h.m_BoneIndices, np.float32) if h.m_BoneIndices else np.zeros((len(V), 4), np.float32)
            BW = np.array(h.m_BoneWeights, np.float32) if h.m_BoneWeights else np.tile([1, 0, 0, 0], (len(V), 1)).astype(np.float32)
            idx = np.array(h.m_IndexBuffer, np.uint32)
            bind = [np.array([[bp['e%d%d' % (r, c)] for c in range(4)] for r in range(4)], np.float64)
                    for bp in mesh.object_reader.read_typetree()['m_BindPose']]
            # bones: transforms named by m_Bones, parents among them, paths relative to the root
            bone_tids = [b['m_PathID'] for b in smr_t['m_Bones']]
            def rel_path(tid):
                names = []
                while tid in trs_ and tid != root_tid:
                    names.append(gos[trs_[tid]['m_GameObject']['m_PathID']]['m_Name'])
                    tid = trs_[tid]['m_Father']['m_PathID']
                return '/'.join(reversed(names))
            paths = [rel_path(t) for t in bone_tids]
            parents = []
            for t in bone_tids:
                f = trs_[t]['m_Father']['m_PathID']
                parents.append(bone_tids.index(f) if f in bone_tids else -1)
            # legacy clip curves by path
            ct = clip.object_reader.read_typetree()
            curves = collections.defaultdict(dict)
            for kind, n in (('m_PositionCurves', 3), ('m_RotationCurves', 4), ('m_ScaleCurves', 3)):
                for c in ct.get(kind, []):
                    curves[c['path']][kind] = (c['curve']['m_Curve'], n)
            # legacy clip: its length is the last key time over all curves
            length = max((k['time'] for cs in curves.values() for (keys, _) in cs.values() for k in keys), default=1.0)
            nf = max(2, int(round(length * FPS)) + 1)
            frames = np.zeros((nf, len(bone_tids), 10), np.float32)
            for bi_, t in enumerate(bone_tids):
                rest = trs_[t]
                rp, rr, rs = rest['m_LocalPosition'], rest['m_LocalRotation'], rest['m_LocalScale']
                cs = curves.get(paths[bi_], {})
                for f in range(nf):
                    tm = min(length, f / FPS)
                    p = hermite(cs['m_PositionCurves'][0], tm, 3) if 'm_PositionCurves' in cs else np.array([rp['x'], rp['y'], rp['z']])
                    q = hermite(cs['m_RotationCurves'][0], tm, 4) if 'm_RotationCurves' in cs else np.array([rr['x'], rr['y'], rr['z'], rr['w']])
                    s = hermite(cs['m_ScaleCurves'][0], tm, 3) if 'm_ScaleCurves' in cs else np.array([rs['x'], rs['y'], rs['z']])
                    q = q / (np.linalg.norm(q) or 1.0)
                    frames[f, bi_] = np.concatenate([p, q, s])
            # sub-meshes with their materials (texture, colour, cutoff)
            subs = []
            mats = list(smr.m_Materials or [])
            sm_list = mesh.m_SubMeshes
            for si, sm in enumerate(sm_list):
                first = sm.firstByte // 4 if getattr(mesh, 'm_IndexFormat', 1) == 1 else sm.firstByte // 2
                sidx = idx[first:first + sm.indexCount]
                mt = mats[min(si, len(mats) - 1)].read() if mats else None
                img = Image.new('RGBA', (1, 1), (255, 255, 255, 255)); col = (1, 1, 1, 1); cutoff = 0.0
                if mt is not None:
                    tt = mt.object_reader.read_typetree()
                    for k, v in mt.m_SavedProperties.m_TexEnvs:
                        if k == '_MainTex' and v.m_Texture and v.m_Texture.path_id:
                            img = tex_rgba(v.m_Texture)
                    c = dict(tt['m_SavedProperties']['m_Colors']).get('_Color')
                    if c: col = (c['r'], c['g'], c['b'], c['a'])
                    fl = dict(tt['m_SavedProperties']['m_Floats'])
                    kw = tt.get('m_ShaderKeywords', '') or ''
                    if fl.get('_Mode', 0) == 1.0 or '_ALPHATEST_ON' in kw:
                        cutoff = float(fl.get('_Cutoff', 0.5))
                subs.append((sidx.astype(np.uint32), img, col, cutoff))
            verts = np.concatenate([V, N, UV, BI[:, :4], BW[:, :4]], 1).astype(np.float32)
            type_index[key] = len(types)
            types.append({'name': clip.m_Name, 'verts': verts, 'subs': subs, 'parents': parents,
                          'bind': bind, 'frames': frames})
            print('  type', len(types) - 1, clip.m_Name, 'verts', len(V), 'bones', len(bone_tids), 'subs', len(subs),
                  'frames', nf, 'paths', paths, flush=True)
        insts.append((type_index[key], world_of(root_tid)))
    levels.append((level, insts))
    print(scene, 'level', level, 'animated trees', len(insts), flush=True)

with open(OUT, 'wb') as f:
    f.write(b'DRTR' + struct.pack('<I', 1))
    f.write(struct.pack('<I', len(types)))
    for t in types:
        v = t['verts']
        f.write(struct.pack('<I', len(v))); f.write(v.tobytes())
        f.write(struct.pack('<I', len(t['subs'])))
        for sidx, img, col, cutoff in t['subs']:
            f.write(struct.pack('<I', len(sidx))); f.write(sidx.tobytes())
            f.write(struct.pack('<II', img.width, img.height)); f.write(img.tobytes())
            f.write(struct.pack('<5f', *col, cutoff))
        f.write(struct.pack('<I', len(t['parents'])))
        for p, b in zip(t['parents'], t['bind']):
            f.write(struct.pack('<i', p)); f.write(b.T.astype(np.float32).tobytes())   # column-major
        fr = t['frames']
        f.write(struct.pack('<If', fr.shape[0], FPS)); f.write(fr.tobytes())
    f.write(struct.pack('<I', len(levels)))
    for level, insts in levels:
        f.write(struct.pack('<II', level, len(insts)))
        for ti, w in insts:
            f.write(struct.pack('<I', ti)); f.write(w.T.astype(np.float32).tobytes())
print('wrote', OUT, os.path.getsize(OUT) // 1024, 'KB')
