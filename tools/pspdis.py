#!/usr/bin/env python3
"""Disassemble guest code: dis.py <addr|function-containing-addr> [count]
Annotates jal targets with import names / function names."""
import json, pathlib, sys
import rabbitizer as R
import psp_prx

ROOT = pathlib.Path(__file__).resolve().parent.parent
img = psp_prx.load(ROOT / "extracted/PSP_GAME/SYSDIR/BOOT.BIN", ROOT / "data/nids.json")
funcs = json.load(open(ROOT / "data/functions.json"))["functions"]
imports = {i.stub: i.name for i in img.imports}
a = int(sys.argv[1], 16)
f = next((f for f in funcs if f["start"] <= a < f["end"]), None)
start = f["start"] if f and len(sys.argv) < 3 else a
end = f["end"] if f and len(sys.argv) < 3 else a + 4 * int(sys.argv[2])
if f: print(f"; function {f['start']:#x}-{f['end']:#x} ({f['kind']})")
for x in range(start, end, 4):
    w = img.u32(x)
    ins = R.Instruction(w, vram=x, category=R.InstrCategory.R4000ALLEGREX)
    note = ""
    if (w >> 26) in (2, 3):
        t = ((x + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        note = imports.get(t, f"F_{t:08X}")
    r = img.code_relocs.get(x)
    if r and r[0] == 5 and r[1] is not None:
        note = f"hi -> {r[1]:#x}"
        s = img.section_of(r[1])
        if s and s.startswith(".rodata"):
            try:
                txt = img.cstr(r[1])
                if 3 <= len(txt) < 60 and txt.isprintable(): note += f' "{txt}"'
            except Exception: pass
    print(f"{x:08x}: {w:08x}  {ins.disassemble():40s} {'; ' + note if note else ''}")
