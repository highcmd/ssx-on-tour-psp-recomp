# SSX On Tour (PSP, USA) — static recompilation to PC

Goal: translate the game's MIPS (Allegrex) code to C++ ahead of time and run it natively on PC
with a custom runtime (HLE kernel, GE→OpenGL, audio via SDL).

## Machine / environment (2026-10-15)
- Debian 13 x86_64, gcc 14, clang 19, cmake 3.31, SDL2-dev (apt).
- `~/pspdev` — pspdev toolchain v20261001 (psp-objdump etc.).
- `~/.local/bin/PPSSPP.AppImage` — PPSSPP 1.20.4, reference emulator (yes/no checks only).
- `~/.local/bin/uv` + `.venv/` — Python tools (rabbitizer 1.16.2, pyelftools).
- `third_party/ppsspp-1.20.4/` — PPSSPP source (GPL-2.0+): NID names, VFPU interpreter, HLE semantics reference.
- User has a real PSP with CFW (model/firmware: not yet asked).

## Input
- `tools/setup_from_iso.py` (via `./setup.sh <dump>` / `setup.bat`): zip → `iso/`, verifies ULUS-10042 + BOOT.BIN SHA-1,
  extracts only BOOT.BIN (+UMD_DATA.BIN, PARAM.SFO) to `extracted/`. The runtime reads disc0: from the ISO
  (ISO 9660 index in hle_io.cpp; the game itself mostly reads raw `umd0:` sectors).
- `extracted/PSP_GAME/SYSDIR/BOOT.BIN`: **unencrypted** ELF PRX (type 0xFFA0), module `ssxpsp` v1.1.
  EBOOT.BIN is the same program encrypted (~PSP) — ignore it.
- No symbol table, but section names, full relocations, RTTI class names and source paths survive.

## Pipeline
1. `tools/gen_nids.py` → `data/nids.json` (NID→name from PPSSPP HLE tables).
2. `tools/psp_prx.py` — loader: segments at base 0x08804000, PSP relocations (HI16 pairs with next
   non-HI16 like the PSP loader), sceModuleInfo, imports (230, all but sceNetInet named), exports.
3. `tools/analyze.py` → `data/functions.json`: seeds = entry/exports/jal targets/data pointers
   (.data/.linkonce.d/.cplinit/.ctors)/HI16-LO16 code addresses; fixpoint trace; jump tables from
   `lui %hi(tab)…lw %lo(tab)…jr`. Result: 10400 functions, 243 jump tables, 0 stray code pointers,
   0 unresolved `jr reg`, 100 % code coverage.
4. `tools/recomp.py` → `build/gen/*.cpp` (one C++ function per guest function).
5. `runtime/` — C++ runtime, CMake.

## Facts
- Opcode census: 180 distinct mnemonics, ~80 VFPU ones (vdot, vtfm4, vmmul, vcrsp, vsqrt, vsin/vcos once…).
  No `syscall` in game code: all kernel calls go through the 8-byte import stubs.
- Imports by count: ThreadMan 33, sceMpeg 23, sceNetInet 19, sceSasCore 18, sceUtility 17, sceGe 15,
  IoFileMgr 14, Adhocctl 10, sceAudio 9, Atrac3plus 8 …

## Build & run
    ./setup.sh <your ULUS-10042 dump>   # everything below, from the dump (docs/BUILDING.md)
    .venv/bin/python tools/gen_nids.py third_party/ppsspp-1.20.4 data/nids.json   # only to regenerate the committed table
    .venv/bin/python tools/analyze.py && .venv/bin/python tools/recomp.py
    cmake -S . -B build/cmake -G Ninja -DCMAKE_CXX_COMPILER=clang++ && ninja -C build/cmake ssx
    ./run.sh [--fps 30|60|120] [--res WxH] [--fullscreen] [--filter nearest] ...   # or ssx.ini (see ssx.ini.example)
Debug env vars: SSX_LOG=kernel,thread,io,display,ge,audio,misc  SSX_SHOT_EVERY=s (PPM shots)
SSX_INPUT="25=start,30=cross:0.3" (scripted pad)  SSX_FPS=s (fps meter)  SSX_GETRACE_AT=s (one-frame GE trace)
SSX_VBTRACE_AT=s (vblank/GE/wait timeline)  SSX_GE_THREADS=n  --headless

## Status (2026-10-15)
- Boots, plays both intro movies (FFmpeg H.264), title, profile creation (OSK), menus, loading, races.
- Runtime: fiber kernel, async GE thread + pipelined banded multi-threaded software rasteriser
  (writes guest VRAM directly), SDL2 window/gamepad, settings (ssx.ini / CLI).
- NOT done yet: audio output (SAS/ATRAC/sceAudio are timed stubs, silent), movie audio,
  real savedata (load reports "no data", save pretends success), network, internal resolution upscaling
  (output resolution only), lines/points are approximated, bezier/spline unimplemented (unused so far).

## Findings worth remembering
- Frame pacing: 0x08B214C4 = engine update rate in Hz (60 in menus, 30 in races, 20 fallback).
  F_0886E774(timerObj, hz) stores it, reprograms the game alarm (F_08A94390, period 1e6/hz) and derives
  dt = 1/hz -> the simulation is time-based. The flip (F_08976494) always waits 2 vblanks -> 30 fps max
  on a PSP. hook_SetGameRate (data/hooks.json) lifts race rate to 60/120 and runs the virtual vblank at
  4 x hz (2 x hz loses a tick ~1 in 5 because the flip can end after the next tick). Race timer verified
  1:1 with wall clock at 60 and 120.
