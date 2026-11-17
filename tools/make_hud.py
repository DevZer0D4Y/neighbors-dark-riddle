"""Packs the original in-game UI of every level into romfs/hud.pak.

Source: each gameplay scene's own Canvas (Screen Space Overlay + CanvasScaler).
- RectTransform hierarchy resolved to 1280x720 pixel rects (anchors, pivots,
  sizeDelta, localScale, CanvasScaler "scale with screen size").
- UnityEngine.UI Image / Text / RawImage components. This Mono build ships no type
  trees for MonoBehaviours, so their serialized fields are parsed from raw data.
- Sprites are the game's own (cropped from their atlases), fonts are the game's own
  TTFs rasterised to atlases.

Format DRHD v1 (little endian):
  u32 nTex, then per texture: u32 w, u32 h, u32 bytes, char[32] name, RGBA bytes
  u32 nFont, then per font: char[32] name, f32 pxSize, u32 texIndex, f32 ascent,
      f32 lineHeight, u32 nGlyph, nGlyph * (u32 code, i32 x, y, w, h, f32 bx, by, adv)
  u32 nElem, then per element:
      str path, u32 level, u32 kind (1 image, 2 text, 3 rawimage, 0 rect only),
      f32 x, y, w, h (pixels, y down), f32 r, g, b, a, char[32] sprite, i32 font,
      f32 fontPx, f32 minPx (best fit, 0 = fixed size), u32 align (Unity TextAnchor), u32 imageType, str text
  str = u32 length + utf-8 bytes
"""
import struct, sys, collections
import UnityPy
from PIL import Image, ImageDraw, ImageFont
import io

sys.stdout.reconfigure(encoding='utf-8')
SCREEN_W, SCREEN_H = 1280, 720
SCENES = [('level3', 1), ('level4', 2), ('level5', 3), ('level6', 4),
          ('level7', 5), ('level8', 6), ('level9', 7)]
# the Menu scene (main menu / options / level select) and the LoadGame scene
SCENES += [('level1', 100), ('level2', 101)]
FONT_PX = 64          # atlas raster size; drawn scaled
OUT = '../game/romfs/hud.pak'

env = UnityPy.load('merged')
by_file = collections.defaultdict(dict)
for o in env.objects:
    by_file[o.assets_file.name][o.path_id] = o


def resolve(af, fid, pid):
    """PPtr -> ObjectReader (fileID 0 = same file)."""
    if not pid:
        return None
    if fid == 0:
        name = af.name
    else:
        try:
            name = af.externals[fid - 1].path.split('/')[-1]
        except Exception:
            return None
    return by_file.get(name, {}).get(pid)


# ------------------------------------------------------------------ raw readers
class Raw:
    def __init__(self, b, p=0):
        self.b, self.p = b, p
    def align(self):
        self.p = (self.p + 3) & ~3
    def i32(self):
        v = struct.unpack_from('<i', self.b, self.p)[0]; self.p += 4; return v
    def f32(self):
        v = struct.unpack_from('<f', self.b, self.p)[0]; self.p += 4; return v
    def u8(self):
        v = self.b[self.p]; self.p += 1; return v
    def pptr(self):
        fid, pid = struct.unpack_from('<iq', self.b, self.p); self.p += 12; return fid, pid
    def string(self):
        n = self.i32()
        if n < 0 or self.p + n > len(self.b):
            raise ValueError('bad string')
        s = self.b[self.p:self.p + n].decode('utf-8'); self.p += n; self.align(); return s


def mono_base(raw):
    """Skip MonoBehaviour header: m_GameObject, m_Enabled, m_Script, m_Name."""
    r = Raw(raw)
    go = r.pptr()
    enabled = r.u8(); r.align()
    r.pptr()
    r.string()
    return r, go, enabled


