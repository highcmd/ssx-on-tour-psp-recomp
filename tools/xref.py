#!/usr/bin/env python3
"""xref.py <addr> : code locations that build this address with lui/addiu|lw|sw (HI16/LO16 pairs)."""
import json, pathlib, sys
import rabbitizer as R
import psp_prx
ROOT = pathlib.Path(__file__).resolve().parent.parent
img = psp_prx.load(ROOT / "extracted/PSP_GAME/SYSDIR/BOOT.BIN", ROOT / "data/nids.json")
funcs = json.load(open(ROOT / "data/functions.json"))["functions"]
target = int(sys.argv[1], 16)
span = int(sys.argv[2], 16) if len(sys.argv) > 2 else 4
for a, (t, full) in sorted(img.code_relocs.items()):
    if t == 5 and full is not None and target - 0x8000 <= full <= target + 0x8000:
        # scan following instructions for lo uses with this hi register
        reg = (img.u32(a) >> 16) & 31
        hi = img.u32(a) & 0xFFFF
        for x in range(a + 4, a + 80, 4):
            w = img.u32(x)
            if ((w >> 21) & 31) == reg:
                lo = w & 0xFFFF; lo = lo - 0x10000 if lo & 0x8000 else lo
                ea = ((hi << 16) + lo) & 0xFFFFFFFF
                if target <= ea < target + span:
                    f = next((f["start"] for f in funcs if f["start"] <= x < f["end"]), 0)
                    print(f"{x:08x} in F_{f:08X}: {R.Instruction(w, vram=x, category=R.InstrCategory.R4000ALLEGREX).disassemble()}")
