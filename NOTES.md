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

## Licensing note
`runtime/vfpu/` is ported from PPSSPP (GPL-2.0+), so the runtime is GPL-2.0+. Game data/code is never
committed (see .gitignore).