def graphic(r):
    """Graphic + MaskableGraphic fields; returns colour. Raises on unexpected data."""
    r.pptr()                                    # m_Material
    col = (r.f32(), r.f32(), r.f32(), r.f32())  # m_Color
    r.u8(); r.align()                           # m_RaycastTarget
    ncalls = r.i32()                            # m_OnCullStateChanged.m_PersistentCalls.m_Calls
    if ncalls != 0:
        raise ValueError('persistent calls')
    r.string()                                  # m_TypeName
    return col


def parse_image(raw):
    r, go, en = mono_base(raw)
    col = graphic(r)
    sprite = r.pptr()
    itype = r.i32()
    preserve = False
    try:
        preserve = bool(r.u8()); r.align()      # m_PreserveAspect (Unity 2018.1 Image)
    except Exception:
        pass
    return {'color': col, 'sprite': sprite, 'type': itype, 'enabled': en, 'preserve': preserve}


def parse_text(raw):
    r, go, en = mono_base(raw)
    col = graphic(r)
    font = r.pptr()
    size = r.i32(); style = r.i32()
    best = r.u8(); r.align()                    # m_BestFit
    mn = r.i32(); mx = r.i32()                  # m_MinSize, m_MaxSize
    align = r.i32()
    r.u8(); r.align()                           # m_AlignByGeometry (each bool is padded)
    r.u8(); r.align()                           # m_RichText
    hov = r.i32(); vov = r.i32(); r.f32()       # overflow, m_LineSpacing
    text = r.string()
    return {'color': col, 'font': font, 'size': size, 'style': style, 'align': align,
            'hwrap': hov == 0, 'text': text, 'enabled': en,
            'bestFit': bool(best), 'minSize': mn, 'maxSize': mx}


def parse_canvas_scaler(raw):
    r, go, en = mono_base(raw)
    mode = r.i32(); r.f32(); sf = r.f32()
    rw, rh = r.f32(), r.f32()
    smm = r.i32(); match = r.f32()
    return mode, sf, rw, rh, smm, match


# ------------------------------------------------------------------ sprites / fonts
textures = []          # (name, PIL image)
tex_index = {}


def sprite_image(obj):
    key = (obj.assets_file.name, obj.path_id)
    if key in tex_index:
        return textures[tex_index[key]][0]
    d = obj.read()
    img = d.image.convert('RGBA')
    # tightly packed sprites: UnityPy returns only the packed area; Unity draws the
    # sprite over its full rect (Image adds the transparent padding), so restore it
    try:
        tt = obj.read_typetree()
        rw, rh = int(round(tt['m_Rect']['width'])), int(round(tt['m_Rect']['height']))
        if (rw, rh) != img.size and rw >= img.width and rh >= img.height:
            off = tt['m_RD'].get('textureRectOffset', {'x': 0, 'y': 0})
            full = Image.new('RGBA', (rw, rh), (0, 0, 0, 0))
            ox = int(round(off['x'])); oy = rh - int(round(off['y'])) - img.height   # Unity y is up
            full.paste(img, (max(0, ox), max(0, oy)))
            img = full
    except Exception as ex:
        print('   sprite pad fail', d.m_Name, ex)
    name = d.m_Name[:31]
    # keep names unique (a few sprites share names across atlases)
    base, k = name, 1
    while any(n == name for n, _ in textures):
        name = f'{base[:28]}~{k}'; k += 1
    tex_index[key] = len(textures)
    textures.append((name, img))
    return name


fonts = []             # (name, ttf bytes)
font_index = {}


def font_id(obj):
    if obj is None:
        return -1
    key = (obj.assets_file.name, obj.path_id)
    if key in font_index:
        return font_index[key]
    d = obj.read(check_read=False)
    data = bytes(d.m_FontData or [])
    if not data:
        return -1
    font_index[key] = len(fonts)
    fonts.append((d.m_Name, data))
    return font_index[key]


# ------------------------------------------------------------------ layout
elements = []
used_chars = set(chr(c) for c in range(32, 127))