- 60 mode measures ~50 fps: about every 100 ms user_main busy-yields (F_08A9489C(0) = ReferThreadStatus +
  RotateThreadReadyQueue) for ~19 ms and a tick is lost. Not CPU bound (all threads <20 % busy). TODO.
- This PC: kernel clocksource is HPET (TSC not synchronised across cores) -> clock_gettime is a syscall
  and rdtsc gives backwards time; rt_time_us() reads an atomic updated by a 100 us ticker thread.
- Through-mode Z is unsigned (u16) — signed reading broke all depth tests.
- Game alarm must be rescheduled from the scheduled time (drift lost frames).
- Raster cost ~60 ns/px (775k px/frame in races); GE list 9-13 ms; guest code itself ~1-5 ms/frame.

## Internal resolution (render_scale)
- Every GE colour/depth target gets an S x S shadow (RGBA8888 / u16) in ge_render.cpp; vertices,
  scissor and through-mode coords are scaled; the display shows the shadow.
- Shadows are averaged back into VRAM at every FINISH (only the drawn rectangle) and before a texture
  or block transfer reads from them. Writing undrawn/stale shadow areas back corrupted display lists
  the game keeps in VRAM (it CALLs lists at 0x040f1200 / 0x041de200) -> runaway GE list.
- Speed (races, this PC, 15 raster threads): S=1 60 fps in 60 mode; S=2 ~30; S=3 ~18; S=4 ~11.
  ~45 ns/px software shading is the limit -> needs SIMD spans or a GPU backend for high S.
- Bezier patches (cmd 0x05) draw the snow terrain; control points are skinned (vtype 0x780).

## GPU renderer (default, --renderer gl)
- CPU side unchanged (decode, skinning, Bezier, lighting, fog, near clip); ge_gl.cpp rasterises:
  one FBO per PSP colour target address (RGBA8, 512*S square) + D24S8 per Z address.
- Shader: texture functions, colour doubling, fog, alpha test, Z range; PSP blend factors mapped to GL
  (doubled src alpha / distinct FIX colours folded into a shader pre-multiply).
- PSP stencil == framebuffer alpha: alpha writes are mirrored into GL stencil with
  GL_ARB_shader_stencil_export (1-bit for 5551, 4-bit for 4444); STE uses real GL stencil ops.
- Contexts: GE thread (hidden 1x1 window, SDL on X11 has no surfaceless) and a present thread on the
  game window; GL fences at FINISH. Render-to-texture samples the FBO texture when the texture
  address equals a target (not used in races; VRAM textures there are real uploads).
- No VRAM writeback of GPU-rendered pixels (avoids the display-list corruption seen with shadows).
- Races at 1920x1088 (S=4): 30/60/120 fps all hold; S=6 holds 60.
- PSP DXT3/DXT5 store the colour block first, then alpha (fixed in both renderers).

## Savedata (hle_savedata.cpp)
- Host memory stick: `--ms <dir>` (default `./ms0` in the working dir) -> `PSP/SAVEDATA/ULUS10042<slot>/`
  with DATA.BIN, PARAM.SFO (PSP keys: CATEGORY, PARENTAL_LEVEL, SAVEDATA_DETAIL/DIRECTORY/FILE_LIST/
  PARAMS/TITLE, TITLE), ICON0.PNG (trimmed at IEND), ICON1.PMF.
- SSX uses the V2 param struct (0x5DC, no key): LISTSAVE when the profile is created/saved, LISTLOAD
  after START. List modes pick the slot the system dialog would focus (no UI drawn).
- Data is stored unencrypted, so saves are not yet interchangeable with a real PSP. Doing that needs
  the PSP savedata crypto (sceChnnlsv + KIRK, mode 1 for key-less games) and the SFO hashes, as PPSSPP has.

## Windows port (2026-10-15)
- `runtime/src/platform.{h,cpp}`: guest memory (mmap / VirtualAlloc), fibers (ucontext / Win32 fibers with
  FIBER_FLAG_FLOAT_SWITCH; a restarted thread gets a fresh fiber), thread names, exe dir, localtime.
  Directories via std::filesystem with UTF-8 paths (`u8p`/`u8s`; the exe manifest sets the UTF-8 code page).
- Windows: rt_time_us() reads QPC directly (no ticker thread: Sleep is too coarse); timeBeginPeriod(1).
- Build: llvm-mingw 20260922 (UCRT) + SDL2 2.32.10 mingw + BtbN FFmpeg n8.1 shared (`tools/win_deps.py`,
  `cmake/llvm-mingw.cmake`); `SSX_PORTABLE`: image.bin/assets next to the exe, which is also the cwd.
  `setup --target windows` assembles `dist/ssx-windows-x64`. Verified under Wine 11 (boot → race, saves, pad).

## Licensing note
`runtime/vfpu/` is ported from PPSSPP (GPL-2.0+), so the runtime is GPL-2.0+. Game data/code is never
committed (see .gitignore).
