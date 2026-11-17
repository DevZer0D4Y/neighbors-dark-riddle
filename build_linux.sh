#!/bin/bash
# Linux build (SDL2 + OpenGL ES 2 / EGL): sudo apt install g++ libsdl2-dev libgles2-mesa-dev libegl1-mesa-dev
# Run it from the game folder so it finds romfs/: cd game && ./darkriddle_linux
set -e
cd "$(dirname "$0")/game"
g++ -O2 -std=gnu++17 -Isrc src/*.cpp -o darkriddle_linux $(pkg-config --cflags sdl2) -lSDL2 -lGLESv2 -lEGL
echo "built game/darkriddle_linux"