def build(scene, level):
    objs = by_file[scene]
    af = next(iter(objs.values())).assets_file
    gos, rts = {}, {}
    for pid, o in objs.items():
        if o.type.name == 'GameObject':
            gos[pid] = o.read_typetree()
        elif o.type.name == 'RectTransform':
            rts[pid] = o.read_typetree()
    rt_by_go = {t['m_GameObject']['m_PathID']: pid for pid, t in rts.items()}
    comps = collections.defaultdict(dict)
    scaler = None
    for pid, o in objs.items():
        if o.type.name not in ('MonoBehaviour',):
            continue
        raw = o.get_raw_data()
        gid = struct.unpack_from('<q', raw, 4)[0]
        try:
            sc = o.read(check_read=False).m_Script.read().m_Name
        except Exception:
            continue
        try:
            if sc == 'Image':
                comps[gid]['image'] = parse_image(raw)
            elif sc == 'Text':
                comps[gid]['text'] = parse_text(raw)
            elif sc == 'RawImage':
                comps[gid]['raw'] = True
            elif sc == 'CanvasScaler':
                scaler = parse_canvas_scaler(raw)
        except Exception as e:
            print('   skip', sc, gos.get(gid, {}).get('m_Name'), e)

    # canvas scale factor (ScaleWithScreenSize, match width/height)
    sf = 1.0
    if scaler and scaler[0] == 1:
        import math
        _, _, rw, rh, smm, match = scaler
        lw, lh = math.log2(SCREEN_W / rw), math.log2(SCREEN_H / rh)
        sf = 2 ** (lw + (lh - lw) * match)
    elif scaler:
        sf = scaler[1]
    print(f'  {scene}: canvas scaler {scaler} -> scale {sf:.3f}')

    children = collections.defaultdict(list)
    roots = []
    for pid, t in rts.items():
        f = t['m_Father']['m_PathID']
        if f and f in rts:
            children[f].append(pid)
        else:
            roots.append(pid)
    for ch in children.values():   # draw order = sibling order (m_Children)
        pass

    def ordered_children(pid):
        t = rts[pid]
        return [c['m_PathID'] for c in t.get('m_Children', []) if c['m_PathID'] in rts]

    def walk(pid, origin, scale, pw, ph, ppx, ppy, path, active):
        t = rts[pid]
        gid = t['m_GameObject']['m_PathID']
        g = gos[gid]
        name = g['m_Name']
        act = active and bool(g.get('m_IsActive', 1))
        am, ax, ap, sd, pv = t['m_AnchorMin'], t['m_AnchorMax'], t['m_AnchoredPosition'], t['m_SizeDelta'], t['m_Pivot']
        ls = t['m_LocalScale']
        # parent rect in its local space (origin at parent pivot)
        px0, py0 = -ppx * pw, -ppy * ph
        ax0, ax1 = px0 + am['x'] * pw, px0 + ax['x'] * pw
        ay0, ay1 = py0 + am['y'] * ph, py0 + ax['y'] * ph
        w = (ax1 - ax0) + sd['x']; h = (ay1 - ay0) + sd['y']
        pivx = ax0 + (ax1 - ax0) * pv['x'] + ap['x']
        pivy = ay0 + (ay1 - ay0) * pv['y'] + ap['y']
        o2 = (origin[0] + scale[0] * pivx, origin[1] + scale[1] * pivy)
        s2 = (scale[0] * ls['x'], scale[1] * ls['y'])
        # rect corners in screen pixels (y up) -> y down
        x0 = o2[0] + s2[0] * (-pv['x'] * w); x1 = o2[0] + s2[0] * ((1 - pv['x']) * w)
        y0 = o2[1] + s2[1] * (-pv['y'] * h); y1 = o2[1] + s2[1] * ((1 - pv['y']) * h)
        rx, ry, rw_, rh_ = min(x0, x1), SCREEN_H - max(y0, y1), abs(x1 - x0), abs(y1 - y0)
        full = f'{path}/{name}' if path else name
        c = comps.get(gid, {})
        e = {'path': full, 'level': level, 'kind': 0, 'rect': (rx, ry, rw_, rh_), 'color': (1, 1, 1, 1),
             'sprite': '', 'font': -1, 'fontPx': 0.0, 'minPx': 0.0, 'align': 0, 'itype': 0, 'text': '', 'active': act}
        if 'image' in c:
            im = c['image']
            e['kind'] = 1; e['color'] = im['color']; e['itype'] = im['type']
            so = resolve(af, *im['sprite'])
            if so is not None and so.type.name == 'Sprite':
                try:
                    e['sprite'] = sprite_image(so)
                    # Image.preserveAspect: the sprite keeps its aspect, centred in the rect
                    if im.get('preserve') and im['type'] == 0:
                        sw_, sh_ = next(img.size for n_, img in textures if n_ == e['sprite'])
                        rx_, ry_, rw0, rh0 = e['rect']
                        if sw_ > 0 and sh_ > 0 and rw0 > 0 and rh0 > 0:
                            a_ = sw_ / sh_
                            if rw0 / rh0 > a_:
                                nw = rh0 * a_; e['rect'] = (rx_ + (rw0 - nw) / 2, ry_, nw, rh0)
                            else:
                                nh = rw0 / a_; e['rect'] = (rx_, ry_ + (rh0 - nh) / 2, rw0, nh)
                except Exception as ex:
                    print('   sprite fail', full, ex)
            if not im['enabled']:
                e['active'] = False
        elif 'text' in c:
            tx = c['text']
            e['kind'] = 2; e['color'] = tx['color']; e['align'] = tx['align']; e['text'] = tx['text']
            e['font'] = font_id(resolve(af, *tx['font']))
            if tx['bestFit']:
                e['fontPx'] = tx['maxSize'] * abs(s2[1]); e['minPx'] = tx['minSize'] * abs(s2[1])
            else:
                e['fontPx'] = tx['size'] * abs(s2[1])
            used_chars.update(tx['text'])
            if not tx['enabled']:
                e['active'] = False
        elif c.get('raw'):
            e['kind'] = 3
        elements.append(e)
        for ch in ordered_children(pid):
            walk(ch, o2, s2, w, h, pv['x'], pv['y'], full, act)

    for pid in roots:
        t = rts[pid]
        g = gos[t['m_GameObject']['m_PathID']]
        if g['m_Name'] != 'Canvas':
            continue
        # Screen Space Overlay root: covers the screen, scaled by the CanvasScaler
        cw, ch = SCREEN_W / sf, SCREEN_H / sf
        origin = (SCREEN_W / 2, SCREEN_H / 2)
        for c in [c['m_PathID'] for c in t.get('m_Children', []) if c['m_PathID'] in rts]:
            walk(c, origin, (sf, sf), cw, ch, 0.5, 0.5, 'Canvas', bool(g.get('m_IsActive', 1)))


