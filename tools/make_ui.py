"""Packs the original Dark Riddle menu UI (sprites + layout) into romfs/ui.pak.

Layout comes from the game's own Menu scene (level1) RectTransforms; textures are
the game's own sprites (room artwork, orange button plate, level numbers, lock icon).
"""
import struct
import UnityPy
from PIL import Image

ENV = UnityPy.load('merged')

WANT = ['MENU', 'buttons-orange_0', 'closedlvl'] + [str(i) for i in range(1, 8)]

textures = []   # (name, PIL image)
sprite_ids = {}

print('reading sprites...')
for o in ENV.objects:
    if o.type.name != 'Sprite':
        continue
    try:
        d = o.read_typetree()
    except Exception:
        continue
    name = d.get('m_Name')
    if name not in WANT or name in sprite_ids:
        continue
    rd = d.get('m_RD', {})
    texid = rd.get('texture', {}).get('m_PathID')
    tex = next((x for x in ENV.objects if x.path_id == texid and x.assets_file == o.assets_file), None)
    if tex is None:
        tex = next((x for x in ENV.objects if x.path_id == texid), None)
    if tex is None:
        continue
    try:
        img = tex.read().image.convert('RGBA')
    except Exception as e:
        print('  tex fail', name, e)
        continue
    rect = d.get('m_Rect', {})
    rx = int(rect.get('x', 0)); ry = int(rect.get('y', 0))
    rw = int(rect.get('width', img.width)); rh = int(rect.get('height', img.height))
    top = img.height - (ry + rh)
    crop = img.crop((rx, top, rx + rw, top + rh))
    sprite_ids[name] = len(textures) + 1
    textures.append((name, crop))
    print('  sprite', name, crop.size)

# ---------------------------------------------------------------- layout
def tt(o):
    try:
        return o.read_typetree()
    except Exception:
        return None

objs = {o.path_id: o for o in ENV.objects if o.assets_file.name.endswith('level1')}
gos, trs = {}, {}
for pid, o in objs.items():
    if o.type.name == 'GameObject':
        d = tt(o)
        if d:
            gos[pid] = d.get('m_Name', '')
    elif o.type.name in ('RectTransform', 'Transform'):
        d = tt(o)
        if d:
            trs[pid] = d
tr_go = {pid: d['m_GameObject']['m_PathID'] for pid, d in trs.items()}

CANVAS_W, CANVAS_H = 722.1, 664.8   # reference canvas the anchors are authored against

def norm(ap_x, ap_y, w, h):
    cx = (CANVAS_W * 0.5 + ap_x) / CANVAS_W
    cy = (CANVAS_H * 0.5 - ap_y) / CANVAS_H
    return cx, cy, w / CANVAS_W, h / CANVAS_H

elements = []   # (panel, spriteId, cx, cy, w, h, label, id)

def add(panel, sprite, ap_x, ap_y, w, h, label, ident):
    sid = sprite_ids.get(sprite, 0)
    cx, cy, nw, nh = norm(ap_x, ap_y, w, h)
    elements.append((panel, sid, cx, cy, nw, nh, label, ident))

# full-screen room artwork behind both panels
add(1, 'MENU', 0, 0, CANVAS_W, CANVAS_H, '', 0)
add(2, 'MENU', 0, 0, CANVAS_W, CANVAS_H, '', 0)

# main menu buttons (from RectTransforms: OpenLevelSelect, Options, Quit)
add(1, 'buttons-orange_0', -276.0, -16.0, 180.5, 80.5, 'PLAY', 10)
add(1, 'buttons-orange_0', -276.0, -85.2, 180.5, 80.5, 'OPTIONS', 11)
add(1, 'buttons-orange_0', -276.0, -152.1, 180.5, 80.5, 'QUIT', 12)

# level select buttons (1Lvl .. 7Lvl) + back
levels = {
    1: (-298.5, 149.1), 2: (-249.5, 26.0), 3: (-172.0, -98.0),
    4: (-40.0, 26.0), 5: (86.0, -56.0), 6: (225.0, 12.0), 7: (316.0, 110.2),
}
for lv, (x, y) in levels.items():
    add(2, str(lv), x, y, 111.2, 110.2, '', 20 + lv)
add(2, 'buttons-orange_0', 271.0, -152.0, 180.5, 80.5, 'BACK', 30)

# ---------------------------------------------------------------- write pak
def w32(v):
    return struct.pack('<I', v)

with open('../game/romfs/ui.pak', 'wb') as f:
    f.write(b'DRUI')
    f.write(w32(1))
    f.write(w32(len(textures)))
    f.write(w32(len(elements)))
    for name, img in textures:
        data = img.tobytes()
        f.write(w32(img.width))
        f.write(w32(img.height))
        f.write(w32(len(data)))
        nb = name.encode('utf-8')[:31]
        f.write(nb + b'\0' * (32 - len(nb)))
        f.write(data)
    for (panel, sid, cx, cy, nw, nh, label, ident) in elements:
        f.write(w32(panel))
        f.write(w32(sid))
        f.write(struct.pack('<4f', cx, cy, nw, nh))
        f.write(w32(ident))
        lb = label.encode('utf-8')[:23]
        f.write(lb + b'\0' * (24 - len(lb)))

print('wrote ../game/romfs/ui.pak', len(textures), 'textures', len(elements), 'elements')
