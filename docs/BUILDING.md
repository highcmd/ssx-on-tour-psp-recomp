# Building and running

This page covers the full process, from your UMD to a running game. For a short version, see the
[README](../README.md).

The game code is generated from your own dump, so nobody can hand you a ready-made `ssx.exe`: every
player builds it once, which takes a minute or two. You can build for:

- **Linux x86-64:** natively (sections 1–4).
- **Windows x64:** natively with only Python installed, or cross-compiled from Linux/WSL
  ([Windows](#windows-x64)).

## 1. Install the dependencies

The build needs:

| What | Why |
|---|---|
| clang (C++20) | compiles the runtime and the ~10 400 recompiled game functions |
| CMake ≥ 3.20 and Ninja | build system |
| pkg-config | finds FFmpeg |
| SDL2 (dev) | window, OpenGL context, audio, gamepads |
| FFmpeg dev libraries: libavcodec, libavutil, libswscale | ATRAC3+ music and H.264 movies |
| Python ≥ 3.10 with venv | the recompiler (`setup.sh` installs its one package, `rabbitizer`) |

**Debian / Ubuntu**

```sh
sudo apt install clang cmake ninja-build pkg-config python3-venv \
    libsdl2-dev libavcodec-dev libavutil-dev libswscale-dev
```

**Fedora**

```sh
sudo dnf install clang cmake ninja-build pkgconf python3 \
    SDL2-devel ffmpeg-free-devel      # or ffmpeg-devel from RPM Fusion
```

**Arch**

```sh
sudo pacman -S clang cmake ninja pkgconf python sdl2 ffmpeg
```

To run the game you need a GPU and driver with OpenGL 3.3 core. Mesa (AMD/Intel), NVIDIA and
llvmpipe all work. Without one, use the CPU renderer: `--renderer software`.

## 2. Dump your game

You need **SSX On Tour, USA release (`ULUS-10042`)** as an ISO dumped from your own UMD:

- On a PSP with custom firmware, use your CFW's UMD-to-ISO dump function or an ISO-dumping
  homebrew, or connect the PSP over USB in a mode that exposes the UMD as a drive and copy the
  image. Then copy the `.iso` to your PC.
- The file must be a raw `.iso`. A `.zip` holding exactly one `.iso` is accepted as well.
  Compressed formats (`.cso`, `.zso`) are not; decompress them first, for example with `maxcso --decompress`.

The setup checks the image before it uses it: the game ID in `UMD_DATA.BIN` must be `ULUS-10042`,
and `PSP_GAME/SYSDIR/BOOT.BIN` must have SHA-1 `74d1360e3111283fe937185ef23fa8d901bb0085`. Other
regions (the European and Japanese releases), demos and patched images are rejected, because the
recompiled code and the hooks are tied to that exact executable.

## 3. Run the setup

```sh
git clone https://github.com/highcmd/ssx-on-tour-psp-recomp.git
cd ssx-on-tour-psp-recomp
./setup.sh ~/Downloads/ssx-on-tour.iso
```

The setup takes about a minute on a modern 8-core CPU (the Windows build adds a one-time download
of about 300 MB). It prints each step:

```
==> checking the image
    ULUS-10042, BOOT.BIN 74d1360e3111… OK
==> extracting BOOT.BIN to extracted/
    3 files, 4 MiB
==> analysing BOOT.BIN (function discovery)
==> recompiling MIPS to C++ (build/gen)
    10400 functions in 70 files; 0 unhandled instructions; VFPU handlers: 29
==> configuring and building
==> done. Start the game with ./run.sh (options: ./run.sh --help)
```

`setup.sh` options (passed through to `tools/setup_from_iso.py`):

| Option | Effect |
|---|---|
| `--no-build` | stop after generating `build/gen/`, for example to build with your own CMake flags |
| `--force` | extract again and redo the analysis, even if the results are already there |
| `--target linux\|windows` | what to build; the default is the system you run it on ([Windows](#windows-x64)) |
| `--extract-all` | unpack the whole UMD to `extracted/`, only needed to run with `--umd extracted` |
| `-j N` | number of parallel compile jobs (default: Ninja's choice) |

Running `./setup.sh` again is safe. Finished steps are skipped, and the recompiler and the build only
redo what changed.

### What gets generated

All of these come from your dump, and all of them are in `.gitignore`. Never commit or share them.

| Path | Contents |
|---|---|
| `iso/` | the image (unpacked from the zip, or a symlink to your `.iso`) |
| `extracted/` | `BOOT.BIN` and two small headers for the recompiler |
| `data/functions.json` | function boundaries and jump tables found in `BOOT.BIN` |
| `build/gen/` | the recompiled C++ (`recomp_*.cpp`) and the initial memory image `image.bin` |
| `build/cmake/ssx` | the Linux executable |
| `build/win64/`, `dist/ssx-windows-x64/` | the Windows build and the ready-to-run folder |
| `third_party/win64/` | the downloaded Windows compiler and libraries |
| `run/` | runtime working directory on Linux: saves, `ssx.ini`, `ssx.log` |

At run time the game reads its files straight from the ISO; nothing else is unpacked. The Linux
executable has absolute paths to `build/gen/image.bin`, the ISO and `runtime/assets/` built in. If
you move the checkout or the ISO, run `./setup.sh` again, or pass the paths explicitly (see
[OPTIONS.md](OPTIONS.md#paths)).

## 4. Play

```sh
./run.sh                                   # 960×544 window, 30 fps (original)
./run.sh --fps 60 --res 1920x1080 --fullscreen
```

`run.sh` starts `build/cmake/ssx` inside `run/`, so saves, the log and `ssx.ini` stay in one place.
All the options are described in [OPTIONS.md](OPTIONS.md).

To quit, close the window or press Ctrl+C in the terminal. The game exits immediately; it saves on
its own after races and in the menus, as on the PSP.

## Windows x64

The Windows build is a self-contained folder, `dist/ssx-windows-x64/`. It holds `ssx.exe`, the SDL2
and FFmpeg DLLs, `image.bin` and `assets/`, and it needs nothing installed to run. The toolchain
([llvm-mingw](https://github.com/mstorsjo/llvm-mingw): clang + lld for Windows) and the
libraries ([SDL2](https://github.com/libsdl-org/SDL) mingw package,
[FFmpeg](https://github.com/BtbN/FFmpeg-Builds) win64 shared build) are downloaded on first use into
`third_party/win64/` (about 300 MB). The llvm-mingw and SDL2 archives are checked against pinned
SHA-256 hashes.

### On Windows

1. Install [Python 3.10 or newer](https://www.python.org/downloads/). During installation, tick
   "Add python.exe to PATH" or keep the `py` launcher. Nothing else is needed: the setup installs
   CMake and Ninja into its own venv and downloads the compiler.
2. Get the source: `git clone https://github.com/highcmd/ssx-on-tour-psp-recomp.git`, or
   download the ZIP from GitHub and unpack it.
3. In that folder, run:

   ```bat
   setup.bat D:\Games\ssx-on-tour.iso
   ```

4. Start `dist\ssx-windows-x64\ssx.exe`. The setup writes the ISO location into `ssx.ini` there.
   You can move the whole `ssx-windows-x64` folder anywhere you like.

### Cross-compiling from Linux or WSL

```sh
./setup.sh /path/to/ssx-on-tour.iso --target windows
```

Then copy `dist/ssx-windows-x64/` to the Windows machine. Put your `.iso` into that folder next to
`ssx.exe`, or set `iso = D:\Games\ssx-on-tour.iso` in `ssx.ini` there. From WSL, the folder
is reachable at `\\wsl$\<distro>\<path>`; copy it to a Windows drive before playing.

The Windows build is compiled for any x86-64 CPU; `-march=native` is turned off so the folder works
on other PCs. It needs Windows 10 or newer and an OpenGL 3.3 driver (all current AMD, Intel and NVIDIA
drivers).

## Building by hand

`setup.sh` does the following. You can run the same steps yourself:

```sh
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/python tools/setup_from_iso.py <dump.iso> --no-build      # iso/, extracted/, data/, build/gen/
cmake -S . -B build/cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=clang++ -DSSX_ISO="$PWD/iso/<dump>.iso"
ninja -C build/cmake ssx
```

`tools/analyze.py` and `tools/recomp.py` can also be run on their own. Both read
`extracted/PSP_GAME/SYSDIR/BOOT.BIN`. After you edit `data/hooks.json` or `tools/recomp.py`, rerun
`recomp.py` and `ninja`.

The build is tested with clang (Linux) and llvm-mingw (Windows). The recompiled code is compiled at
`-O1` to keep the build time reasonable. The software GE renderer (`ge_render.cpp`) is built with
`-O3 -march=native` on Linux; `-DSSX_NATIVE_ARCH=OFF` turns that off, as the Windows build does.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `this is not SSX On Tour USA` / `BOOT.BIN has SHA-1 …` | wrong region or a modified image; dump the USA UMD again |
| `Run tools/recomp.py first` from CMake | `build/gen/` is missing; run `./setup.sh <dump>` |
| `cannot load …/image.bin` | the checkout moved; rerun `./setup.sh` or pass `--image` |
| `No UMD image found` (Linux) | the ISO given to `setup.sh` moved; pass `--iso` or set `iso = …` in `run/ssx.ini` |
| black window, `OpenGL` errors in the log | driver without GL 3.3 core; try `--renderer software` |
| Windows: `No UMD image found` | put the `.iso` next to `ssx.exe` or set `iso = …` in `ssx.ini` next to it |
| Windows: SmartScreen warns about `ssx.exe` | the file is unsigned because you built it yourself; choose "More info → Run anyway" |
| no sound | check that SDL sees an audio device (`SDL_AUDIODRIVER=pulseaudio ./run.sh`, or `pipewire`/`alsa`) |
| pad not detected | see [OPTIONS.md → Gamepads](OPTIONS.md#gamepads) |
| something else | run from a terminal and look at `run/ssx.log`; open an issue with its tail |
