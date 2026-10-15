# SSX On Tour — native PC port (static recompilation)

A static recompiler and runtime for the PSP game **SSX On Tour** (USA, `ULUS-10042`). The game's MIPS
code is translated to C++ ahead of time and runs natively on x86-64 Linux. The runtime provides a
PSP kernel/HLE layer, an OpenGL 3.3 renderer for the GE, audio through SDL, and gamepad support.

**This repository contains no game data and no game code.** You need your own copy of the game,
dumped from a UMD you own. The setup script reads that dump and generates everything locally:
the extracted UMD file system, the function analysis, the recompiled C++, and the executable. None of
this output is tracked by git (see `.gitignore`), so please don't commit it, upload it or share it.

## Quick start

1. Install the build dependencies. On Debian/Ubuntu:

   ```sh
   sudo apt install clang cmake ninja-build pkg-config python3-venv \
       libsdl2-dev libavcodec-dev libavutil-dev libswscale-dev
   ```

   Other distributions are covered in [docs/BUILDING.md](docs/BUILDING.md).
2. Dump your UMD of the **USA release (`ULUS-10042`)** to an `.iso` with a PSP running custom firmware.
3. Build from your dump. This takes about a minute and only needs to be done once:

   ```sh
   ./setup.sh /path/to/your/ssx-on-tour.iso      # a .zip holding the .iso also works
   ```

   The script checks that the image is the right game (game ID and the SHA-1 of `BOOT.BIN`),
   extracts it, recompiles the game into C++ and builds `build/cmake/ssx`.
4. Play:

   ```sh
   ./run.sh                                       # 960×544 window, original 30 fps
   ./run.sh --fps 60 --res 1920x1080 --fullscreen
   ```

## Documentation

- [docs/BUILDING.md](docs/BUILDING.md): dependencies, dumping, every `setup.sh` option, what gets
  generated, building by hand, troubleshooting
- [docs/OPTIONS.md](docs/OPTIONS.md): every setting (`ssx.ini` and flags), resolution and frame
  rate, controls and gamepads, saves, debugging variables
- [NOTES.md](NOTES.md): technical notes on the recompiler and the runtime

## Features

- Native x86-64 code, translated ahead of time from the game's MIPS (Allegrex) executable
- Rendering on the GPU (OpenGL 3.3) at a real internal resolution of up to 2880×1632
  (`--render-scale 1..6`), not an upscaled 480×272 image
- 30 (original), 60 or 120 fps in races, with physics and race timing kept correct
- Xbox-style gamepads (hot-plug) and the keyboard
- ATRAC3+ music, PSMF/H.264 movies, saves on a virtual memory stick
- Not done yet: sound effects (SAS), saves that work on a real PSP, the system dialog UI, network play

## How it works

See [NOTES.md](NOTES.md). The pipeline lives in `tools/`: `psp_prx.py` (PRX loader), `analyze.py`
(function discovery), `recomp.py` (MIPS → C++) and `setup_from_iso.py` (the whole flow). The C++
runtime lives in `runtime/`.

## License

The code in this repository is licensed under the GNU General Public License v2.0 or later (`LICENSE`).
`runtime/vfpu/`, `runtime/assets/vfpu/` and the NID table `data/nids.json` (generated with
`tools/gen_nids.py`) come from [PPSSPP](https://github.com/hrydgard/ppsspp), © PPSSPP Project,
under GPL-2.0-or-later. Many HLE semantics were checked against PPSSPP's source.

SSX On Tour is © Electronic Arts. This project is not affiliated with or endorsed by Electronic
Arts or Sony. "PSP" is a trademark of Sony Interactive Entertainment.