for scene, level in SCENES:
    build(scene, level)

# minimap SpriteRenderers (layer 8): MinimapSprite / MinimapArrowNeighbor use
# "MiniMapIcon", the UpArrow / DownArrow children the arrow sprite
mm_names = {}
for o in env.objects:
    if o.type.name != 'SpriteRenderer' or o.assets_file.name != 'level3':
        continue
    try:
        d = o.read_typetree()
        go = by_file['level3'][d['m_GameObject']['m_PathID']].read_typetree()['m_Name']
        if go not in ('MinimapSprite (1)', 'UP'):
            continue
        so = resolve(by_file['level3'][d['m_GameObject']['m_PathID']].assets_file, d['m_Sprite']['m_FileID'], d['m_Sprite']['m_PathID'])
        mm_names[go] = sprite_image(so)
    except Exception as ex:
        print('   minimap sprite fail', ex)
print('minimap sprites', mm_names)
print('elements', len(elements), 'sprites', len(textures), 'fonts', [f[0] for f in fonts])

# ------------------------------------------------------------------ font atlases
font_blobs = []
for fname, data in fonts:
    pf = ImageFont.truetype(io.BytesIO(data), FONT_PX)
    asc, desc = pf.getmetrics()
    chars = sorted(c for c in used_chars if c >= ' ' and c != '\x7f')
    atlas_w = 1024
    PAD = 8           # empty texels around every glyph: no bleeding when scaled down
    ORG = FONT_PX     # pen origin inside the per-glyph canvas
    x = y = PAD
    row_h = 0
    glyphs = []
    places = []
    for ch in chars:
        adv = pf.getlength(ch)
        if ch == ' ':
            glyphs.append((ord(ch), 0, 0, 0, 0, 0.0, 0.0, adv)); continue
        # render alone and measure the real ink (decorative fonts overhang getbbox)
        cell = Image.new('L', (FONT_PX * 3, FONT_PX * 3), 0)
        ImageDraw.Draw(cell).text((ORG, ORG), ch, font=pf, fill=255)
        ink = cell.getbbox()
        if not ink:
            glyphs.append((ord(ch), 0, 0, 0, 0, 0.0, 0.0, adv)); continue
        gw, gh = ink[2] - ink[0], ink[3] - ink[1]
        if x + gw + PAD > atlas_w:
            x = PAD; y += row_h + PAD; row_h = 0
        places.append((x, y, cell.crop(ink)))
        glyphs.append((ord(ch), x, y, gw, gh, float(ink[0] - ORG), float(ink[1] - ORG), adv))
        x += gw + PAD
        row_h = max(row_h, gh)
    atlas_h = 1
    while atlas_h < y + row_h + PAD:
        atlas_h *= 2
    img = Image.new('RGBA', (atlas_w, atlas_h), (255, 255, 255, 0))
    for gx, gy, a in places:
        g = Image.new('RGBA', a.size, (255, 255, 255, 0)); g.putalpha(a)
        img.paste(g, (gx, gy))
    tex_name = ('font:' + fname)[:31]
    textures.append((tex_name, img))
    font_blobs.append((fname, len(textures) - 1, float(asc), float(asc + desc), glyphs))
    print('  font', fname, 'glyphs', len(glyphs), 'atlas', img.size)


