#!/bin/bash
# Converts the extracted APK data (tools/merged, see extract_apk.py) into the
# game's romfs/*.pak files.
set -e
cd "$(dirname "$0")"
mkdir -p ../game/romfs
for t in make_data.py make_hud.py make_ui.py make_char.py make_nav.py make_trees.py; do
    echo "== $t"
    PYTHONIOENCODING=utf-8 python3 "$t"
done
echo "done: ../game/romfs"
