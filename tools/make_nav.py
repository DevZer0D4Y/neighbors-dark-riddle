"""Exports the levels' baked Unity NavMesh (sharedassets3 NavMeshData, shared by all
gameplay scenes) as a polygon graph: romfs/nav.pak.

Each tile's m_MeshData is a Detour tile in Unity's format version 16:
  header (72 bytes): 'DNAV' (as a little-endian int, bytes 'VAND'), version, tile x, tile y, layer, polyCount, vertCount,
      detailMeshCount, detailVertCount, detailTriCount, bvNodeCount, bmin[3], bmax[3],
      bvQuantFactor
  verts:  vertCount * float3
  polys:  polyCount * 32 bytes: u16 verts[6], u16 neis[6], u32 flags, u8 vertCount,
          u8 area, 2 pad.  nei: 0 none, n = poly n-1 of this tile, 0x8000|dir = portal
          to the neighbouring tile.
Cross-tile portals are resolved geometrically (collinear border edges that overlap),
which is what Detour's connectExtLinks does at runtime.

Format DRNV v1: u32 nVerts, nVerts*float3, u32 nPolys, per poly: u8 n, u8 pad[3],
  u32 verts[6], then u32 nLinks, per link: u32 polyA, u32 edgeA, u32 polyB, float3 a,
  float3 b (portal segment, a/b in polyA's edge order).
"""
import struct, sys, collections
import numpy as np
import UnityPy

sys.stdout.reconfigure(encoding='utf-8')
env = UnityPy.load('merged')
nav = next(o for o in env.objects if o.type.name == 'NavMeshData')
tiles = nav.read_typetree()['m_NavMeshTiles']

verts = []          # global
polys = []          # (tile index, [global vert idx], neis raw list)
tile_poly_base = []
for ti, t in enumerate(tiles):
    d = bytes(t['m_MeshData'])
    magic, ver, tx, ty, layer, pc, vc, dmc, dvc, dtc, bvc = struct.unpack_from('<4s10i', d, 0)
    assert magic == b'VAND' and ver == 16, (magic, ver)
    vbase = len(verts)
    for k in range(vc):
        verts.append(struct.unpack_from('<3f', d, 72 + k * 12))
    p0 = 72 + vc * 12
    tile_poly_base.append(len(polys))
    for k in range(pc):
        off = p0 + k * 32
        vs = struct.unpack_from('<6H', d, off)
        ns = struct.unpack_from('<6H', d, off + 12)
        n = d[off + 28]
        polys.append((ti, [vbase + vs[i] for i in range(n)], list(ns[:n])))
V = np.array(verts, np.float64)
print('tiles', len(tiles), 'polys', len(polys), 'verts', len(verts))

links = []          # (polyA, edgeA, polyB, a, b)
ext = []            # (poly, edge, p, q) border edges awaiting a partner
for pi, (ti, vs, ns) in enumerate(polys):
    base = tile_poly_base[ti]
    for e, nei in enumerate(ns):
        a, b = V[vs[e]], V[vs[(e + 1) % len(vs)]]
        if nei == 0:
            continue
        if nei & 0x8000:
            ext.append((pi, e, a, b))
        else:
            links.append((pi, e, base + nei - 1, a, b))

# cross-tile: collinear on the tile border and overlapping
EPS = 0.02
by_line = collections.defaultdict(list)
for item in ext:
    pi, e, a, b = item
    if abs(a[0] - b[0]) < EPS:
        by_line[('x', round(a[0] / 0.05))].append(item)
    elif abs(a[2] - b[2]) < EPS:
        by_line[('z', round(a[2] / 0.05))].append(item)
nx = 0
for key, items in by_line.items():
    axis = 2 if key[0] == 'x' else 0          # coordinate that varies along the edge
    for i, (pa, ea, a1, b1) in enumerate(items):
        lo1, hi1 = sorted((a1[axis], b1[axis]))
        for j, (pb, eb, a2, b2) in enumerate(items):
            if i == j or polys[pa][0] == polys[pb][0]:
                continue
            lo2, hi2 = sorted((a2[axis], b2[axis]))
            lo, hi = max(lo1, lo2), min(hi1, hi2)
            if hi - lo < 0.05:
                continue
            # vertical agreement at the overlap (avoid linking floors stacked above each other)
            def at(p, q, s):
                tt = (s - p[axis]) / (q[axis] - p[axis]) if abs(q[axis] - p[axis]) > 1e-9 else 0.0
                return p + (q - p) * tt
            m = 0.5 * (lo + hi)
            if abs(at(a1, b1, m)[1] - at(a2, b2, m)[1]) > 0.7:
                continue
            # portal segment in polyA's edge order
            s0, s1 = (lo, hi) if a1[axis] <= b1[axis] else (hi, lo)
            links.append((pa, ea, pb, at(a1, b1, s0), at(a1, b1, s1)))
            nx += 1
print('internal + external links', len(links), 'external', nx)

with open('../game/romfs/nav.pak', 'wb') as f:
    f.write(b'DRNV' + struct.pack('<I', 1))
    f.write(struct.pack('<I', len(verts)) + V.astype('<f4').tobytes())
    f.write(struct.pack('<I', len(polys)))
    for ti, vs, ns in polys:
        f.write(struct.pack('<B3x6I', len(vs), *(vs + [0] * (6 - len(vs)))))
    f.write(struct.pack('<I', len(links)))
    for pa, ea, pb, a, b in links:
        f.write(struct.pack('<3I6f', pa, ea, pb, *a, *b))
print('wrote ../game/romfs/nav.pak')
