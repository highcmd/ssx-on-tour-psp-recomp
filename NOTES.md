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
- `SSX - On Tour (USA) (En,Fr,De).zip` → `iso/` → `extracted/` (7z).
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
    .venv/bin/python tools/gen_nids.py third_party/ppsspp-1.20.4 data/nids.json   # once
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

## Licensing note
`runtime/vfpu/` is ported from PPSSPP (GPL-2.0+), so the runtime is GPL-2.0+. Game data/code is never
committed (see .gitignore).
