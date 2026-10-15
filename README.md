# SSX On Tour — native PC port (static recompilation)

A static recompiler and runtime for the PSP game **SSX On Tour** (USA, `ULUS-10042`). The game's MIPS
code is translated to C++ ahead of time and runs natively on x86-64 Linux. The runtime provides a
PSP kernel/HLE layer, an OpenGL 3.3 renderer for the GE, audio through SDL, and gamepad support.

**This repository contains no game data and no game code.** You need your own copy of the game,
dumped from a UMD you own. The setup script reads that dump and generates everything locally:
the extracted UMD file system, the function analysis, the recompiled C++, and the executable. None of
this output is tracked by git (see `.gitignore`), so please don't commit it, upload it or share it.

## Requirements

- Linux x86-64, a GPU with OpenGL 3.3 (`GL_ARB_shader_stencil_export` is used when available)
- clang (C++20), CMake ≥ 3.20, Ninja
- SDL2 and FFmpeg development packages (libavcodec, libavutil, libswscale), pkg-config
- Python ≥ 3.10 (the setup creates a venv with [rabbitizer](https://github.com/Decompollaborate/rabbitizer))

On Debian/Ubuntu:

```sh
sudo apt install clang cmake ninja-build pkg-config python3-venv \
    libsdl2-dev libavcodec-dev libavutil-dev libswscale-dev
```

## Building from your dump

1. Dump your UMD to an ISO with a PSP running custom firmware (for example, the "UMD → ISO" function of
   your CFW's VSH menu, or an ISO-dumping homebrew). Only the retail USA release is supported. The
   script checks the game ID and the SHA-1 of `BOOT.BIN`, and it rejects other revisions and modified images.
2. Run:

   ```sh
   ./setup.sh /path/to/your/ssx-on-tour.iso      # a .zip holding the .iso also works
   ```

   This takes a few minutes. It extracts the UMD, analyses `BOOT.BIN`, recompiles it into
   `build/gen/`, and builds `build/cmake/ssx`. You can run it again at any time: completed steps are
   skipped, and `--force` redoes them.
3. Play:

   ```sh
   ./run.sh                          # or e.g. ./run.sh --fps 60 --res 1920x1088 --fullscreen
   ```

Saves go to `run/ms0/PSP/SAVEDATA/`. Settings can go in `run/ssx.ini` (see `ssx.ini.example`), and
`./run.sh --help` lists every option.

## Controls

| PSP | Keyboard | Xbox-style pad |
|---|---|---|
| D-pad | arrow keys | D-pad (or the right stick with `--right-stick-dpad`) |
| Analog nub | I J K L | left stick |
| ✕ ○ □ △ | Z X A S | A B X Y |
| L / R | Q / E | LB / RB or LT / RT |
| Start / Select | Enter / Backspace | Start / Back |

F11 toggles fullscreen.

## Features

- Frame rate of 30 (original), 60 or 120 fps, with the game's own tick raised so physics and race
  timing stay correct
- True internal resolution rendering on the GPU (`--render-scale N` = 480×272·N), not upscaling
- ATRAC3+ music, PSMF movies, real save files
- Not done yet: sound effects (SAS), saves that work on a real PSP, system dialog UI, network play

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
