# NBA Street Homecourt — PC port (playtest)

An unofficial native Windows port of **NBA Street Homecourt** (Xbox 360, USA, title ID `454107F7`),
built with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp). The game's code is statically
recompiled from **your own copy** of the game; this repository contains only the port's runtime
(graphics, audio, input, file system, mod loader) and build setup.

> **No game files are included or distributed.** You must own the game and dump it yourself.
> This project is not affiliated with or endorsed by Electronic Arts, the NBA or any team or player.
> Do not open issues or pull requests containing game data, recompiled game code or built executables.

## What you need

- An Xbox 360 copy of NBA Street Homecourt (USA) that you own, dumped to a folder (the folder that
  contains `default.xex` and `data/`).
- Windows 10/11 with a Direct3D 11 GPU.
- To build: [LLVM/Clang](https://releases.llvm.org/) (clang-cl), CMake 3.20+, Ninja, and Python 3.

## Building

```bash
git clone --recursive https://github.com/<you>/nba-street-homecourt-pc.git
cd nba-street-homecourt-pc

# 1. copy YOUR default.xex into the repo root (it is git-ignored)
# 2. build XenonRecomp and recompile the game code into ppc/ (git-ignored)
cmake -S thirdparty/XenonRecomp -B build-recomp -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-recomp
build-recomp/XenonRecomp/XenonRecomp nbash.toml thirdparty/XenonRecomp/XenonUtils/ppc_context.h

# 3. build the port
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build
```

Put the resulting `nbash.exe` next to a `game/` folder holding your dumped game
(`game/default.xex`, `game/data/...`) and run it.

The movie player uses FFmpeg's shared libraries (`avcodec`, `avformat`, `avutil`, `swscale`,
`swresample`); download a Windows LGPL build from https://ffmpeg.org/download.html and place the DLLs
next to the exe (and the import libraries/headers in `thirdparty/ffmpeg-win/` when building).

## Features added on top of the console game

| Feature | Default | Switch off |
|---|---|---|
| Right-stick dribble moves (crossovers, spins, behind the back; hold turbo + flick for Y tricks) | on | `NBASH_RSTICK=off` |
| Dunk only with turbo (no turbo near the rim = layup) | on | `NBASH_TURBODUNK=0` |
| Random dunk/layup styles | on | `NBASH_NORANDOMDUNK=1`, `NBASH_NORANDOMLAYUP=1` |
| Master dunkers can use every dunk class (small man / high-flyer / big man) | on | `NBASH_ANYDUNK=0` |
| Regular players limited to simpler dunk/layup styles | on | `NBASH_RATINGMOVES=0` |
| Light left-stick push walks, full push runs | on | `NBASH_WALK=0` (`NBASH_WALK_PCT` sets the split) |
| Player ratings screen in the menus (LB / Tab) | on | — |
| Colour grading (F8 cycles original / natural / vivid) | natural | `NBASH_COLORS=0` |

Set switches in a small `.bat` launcher, e.g.:

```bat
@echo off
cd /d "%~dp0"
set NBASH_ANYDUNK=0
start "" nbash.exe
```

## Mods

Files placed in `game/mods/<archive name>/<path inside the archive>` replace or add files inside the
game's `.big` archives without touching the originals (for example
`game/mods/charplatform/data/characters/textures/...`). Share mods made from your own work only.

## Playtesting

Please report crashes, graphics glitches and gameplay issues in the Issues tab with your Windows
version, GPU, and the `nbash_log.txt` written next to the exe. Never attach game files.

## Credits

- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) (MIT) — the static recompiler.
- Parts of [Xenia](https://github.com/xenia-project/xenia) (BSD) — see `thirdparty/xenia/LICENSE`.
- Fonts used by the ratings screen: Roboto Slab and Permanent Marker (Apache 2.0) — see `third_party/fonts/`.
