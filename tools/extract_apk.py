"""Extracts the Unity data of your own copy of the Dark Riddle Android APK.

Usage:  python extract_apk.py path/to/darkriddle.apk

Writes tools/merged/: the APK's assets/bin/Data folder with Unity's split files
(levelN.split0, levelN.split1, ...) joined back into levelN, and the launcher icon
(app_icon.png). All other tools read their input from this folder.
"""
import os, re, sys, zipfile

if len(sys.argv) != 2:
    print(__doc__)
    sys.exit(1)

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, 'merged')
os.makedirs(out, exist_ok=True)
prefix = 'assets/bin/Data/'
splits = {}
with zipfile.ZipFile(sys.argv[1]) as z:
    for info in z.infolist():
        if not info.filename.startswith(prefix) or info.is_dir():
            continue
        rel = info.filename[len(prefix):]
        dst = os.path.join(out, *rel.split('/'))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with z.open(info) as src, open(dst, 'wb') as f:
            f.write(src.read())
        m = re.match(r'(.+)\.split(\d+)$', rel)
        if m:
            splits.setdefault(m.group(1), []).append((int(m.group(2)), dst))
    # the launcher icon (largest density) for make_icon.py
    icons = [n for n in z.namelist() if re.match(r'res/(drawable|mipmap)-[a-z]+(-v\d+)?/app_icon\.png$', n)]
    order = ['xxxhdpi', 'xxhdpi', 'xhdpi', 'hdpi', 'mdpi', 'ldpi']
    icons.sort(key=lambda n: next((i for i, d in enumerate(order) if '-' + d in n), 99))
    if icons:
        with z.open(icons[0]) as src, open(os.path.join(out, 'app_icon.png'), 'wb') as f:
            f.write(src.read())
for base, parts in splits.items():
    with open(os.path.join(out, *base.split('/')), 'wb') as f:
        for _, p in sorted(parts):
            with open(p, 'rb') as src:
                f.write(src.read())
print('extracted to', out, '-', len(splits), 'split files joined')
