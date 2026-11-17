"""Exports the neighbour (NeighborNew) as an animated character: romfs/char.pak.

- Skinned mesh of the T-Pose/123_001 SkinnedMeshRenderer (bind-space vertices,
  4 bone weights per vertex) and its texture.
- The 52 skin bones: rest local TRS (relative to the parent bone, root bones relative
  to the NeighborNew transform), bind poses, and for humanoid bones the Avatar's
  Mecanim axes (preQ, postQ, sign, limits) plus which muscle drives each DoF.
- The Animator's clips (idle, walk, run, mixamo.com) decoded from Mecanim's
  streamed / dense / constant curves and resampled at 30 fps into a fixed channel
  layout: RootT(3) RootQ(4) then the 95 muscles (55 body + 40 fingers).
- Per level: NeighborNew start pose and NeighborAI's serialized settings
  (seeDistance, attackDistance, ordered waypoints).

Muscle -> bone DoF mapping follows Mecanim's internal tables: per human bone the x
axis is the twist/roll, y the left-right / in-out (front-back for shoulder/arm),
z the front-back / down-up / stretch.

Format DRCH v1 (little endian): see write section at the bottom.
"""
import struct, math, sys, collections
import numpy as np
import UnityPy
from UnityPy.helpers.MeshHelper import MeshHandler
from PIL import Image

sys.stdout.reconfigure(encoding='utf-8')
SCENES = [('level3', 1), ('level4', 2), ('level5', 3), ('level6', 4),
          ('level7', 5), ('level8', 6), ('level9', 7)]
FPS = 30.0
OUT = '../game/romfs/char.pak'
TEX_MAX = 1024

env = UnityPy.load('merged')
by_file = collections.defaultdict(dict)
for o in env.objects:
    by_file[o.assets_file.name][o.path_id] = o


def resolve(af, fid, pid):
    if not pid:
        return None
    name = af.name if fid == 0 else af.externals[fid - 1].path.split('/')[-1]
    return by_file.get(name, {}).get(pid)


# ------------------------------------------------------------------ math (Unity, x y z w)
def qmul(a, b):
    ax, ay, az, aw = a; bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def qconj(q):
    return (-q[0], -q[1], -q[2], q[3])


def qrot(q, v):
    p = qmul(qmul(q, (v[0], v[1], v[2], 0.0)), qconj(q))
    return (p[0], p[1], p[2])


