#!/usr/bin/env python3
"""Build a NID -> function name database from PPSSPP's HLE tables.

Usage: gen_nids.py <ppsspp-src-dir> <out.json>
Output: {"<Module>": {"0xNID": {"name": ..., "ret": 'i', "args": "xx"}}}
"""
import json, re, sys, pathlib

src = pathlib.Path(sys.argv[1]) / "Core" / "HLE"
table_re = re.compile(r'(?:static\s+)?const\s+HLEFunction\s+(\w+)\s*\[\s*\]\s*=\s*\{(.*?)\n\s*\};', re.S)
entry_re = re.compile(r'\{\s*(0[xX][0-9A-Fa-f]{8})\s*,\s*([^,]*?)\s*,\s*"(\w+)"\s*(?:,\s*\'(.)\'\s*(?:,\s*"([^"]*)")?)?')
reg_re = re.compile(r'"(\w+)"\s*,\s*ARRAY_SIZE\((\w+)\)\s*,\s*(\w+)\s*[)}]')

tables, regs = {}, []
for f in sorted(src.glob("*.cpp")):
    text = f.read_text(errors="replace")
    for m in table_re.finditer(text):
        tables[m.group(1)] = [
            (int(e.group(1), 16), e.group(3), e.group(4) or "", e.group(5) or "", e.group(2) not in ("0", "nullptr", "NULL"))
            for e in entry_re.finditer(m.group(2))]
    regs += reg_re.findall(text)

db = {}
for mod, _, tab in regs:
    for nid, name, ret, args, impl in tables.get(tab, []):
        db.setdefault(mod, {})[f"0x{nid:08X}"] = {"name": name, "ret": ret, "args": args, "ppsspp_impl": impl}
json.dump(db, open(sys.argv[2], "w"), indent=1, sort_keys=True)
print(f"{len(db)} modules, {sum(map(len, db.values()))} functions")
