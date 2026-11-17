# Dark Riddle — Native Port for Nintendo Switch, Windows & Linux

A from-scratch native C++ port of the Android Unity game **Dark Riddle**, made to look and play
like the original on **Nintendo Switch (homebrew)**, **Windows** and **Linux**.

The port does not emulate Unity. It reimplements the game's logic in C++ and runs the original
levels, models, textures, animations, sounds and UI. You convert those from **your own copy of
the APK** with the included tools.

> [!IMPORTANT]
> **This repository contains no game content.** It holds only the port's source code and the
> conversion tools. You need your own copy of the Dark Riddle APK to generate the game data.
> Do not redistribute the generated data (`game/romfs/*.pak`) or any build that contains it.
> The Switch `.nro` embeds the data.

---

## Features

- **All 7 levels** with their objectives: key and lock, wires, money, car, grave.
- **The neighbour's AI** follows the original NeighborAI script: patrols, sight, chase,
  catch and the game-over camera. It moves on the level's baked NavMesh, and the character is
  animated with Mecanim-style humanoid retargeting.
- **Physics** for throwable objects, breakable windows, doors and locks, and Unity's
  CharacterController movement and collision.
- **Original look:** baked lighting that matches Unity's "Very Low" quality level, the skybox,
  swaying trees, the road, and the live minimap.
- **Original interface:** the main menu, level select, task panels and pickup icons, rendered
  from the game's own canvases.
- **Audio** from the original clips, played through a small software mixer: SDL on PC,
  `audout` on Switch.
- **Controls menu** on every platform: control schemes, look/mouse sensitivity and a
  **controls editor** that rebinds keys and controller buttons.
- **PC version:** keyboard, mouse and controller support, a resizable window that keeps the
  16:9 picture with black bars, fullscreen with F11 / Alt+Enter, and a fixed 60 FPS.

## Platforms

| Platform | Graphics | Audio | Input |
|---|---|---|---|
| Nintendo Switch (homebrew `.nro`) | GLES2 (mesa / nouveau) | libnx `audout` | Joy-Con / Pro Controller, touch |
| Windows 10/11 (x64) | OpenGL ES 2 via WGL (desktop GL 2.1 fallback) | SDL2 | keyboard, mouse, any SDL controller |
| Linux | OpenGL ES 2 / EGL | SDL2 | keyboard, mouse, any SDL controller |

## Repository layout

```
game/
  src/            engine and game (C++17, SDL2, OpenGL ES 2)
  Makefile        Nintendo Switch build (devkitPro)
  romfs/          generated game data goes here (empty in the repository)
tools/            Python tools: APK  ->  game/romfs/*.pak
build_switch.sh   build_windows.sh   build_linux.sh
```

## Getting started

### 1. Generate the game data from your APK

Requires Python 3.10 or newer.

```bash
cd tools
pip install -r requirements.txt
python extract_apk.py path/to/darkriddle.apk     # unpacks the Unity data into tools/merged/
./make_all_data.sh                                # or make_all_data.bat on Windows
```

This writes the files the game loads into `game/romfs/`:

| File | Contents |
|---|---|
| `data.pak` | levels, meshes, textures, materials, lighting, colliders, audio |
| `hud.pak`, `ui.pak` | menus and in-game interface |
| `char.pak` | the neighbour: skinned mesh, skeleton, animations |
| `nav.pak` | the levels' NavMesh |
| `trees.pak` | animated trees |

Converting the levels takes a few minutes.

### 2. Build

**Nintendo Switch.** Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) with
`switch-dev switch-sdl2 switch-mesa`, then run:

```bash
./build_switch.sh            # -> game/darkriddle.nro
```

Copy `darkriddle.nro` to `/switch/` on the SD card and start it from the Homebrew Menu.

**Windows.** The Windows version is cross-compiled from Linux or WSL with
[llvm-mingw](https://github.com/mstorsjo/llvm-mingw) and the
[SDL2 MinGW development files](https://github.com/libsdl-org/SDL/releases):

```bash
LLVM_MINGW=/path/to/llvm-mingw \
SDL2_MINGW=/path/to/SDL2-2.x/x86_64-w64-mingw32 \
./build_windows.sh           # -> dist_windows/DarkRiddle/
```

Keep the `DarkRiddle` folder together: it holds the exe, the SDL2 and C++ runtime DLLs and
`romfs/`.

**Linux:**

```bash
sudo apt install g++ libsdl2-dev libgles2-mesa-dev libegl1-mesa-dev
./build_linux.sh
cd game && ./darkriddle_linux
```

## Controls

Every action except movement on the stick and Pause can be rebound in
**Main menu → Controls → Edit controls**.

| Action | Keyboard / mouse | Controller (PC) | Switch |
|---|---|---|---|
| Move | W A S D | Left stick | Left stick |
| Look | Mouse | Right stick | Right stick |
| Jump | Space | A | B |
| Run | Left Shift | L3 | R |
| Use / pick up | Left mouse | X | A |
| Throw | Right mouse | RT | X |
| Tasks | Tab | Y | Y |
| Pause | Esc | Start | + |

Two alternative schemes are built in: **Classic** (E to use, Q to throw) and
**Alternative pad** (use and throw on the triggers).

## Saves and settings

| | Progress | Controls |
|---|---|---|
| Switch | `sdmc:/switch/darkriddle.sav` | `sdmc:/switch/darkriddle_controls.cfg` |
| PC | `darkriddle.sav` next to the game | `darkriddle_controls.cfg` next to the game |

## How it works (short version)

- **Conversion:** the tools read the Unity 2018.1 asset files with
  [UnityPy](https://github.com/K0lb3/UnityPy). They export the meshes in world space with baked
  per-vertex lighting, and the materials, colliders, triggers and AudioClips. They also export
  the humanoid animation clips as muscle curves, with Unity's root-motion extraction applied,
  plus the UI canvases at 1280×720 and the NavMesh.
- **Engine:** a small GLES2 renderer; a collision triangle grid for the player and the
  physics; rigid-body physics for the throwable objects; and a muscle-space humanoid retargeter
  that skins the neighbour on the GPU.
- **Game logic:** the original scripts reimplemented in C++, including FirstPersonController,
  NeighborAI, DoorControl, LockScript, BreakableGlass and Menu.

## Disclaimer

This is an unofficial fan project. It is not affiliated with, endorsed by or connected to the
developers or publishers of Dark Riddle. *Dark Riddle* and all related names, characters and
assets belong to their respective owners. No copyrighted game content is distributed here. Use
this project only with a copy of the game you legally own.
