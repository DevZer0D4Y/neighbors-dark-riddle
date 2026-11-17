"""Builds the app icons from the game's own launcher icon (taken from your APK by
extract_apk.py as tools/merged/app_icon.png):

  game/icon.jpg   256x256 JPEG, the Nintendo Switch homebrew icon (.nro)
  game/icon.ico   16-256 px, the Windows executable / window icon
"""
import os, sys
from PIL import Image

here = os.path.dirname(os.path.abspath(__file__))
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, 'merged', 'app_icon.png')
game = os.path.join(here, '..', 'game')
img = Image.open(src).convert('RGBA')

# Switch: square 256x256 JPEG (no alpha: the rounded corners become black)
sw = Image.new('RGB', (256, 256), (0, 0, 0))
big = img.resize((256, 256), Image.LANCZOS)
sw.paste(big, (0, 0), big)
sw.save(os.path.join(game, 'icon.jpg'), quality=95)

# Windows: multi-size .ico (keeps the transparent rounded corners)
img.resize((256, 256), Image.LANCZOS).save(os.path.join(game, 'icon.ico'),
                                           sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
print('wrote', os.path.join(game, 'icon.jpg'), 'and icon.ico')
