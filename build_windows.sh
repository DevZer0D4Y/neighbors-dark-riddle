#!/bin/bash
# Windows build, cross-compiled from Linux / WSL with llvm-mingw.
#   LLVM_MINGW: an extracted llvm-mingw release (https://github.com/mstorsjo/llvm-mingw)
#   SDL2_MINGW: the x86_64-w64-mingw32 folder of SDL2-devel-2.x-mingw (https://github.com/libsdl-org/SDL)
# Output: dist_windows/NeighborsDarkRiddle/ (NeighborsDarkRiddle.exe, DLLs and romfs/ copied from game/romfs)
set -e
: "${LLVM_MINGW:?set LLVM_MINGW to the llvm-mingw folder}"
: "${SDL2_MINGW:?set SDL2_MINGW to SDL2-devel/x86_64-w64-mingw32}"
ROOT="$(cd "$(dirname "$0")" && pwd)"
OUT="$ROOT/dist_windows/NeighborsDarkRiddle"
mkdir -p "$OUT/romfs"
cd "$ROOT/game"
RES=""
if [ -f icon.ico ]; then   # app icon (tools/make_icon.py)
    mkdir -p build
    "$LLVM_MINGW/bin/x86_64-w64-mingw32-windres" src/icon.rc -O coff -o build/icon.res && RES=build/icon.res
fi
"$LLVM_MINGW/bin/x86_64-w64-mingw32-clang++" -O2 -std=gnu++17 -DNDEBUG -Isrc \
    -I"$SDL2_MINGW/include" -I"$SDL2_MINGW/include/SDL2" src/*.cpp \
    $RES -o "$OUT/NeighborsDarkRiddle.exe" -L"$SDL2_MINGW/lib" -lmingw32 -lSDL2main -lSDL2 -mwindows
cp "$SDL2_MINGW/bin/SDL2.dll" "$OUT/"
cp "$LLVM_MINGW/x86_64-w64-mingw32/bin/libc++.dll" "$LLVM_MINGW/x86_64-w64-mingw32/bin/libunwind.dll" "$OUT/"
cp romfs/*.pak "$OUT/romfs/" 2>/dev/null || echo "note: game/romfs is empty - generate the data with tools/ first"
echo "built $OUT"
