# SSX On Tour — native PC port (static recompilation)

A static recompiler and runtime for the PSP game **SSX On Tour** (USA, `ULUS-10042`). The game's MIPS
code is translated to C++ ahead of time and runs natively on x86-64 Linux and Windows. The runtime provides a
PSP kernel/HLE layer, an OpenGL 3.3 renderer for the GE, audio through SDL, and gamepad support.

**This repository contains no game data and no game code.** You need your own copy of the game,
dumped from a UMD you own. The setup script reads that dump and generates everything locally:
the function analysis, the recompiled C++, and the executable. None of
this output is tracked by git (see `.gitignore`), so please don't commit it, upload it or share it.

## Quick start

You need **SSX On Tour, USA release (`ULUS-10042`)** dumped to an `.iso` from your own UMD with a PSP
running custom firmware.

### Windows

1. Install [Python 3.10+](https://www.python.org/downloads/). That is the only prerequisite: the setup
   downloads the compiler and the libraries itself.
2. Download this repository (Code → Download ZIP, or `git clone`) and, in its folder, run:

   ```bat
   setup.bat D:\Games\ssx-on-tour.iso
   ```

3. Start `dist\ssx-windows-x64\ssx.exe`.

### Linux

1. Install the build dependencies. On Debian/Ubuntu:

   ```sh
   sudo apt install clang cmake ninja-build pkg-config python3-venv \
       libsdl2-dev libavcodec-dev libavutil-dev libswscale-dev
   ```

   Other distributions are covered in [docs/BUILDING.md](docs/BUILDING.md).
2. Build from your dump. This takes about a minute and is needed only once:

   ```sh
   ./setup.sh /path/to/your/ssx-on-tour.iso      # a .zip holding the .iso also works
   ```

3. Play:

   ```sh
   ./run.sh                                       # 960×544 window, original 30 fps
   ./run.sh --fps 60 --res 1920x1080 --fullscreen
   ```

`./setup.sh <iso> --target windows` also builds the Windows version from Linux or WSL.

The setup checks that the image is the right game (game ID and the SHA-1 of `BOOT.BIN`), recompiles
the game into C++ and builds it. At run time the game reads its files straight from the ISO.

## Documentation

- [docs/BUILDING.md](docs/BUILDING.md): dependencies, dumping, every `setup.sh` option, the Windows
  build (native and cross), what gets generated, building by hand, troubleshooting
- [docs/OPTIONS.md](docs/OPTIONS.md): every setting (`ssx.ini` and flags), resolution and frame
  rate, controls and gamepads, saves, debugging variables
- [NOTES.md](NOTES.md): technical notes on the recompiler and the runtime

## Features

- Native x86-64 code, translated ahead of time from the game's MIPS (Allegrex) executable
- Rendering on the GPU (OpenGL 3.3) at a real internal resolution of up to 2880×1632
  (`--render-scale 1..6`), not an upscaled 480×272 image
- 30 (original), 60 or 120 fps in races, with physics and race timing kept correct
- Linux and Windows x64 builds
- Xbox-style gamepads (hot-plug) and the keyboard
- ATRAC3+ music, PSMF/H.264 movies, saves on a virtual memory stick
- Not done yet: sound effects (SAS), saves that work on a real PSP, the system dialog UI, network play

## How it works

See [NOTES.md](NOTES.md). The pipeline lives in `tools/`: `psp_prx.py` (PRX loader), `analyze.py`
(function discovery), `recomp.py` (MIPS → C++) and `setup_from_iso.py` (the whole flow). The C++
runtime lives in `runtime/`.

## License

The code in this repository is licensed under the GNU General Public License v2.0 or later (`LICENSE`).
The Windows build ships SDL2 (zlib license) and FFmpeg (GPL build) DLLs; their licenses are copied
into the build folder.
`runtime/vfpu/`, `runtime/assets/vfpu/` and the NID table `data/nids.json` (generated with
`tools/gen_nids.py`) come from [PPSSPP](https://github.com/hrydgard/ppsspp), © PPSSPP Project,
under GPL-2.0-or-later. Many HLE semantics were checked against PPSSPP's source.

SSX On Tour is © Electronic Arts. This project is not affiliated with or endorsed by Electronic
Arts or Sony. "PSP" is a trademark of Sony Interactive Entertainment.