# ------------------------------------------------------------------ write
def s32(s, n=32):
    b = s.encode('utf-8')[:n - 1]
    return b + b'\0' * (n - len(b))


def lstr(s):
    b = s.encode('utf-8')
    return struct.pack('<I', len(b)) + b


with open(OUT, 'wb') as f:
    f.write(b'DRHD' + struct.pack('<I', 1))
    f.write(struct.pack('<I', len(textures)))
    for name, img in textures:
        data = img.tobytes()
        f.write(struct.pack('<III', img.width, img.height, len(data)) + s32(name) + data)
    f.write(struct.pack('<I', len(font_blobs)))
    for fname, ti, asc, lh, glyphs in font_blobs:
        f.write(s32(fname) + struct.pack('<fIffI', float(FONT_PX), ti, asc, lh, len(glyphs)))
        for g in glyphs:
            f.write(struct.pack('<Iiiiifff', *g))
    f.write(struct.pack('<I', len(elements)))
    for e in elements:
        x, y, w, h = e['rect']
        f.write(lstr(e['path']) + struct.pack('<II', e['level'], e['kind'] | (0 if e['active'] else 0x100)))
        f.write(struct.pack('<4f4f', x, y, w, h, *e['color']) + s32(e['sprite']))
        f.write(struct.pack('<iffII', e['font'], e['fontPx'], e['minPx'], e['align'], e['itype']) + lstr(e['text']))
print('wrote', OUT)