def trs(t, q, s):
    x, y, z, w = q
    r = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                  [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                  [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
    m = np.eye(4)
    m[:3, :3] = r * np.array([s[0], s[1], s[2]])[None, :]
    m[:3, 3] = t
    return m


def mat_to_trs(m):
    t = m[:3, 3].copy()
    sx, sy, sz = (np.linalg.norm(m[:3, i]) for i in range(3))
    r = m[:3, :3] / np.array([sx, sy, sz])[None, :]
    tr = r[0, 0] + r[1, 1] + r[2, 2]
    if tr > 0:
        s = math.sqrt(tr + 1.0) * 2
        q = ((r[2, 1] - r[1, 2]) / s, (r[0, 2] - r[2, 0]) / s, (r[1, 0] - r[0, 1]) / s, 0.25 * s)
    elif r[0, 0] > r[1, 1] and r[0, 0] > r[2, 2]:
        s = math.sqrt(1.0 + r[0, 0] - r[1, 1] - r[2, 2]) * 2
        q = (0.25 * s, (r[0, 1] + r[1, 0]) / s, (r[0, 2] + r[2, 0]) / s, (r[2, 1] - r[1, 2]) / s)
    elif r[1, 1] > r[2, 2]:
        s = math.sqrt(1.0 + r[1, 1] - r[0, 0] - r[2, 2]) * 2
        q = ((r[0, 1] + r[1, 0]) / s, 0.25 * s, (r[1, 2] + r[2, 1]) / s, (r[0, 2] - r[2, 0]) / s)
    else:
        s = math.sqrt(1.0 + r[2, 2] - r[0, 0] - r[1, 1]) * 2
        q = ((r[0, 2] + r[2, 0]) / s, (r[1, 2] + r[2, 1]) / s, 0.25 * s, (r[1, 0] - r[0, 1]) / s)
    return t, q, (sx, sy, sz)


def v3(d):
    return (d['x'], d['y'], d['z'])


def v4(d):
    return (d['x'], d['y'], d['z'], d['w'])


# ------------------------------------------------------------------ scene objects
def scene_maps(scene):
    objs = by_file[scene]
    gos, trs_ = {}, {}
    for pid, o in objs.items():
        if o.type.name == 'GameObject':
            gos[pid] = o.read_typetree()
        elif o.type.name == 'Transform':
            trs_[pid] = o.read_typetree()
    go2tr = {t['m_GameObject']['m_PathID']: pid for pid, t in trs_.items()}
    return objs, gos, trs_, go2tr


def world_of(trs_, tid):
    m = np.eye(4)
    chain = []
    while tid and tid in trs_:
        chain.append(trs_[tid]); tid = trs_[tid]['m_Father']['m_PathID']
    for t in reversed(chain):
        m = m @ trs(v3(t['m_LocalPosition']), v4(t['m_LocalRotation']), v3(t['m_LocalScale']))
    return m


# ------------------------------------------------------------------ human tables
# Mecanim internal human bone order (m_HumanBoneIndex)
HB = ['Hips', 'LeftUpperLeg', 'RightUpperLeg', 'LeftLowerLeg', 'RightLowerLeg', 'LeftFoot', 'RightFoot',
      'Spine', 'Chest', 'UpperChest', 'Neck', 'Head', 'LeftShoulder', 'RightShoulder', 'LeftUpperArm',
      'RightUpperArm', 'LeftLowerArm', 'RightLowerArm', 'LeftHand', 'RightHand', 'LeftToes', 'RightToes',
      'LeftEye', 'RightEye', 'Jaw']
# body muscles 0..54 in Mecanim order: (human bone, axis 0=x 1=y 2=z)
BODY_MUSCLES = []
for b in ('Spine', 'Chest', 'UpperChest'):
    BODY_MUSCLES += [(b, 2), (b, 1), (b, 0)]          # front-back, left-right, twist
for b in ('Neck', 'Head'):
    BODY_MUSCLES += [(b, 2), (b, 1), (b, 0)]          # nod, tilt, turn
BODY_MUSCLES += [('LeftEye', 2), ('LeftEye', 1), ('RightEye', 2), ('RightEye', 1), ('Jaw', 2), ('Jaw', 1)]
for side in ('Left', 'Right'):
    BODY_MUSCLES += [(side + 'UpperLeg', 2), (side + 'UpperLeg', 1), (side + 'UpperLeg', 0),
                     (side + 'LowerLeg', 2), (side + 'LowerLeg', 0),
                     (side + 'Foot', 2), (side + 'Foot', 1), (side + 'Toes', 2)]
for side in ('Left', 'Right'):
    BODY_MUSCLES += [(side + 'Shoulder', 2), (side + 'Shoulder', 1),
                     (side + 'UpperArm', 2), (side + 'UpperArm', 1), (side + 'UpperArm', 0),
                     (side + 'LowerArm', 2), (side + 'LowerArm', 0),
                     (side + 'Hand', 2), (side + 'Hand', 1)]
assert len(BODY_MUSCLES) == 55
ATTR_ROOT_T, ATTR_ROOT_Q, ATTR_MUSCLE, ATTR_FINGER = 7, 10, 42, 97
NCH = 7 + 95


# ------------------------------------------------------------------ clip decoding
def decode_clip(clip_obj):
    t = clip_obj.read_typetree()
    mc = t['m_MuscleClip']
    cl = mc['m_Clip']['data']
    start, stop = mc['m_StartTime'], mc['m_StopTime']
    binds = t['m_ClipBindingConstant']['genericBindings']
    # curves: streamed, then dense, then constant
    sc = cl['m_StreamedClip']
    raw = struct.pack('<%dI' % len(sc['data']), *sc['data'])
    keys = collections.defaultdict(list)       # curve -> [(time, c0..c3)]
    p = 0
    while p + 8 <= len(raw):
        time, n = struct.unpack_from('<fi', raw, p); p += 8
        for _ in range(n):
            idx, c0, c1, c2, c3 = struct.unpack_from('<i4f', raw, p); p += 20
            keys[idx].append((time, c0, c1, c2, c3))
    ns = sc['curveCount']
    dc = cl['m_DenseClip']
    nd, nf, rate, beg = dc['m_CurveCount'], dc['m_FrameCount'], dc['m_SampleRate'], dc['m_BeginTime']
    dense = np.array(dc['m_SampleArray'], np.float32).reshape(max(nf, 1), nd) if nd else None
    const = cl['m_ConstantClip']['data']

    def curve_value(ci, tm):
        if ci < ns:
            ks = keys.get(ci)
            if not ks:
                return 0.0
            k = ks[0]
            for kk in ks:
                if kk[0] <= tm:
                    k = kk
                else:
                    break
            dt = tm - k[0] if math.isfinite(k[0]) else 0.0
            return ((k[1] * dt + k[2]) * dt + k[3]) * dt + k[4]
        ci -= ns
        if ci < nd:
            f = (tm - beg) * rate
            f0 = int(max(0, min(nf - 1, math.floor(f)))); f1 = min(nf - 1, f0 + 1)
            a = max(0.0, min(1.0, f - f0))
            return float(dense[f0, ci] * (1 - a) + dense[f1, ci] * a)
        ci -= nd
        return float(const[ci]) if ci < len(const) else 0.0

    chan_of_curve = []
    for b in binds:
        a = b['attribute'] if (b['typeID'] == 95 and b['customType'] == 8) else -1
        if ATTR_ROOT_T <= a < ATTR_ROOT_T + 7:
            chan_of_curve.append(a - ATTR_ROOT_T)
        elif ATTR_MUSCLE <= a < ATTR_MUSCLE + 95:
            chan_of_curve.append(7 + (a - ATTR_MUSCLE))
        else:
            chan_of_curve.append(-1)
    length = max(stop - start, 1.0 / FPS)
    nfr = max(2, int(round(length * FPS)) + 1)
    out = np.zeros((nfr, NCH), np.float32)
    out[:, 6] = 1.0     # RootQ.w default
    for f in range(nfr):
        tm = start + min(length, f / FPS)
        for ci, ch in enumerate(chan_of_curve):
            if ch >= 0:
                out[f, ch] = curve_value(ci, tm)
    # Animator.applyRootMotion is off: the body keeps its height/sway but not the
    # clip's travel, and its mean heading is the character's facing.
    # Root-motion clips (Basic_Run_03_Root) travel metres per cycle: remove the travel
    # ramp (start -> end trend) first, then the mean, keeping only the in-place sway.
    # De-meaning alone left the run's body sliding +-1.5 m around the agent.
    # Unity root-motion extraction with every "Bake Into Pose" off (all the neighbour's
    # clips: m_LoopBlendOrientation / PositionXZ false) and applyRootMotion off:
    #  - Root Transform Rotation based on Body Orientation: the body's yaw goes to the
    #    root EVERY FRAME, so the body always faces the character's heading (no twist);
    #  - Root Transform Position XZ based on the centre of mass: the root follows the
    #    body, so the body never leaves the character's position (no sway / travel);
    #  - Position Y based on Original (keepOriginalPositionY): the root carries no
    #    vertical motion, so the body's bob stays in the pose.
    bake_rot = bool(mc.get('m_LoopBlendOrientation', 0))
    bake_xz = bool(mc.get('m_LoopBlendPositionXZ', 0))
    if not bake_xz:
        out[:, 0] = 0.0
        out[:, 2] = 0.0
    else:
        ramp = np.linspace(0.0, 1.0, nfr, dtype=np.float32)
        for ch in (0, 2):
            v = out[:, ch]
            v -= v[0] + (v[-1] - v[0]) * ramp
            v -= v.mean()
    for f in range(nfr):
        q = tuple(out[f, 3:7])
        fwd = qrot(q, (0, 0, 1))
        yaw = math.atan2(fwd[0], fwd[2])
        if bake_rot:
            continue
        unyaw = (0.0, math.sin(-yaw / 2), 0.0, math.cos(-yaw / 2))
        out[f, 3:7] = qmul(unyaw, q)
    if bake_rot:
        yaws = []
        for f in range(nfr):
            fwd = qrot(tuple(out[f, 3:7]), (0, 0, 1)); yaws.append(math.atan2(fwd[0], fwd[2]))
        mean_yaw = math.atan2(sum(math.sin(y) for y in yaws), sum(math.cos(y) for y in yaws))
        unyaw = (0.0, math.sin(-mean_yaw / 2), 0.0, math.cos(-mean_yaw / 2))
        for f in range(nfr):
            out[f, 3:7] = qmul(unyaw, tuple(out[f, 3:7]))
    return t['m_Name'], bool(mc.get('m_LoopTime', 0)), out


# ------------------------------------------------------------------ character from level3
objs, gos, trs_, go2tr = scene_maps('level3')
af = next(iter(objs.values())).assets_file
nb_gid = next(g for g, d in gos.items() if d['m_Name'] == 'NeighborNew')
nb_tid = go2tr[nb_gid]
nb_world = world_of(trs_, nb_tid)
nb_inv = np.linalg.inv(nb_world)

smr_o = None
anim_o = None
for pid, o in objs.items():
    if o.type.name == 'SkinnedMeshRenderer':
        t = o.read_typetree()
        if len(t['m_Bones']) >= 40:
            smr_o = o
    if o.type.name == 'Animator' and o.read_typetree()['m_GameObject']['m_PathID'] == nb_gid:
        anim_o = o
smr = smr_o.read()
smr_t = smr_o.read_typetree()
mesh = smr.m_Mesh.read()
h = MeshHandler(mesh); h.process()
V = np.array(h.m_Vertices, np.float32)[:, :3]
N = np.array(h.m_Normals, np.float32)[:, :3]
UV = np.array(h.m_UV0, np.float32)[:, :2]
BI = np.array(h.m_BoneIndices, np.int32)
BW = np.array(h.m_BoneWeights, np.float32)
IDX = np.array(h.m_IndexBuffer, np.uint32)
BIND = [np.array([[bp['e%d%d' % (r, c)] for c in range(4)] for r in range(4)], np.float64) for bp in mesh.object_reader.read_typetree()['m_BindPose']]
print('mesh', len(V), 'verts', len(IDX) // 3, 'tris', len(BIND), 'bones')

# texture of the character material
mat = smr.m_Materials[0].read()
tex_img = None
for k, v in mat.m_SavedProperties.m_TexEnvs:
    if k == '_MainTex' and v.m_Texture:
        tex_img = v.m_Texture.read().image.convert('RGBA')
if tex_img.width > TEX_MAX or tex_img.height > TEX_MAX:
    s = TEX_MAX / max(tex_img.size)
    nw, nh = int(tex_img.width * s), int(tex_img.height * s)
    rgb = tex_img.convert('RGB').resize((nw, nh), Image.LANCZOS)
    a = tex_img.getchannel('A').resize((nw, nh), Image.LANCZOS)
    tex_img = rgb.convert('RGBA'); tex_img.putalpha(a)
print('texture', tex_img.size)

# bones
bone_tids = [b['m_PathID'] for b in smr_t['m_Bones']]
bone_index = {tid: i for i, tid in enumerate(bone_tids)}
bones = []
for i, tid in enumerate(bone_tids):
    t = trs_[tid]
    name = gos[t['m_GameObject']['m_PathID']]['m_Name']
    par = t['m_Father']['m_PathID']
    if par in bone_index:
        local = trs(v3(t['m_LocalPosition']), v4(t['m_LocalRotation']), v3(t['m_LocalScale']))
        pi = bone_index[par]
    else:
        local = nb_inv @ world_of(trs_, tid)    # relative to the NeighborNew root
        pi = -1
    lt, lq, ls = mat_to_trs(local)
    bones.append({'name': name, 'parent': pi, 't': lt, 'q': lq, 's': ls, 'bind': BIND[i],
                  'axes': None, 'dof': [-1, -1, -1], 'human': ''})
# parents must come before children
order_ok = all(b['parent'] < i for i, b in enumerate(bones))
print('bones', len(bones), 'parent-first order', order_ok)

# avatar: human skeleton node -> bone, axes, muscles
avatar = anim_o.read().m_Avatar.read().object_reader.read_typetree()
A = avatar['m_Avatar']
hu = A['m_Human']['data']
hsk = hu['m_Skeleton']['data']
tos = dict(avatar['m_TOS'])
node_bone = []
for nid in hsk['m_ID']:
    path = tos.get(nid, '')
    nm = path.split('/')[-1] if path else ''
    node_bone.append(next((i for i, b in enumerate(bones) if b['name'] == nm), -1))
axes = hsk['m_AxesArray']
human_node = {}
for hbi, node in enumerate(hu['m_HumanBoneIndex']):
    if node >= 0:
        human_node[HB[hbi]] = node
hand_names = ['Thumb', 'Index', 'Middle', 'Ring', 'Little']
for side, key in (('Left', 'm_LeftHand'), ('Right', 'm_RightHand')):
    hb = hu[key]['data']['m_HandBoneIndex'] if 'data' in hu[key] else hu[key]['m_HandBoneIndex']
    for fi, fn in enumerate(hand_names):
        for ph in range(3):
            node = hb[fi * 3 + ph]
            if node >= 0:
                human_node[f'{side}{fn}{ph + 1}'] = node


def set_axes(bname, axis, muscle):
    node = human_node.get(bname)
    if node is None:
        return
    bi = node_bone[node]
    if bi < 0:
        return
    b = bones[bi]
    ai = hsk['m_Node'][node]['m_AxesId']
    if ai >= 0 and b['axes'] is None:
        ax = axes[ai]
        b['axes'] = (v4(ax['m_PreQ']), v4(ax['m_PostQ']), v3(ax['m_Sgn']),
                     v3(ax['m_Limit']['m_Min']), v3(ax['m_Limit']['m_Max']))
    b['dof'][axis] = muscle
    b['human'] = bname


for k, (bname, axis) in enumerate(BODY_MUSCLES):
    set_axes(bname, axis, k)
fk = 55
for side in ('Left', 'Right'):
    for fn in hand_names:
        set_axes(f'{side}{fn}1', 2, fk); set_axes(f'{side}{fn}1', 1, fk + 1)
        set_axes(f'{side}{fn}2', 2, fk + 2); set_axes(f'{side}{fn}3', 2, fk + 3)
        fk += 4
# bones that are human but have no DoF muscles still get their axes (neutral pose)
for hname, node in human_node.items():
    bi = node_bone[node]
    if bi >= 0 and bones[bi]['axes'] is None and hsk['m_Node'][node]['m_AxesId'] >= 0:
        ax = axes[hsk['m_Node'][node]['m_AxesId']]
        bones[bi]['axes'] = (v4(ax['m_PreQ']), v4(ax['m_PostQ']), v3(ax['m_Sgn']),
                             v3(ax['m_Limit']['m_Min']), v3(ax['m_Limit']['m_Max']))
        bones[bi]['human'] = hname
hips_bone = node_bone[human_node['Hips']]
root_x = hu['m_RootX']
root_xt, root_xq = v3(root_x['t']), v4(root_x['q'])
human_scale = hu['m_Scale']
print('human bones mapped', sum(1 for b in bones if b['human']), 'hips bone', bones[hips_bone]['name'],
      'scale', human_scale, 'rootX', root_xt, root_xq)

# clips
ctrl = anim_o.read().m_Controller.read()
clips = []
for c in ctrl.m_AnimationClips:
    co = c.read().object_reader
    name, loop, data = decode_clip(co)
    clips.append((name, loop, data))
    print('clip', name, 'loop', loop, 'frames', len(data), 'rootT.y range', data[:, 1].min().round(3), data[:, 1].max().round(3))


# ------------------------------------------------------------------ per-level AI data
def neighbor_ai(scene):
    objs, gos, trs_, go2tr = scene_maps(scene)
    af = next(iter(objs.values())).assets_file
    nb = next((g for g, d in gos.items() if d['m_Name'] == 'NeighborNew'), None)
    if nb is None:
        return None
    w = world_of(trs_, go2tr[nb])
    t, q, s = mat_to_trs(w)
    active = bool(gos[nb].get('m_IsActive', 1))
    see, attack, wps = 10.0, 0.5, []
    for pid, o in objs.items():
        if o.type.name != 'MonoBehaviour':
            continue
        raw = o.get_raw_data()
        if struct.unpack_from('<q', raw, 4)[0] != nb:
            continue
        try:
            if o.read(check_read=False).m_Script.read().m_Name != 'NeighborAI':
                continue
        except Exception:
            continue
        n = struct.unpack_from('<i', raw, 28)[0]
        p = (32 + n + 3) & ~3
        p += 4                    # playerLayer
        p += 12 + 12              # stepSound, screamSound
        see, attack = struct.unpack_from('<ff', raw, p); p += 8
        cnt = struct.unpack_from('<i', raw, p)[0]; p += 4
        for _ in range(cnt):
            fid, gpid = struct.unpack_from('<iq', raw, p); p += 12
            if fid == 0 and gpid in go2tr:
                wt = world_of(trs_, go2tr[gpid])[:3, 3]
                wps.append((gos[gpid]['m_Name'], tuple(float(x) for x in wt)))
    return {'active': active, 't': t, 'q': q, 'see': see, 'attack': attack, 'wps': wps}


levels = []
for scene, lv in SCENES:
    ai = neighbor_ai(scene)
    levels.append((lv, ai))
    if ai:
        print(f'  level {lv}: active={ai["active"]} see={ai["see"]} attack={ai["attack"]} waypoints {[n for n, _ in ai["wps"]]}')
    else:
        print(f'  level {lv}: no neighbour')

# GameOverCamera (child of NeighborNew): the 2 s catch shot before the GameOver panel
goc_t, goc_q, goc_fov = (0.0, 1.6, 1.0), (0.0, 1.0, 0.0, 0.0), 60.0
for gid, d in gos.items():
    if d['m_Name'] == 'GameOverCamera' and gid in go2tr:
        lt, lq, _ = mat_to_trs(nb_inv @ world_of(trs_, go2tr[gid]))
        goc_t, goc_q = tuple(float(x) for x in lt), tuple(float(x) for x in lq)
        for pid, o in objs.items():
            if o.type.name == 'Camera':
                ct = o.read_typetree()
                if ct['m_GameObject']['m_PathID'] == gid:
                    goc_fov = float(ct['field of view'])
print('GameOverCamera local', [round(x, 3) for x in goc_t], [round(x, 3) for x in goc_q], 'fov', goc_fov)

# ------------------------------------------------------------------ write
def s32(s):
    b = s.encode('utf-8')[:31]
    return b + b'\0' * (32 - len(b))


with open(OUT, 'wb') as f:
    f.write(b'DRCH' + struct.pack('<I', 1))
    # texture
    data = tex_img.tobytes()
    f.write(struct.pack('<III', tex_img.width, tex_img.height, len(data)) + data)
    # mesh
    f.write(struct.pack('<II', len(V), len(IDX)))
    rec = np.zeros(len(V), dtype=[('p', '<f4', 3), ('n', '<f4', 3), ('uv', '<f4', 2), ('bi', 'u1', 4), ('bw', '<f4', 4)])
    rec['p'] = V; rec['n'] = N; rec['uv'] = UV
    rec['bi'] = np.clip(BI, 0, 255).astype(np.uint8)
    wsum = BW.sum(1, keepdims=True); wsum[wsum == 0] = 1
    rec['bw'] = BW / wsum
    f.write(rec.tobytes())
    f.write(IDX.astype('<u4').tobytes())
    # skeleton
    f.write(struct.pack('<Iif3f4f', len(bones), hips_bone, human_scale, *root_xt, *root_xq))
    for b in bones:
        f.write(s32(b['name']) + struct.pack('<i3f4f3f', b['parent'], *b['t'], *b['q'], *b['s']))
        f.write(struct.pack('<16f', *b['bind'].T.reshape(-1)))     # column-major
        if b['axes']:
            pre, post, sgn, mn, mx = b['axes']
            f.write(struct.pack('<I4f4f3f3f3f3i', 1, *pre, *post, *sgn, *mn, *mx, *b['dof']))
        else:
            f.write(struct.pack('<I4f4f3f3f3f3i', 0, *([0] * 3 + [1]), *([0] * 3 + [1]), 1, 1, 1, 0, 0, 0, 0, 0, 0, -1, -1, -1))
    # clips
    f.write(struct.pack('<I', len(clips)))
    for name, loop, data in clips:
        f.write(s32(name) + struct.pack('<fIII', FPS, len(data), int(loop), NCH))
        f.write(data.astype('<f4').tobytes())
    # levels
    f.write(struct.pack('<I', len(levels)))
    for lv, ai in levels:
        if not ai:
            f.write(struct.pack('<II', lv, 0)); continue
        f.write(struct.pack('<II', lv, 1 if ai['active'] else 2))
        f.write(struct.pack('<3f4fffI', *ai['t'], *ai['q'], ai['see'], ai['attack'], len(ai['wps'])))
        for _, p in ai['wps']:
            f.write(struct.pack('<3f', *p))
    # GameOverCamera
    f.write(struct.pack('<3f4ff', *goc_t, *goc_q, goc_fov))
print('wrote', OUT)
