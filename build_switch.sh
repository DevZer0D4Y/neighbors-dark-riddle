#!/bin/bash
# Nintendo Switch homebrew (.nro). Needs devkitPro with devkitA64, libnx and the
# switch-sdl2 / switch-mesa portlibs (dkp-pacman -S switch-dev switch-sdl2 switch-mesa).
# The game data (game/romfs/*.pak, see tools/) is packed into the .nro.
set -e
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$DEVKITPRO/portlibs/switch/bin:$PATH"
cd "$(dirname "$0")/game"
make -j"$(nproc)" all
echo "built game/darkriddle.nro"
