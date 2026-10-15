#!/usr/bin/env python3
"""Function discovery for the loaded PRX.

Seeds:   module entry/exports, every jal target, code pointers stored in data
         (.data/.linkonce.d/.cplinit/.ctors: vtables, callbacks, static ctors),
         code addresses materialised with lui/addiu (HI16/LO16 relocs).
Bodies:  a function runs from its seed to the next seed; we trace its control
         flow to find the real end, resolve jump tables at `jr <reg>`, and turn
         any non-padding code left after the end into a new (orphan) function.
Output:  data/functions.json  {functions: [...], jumptables: {...}, stats}
"""
import bisect, json, sys, pathlib
import psp_prx

ROOT = pathlib.Path(__file__).resolve().parent.parent

OP_J, OP_JAL = 2, 3
BRANCH_OPS = {1, 4, 5, 6, 7, 20, 21, 22, 23}       # regimm, beq, bne, blez, bgtz, +likely
COP_BRANCH = {(17, 8), (18, 3)}                    # bc1x, bvx (VFPU branch is cop2 rs=8)


def branch_target(w, a):
    """Return target for a conditional branch word, else None."""
    op = w >> 26
    off = w & 0xFFFF
    off = off - 0x10000 if off & 0x8000 else off
    tgt = (a + 4 + (off << 2)) & 0xFFFFFFFF
    if op in BRANCH_OPS:
        if op == 1 and ((w >> 16) & 0x1F) not in (0, 1, 2, 3, 16, 17, 18, 19):
            return None
        return tgt
    if op == 17 and ((w >> 21) & 0x1F) == 8:   # bc1f/t/fl/tl
        return tgt
    if op == 18 and ((w >> 21) & 0x1F) == 8:   # bvf/bvt/bvfl/bvtl
        return tgt
    return None


def is_uncond_branch(w):
    op = w >> 26
    # beq $0,$0 (b) or bgez $0 / bal
    return (op == 4 and ((w >> 21) & 31) == 0 and ((w >> 16) & 31) == 0) or \
           (op == 1 and ((w >> 21) & 31) == 0 and ((w >> 16) & 31) in (1, 17))


class Analyzer:
    def __init__(self, img):
        self.img = img
        self.code_ranges = [img.sections[s] for s in (".text", "render_model")]
        stubs = [img.sections[n] for n in img.sections if n.startswith(".sceStub.text")]
        self.stub_lo = min(s for s, _ in stubs)
        self.stub_hi = max(s + n for s, n in stubs)
        self.import_at = {i.stub: i for i in img.imports}
        self.jumptables = {}       # jr address -> (table addr, [targets])
        self.notes = []

    def is_code(self, a):
        return any(s <= a < s + n for s, n in self.code_ranges)

    def word(self, a):
        return self.img.u32(a)

    # ------------------------------------------------------------------
    def seeds(self):
        img = self.img
        S = {img.entry: "entry"}
        for e in img.exports:
            if self.is_code(e.addr):
                S.setdefault(e.addr, "export")
        for a, (t, tgt) in img.code_relocs.items():
            if t == psp_prx.R_26 and (self.word(a) >> 26) == OP_JAL and self.is_code(tgt):
                S.setdefault(tgt, "jal")
        for a, tgt in img.ptr_relocs.items():
            if not self.is_code(tgt):
                continue
            sec = img.section_of(a)
            if sec in (".data", ".linkonce.d", ".cplinit", ".ctors", ".dtors", ".rodata.sceResident"):
                S.setdefault(tgt, "dataptr")
        for a, (t, full) in img.code_relocs.items():
            if t == psp_prx.R_HI16 and full is not None and self.is_code(full) and full not in S:
                S.setdefault(full, "hilo")
        # stubs are handled separately (imports), never as functions
        return {a: k for a, k in S.items() if not (self.stub_lo <= a < self.stub_hi)}

    # ------------------------------------------------------------------
    def find_jumptable(self, jr_addr, fstart, fend):
        """Find the table feeding `jr rX` at jr_addr.  GCC emits
        lui rT,%hi(tab) ... lw rX,%lo(tab)(rT) ... jr rX; the HI16 reloc tells us `tab`."""
        reg = (self.word(jr_addr) >> 21) & 31
        lw_addr = None
        a = jr_addr - 4
        while a >= max(fstart, jr_addr - 64):
            w = self.word(a)
            if (w >> 26) == 0x23 and ((w >> 16) & 31) == reg:
                lw_addr = a
                break
            a -= 4
        if lw_addr is None:
            return None
        # nearest preceding HI16 whose full address is in rodata/data
        a = lw_addr - 4
        while a >= max(fstart, lw_addr - 64):
            r = self.img.code_relocs.get(a)
            if r and r[0] == psp_prx.R_HI16 and r[1] is not None and not self.is_code(r[1]):
                tab = r[1]
                targets = []
                p = tab
                while p in self.img.ptr_relocs and fstart <= self.img.ptr_relocs[p] < fend:
                    targets.append(self.img.ptr_relocs[p])
                    p += 4
                return (tab, targets) if targets else None
            a -= 4
        return None

    def trace(self, start, limit):
        """Trace control flow from start, never past limit. Returns (end, tailcalls)."""
        seen, todo = set(), [start]
        maxa = start
        tails = set()
        while todo:
            a = todo.pop()
            while start <= a < limit and a not in seen:
                seen.add(a)
                maxa = max(maxa, a)
                w = self.word(a)
                op = w >> 26
                bt = branch_target(w, a)
                if bt is not None:
                    maxa = max(maxa, a + 4)
                    if start <= bt < limit:
                        todo.append(bt)
                    else:
                        tails.add(bt)
                    if is_uncond_branch(w) and not (op == 1 and ((w >> 16) & 31) in (17,)):
                        break
                    a += 4  # likely branches may skip the delay slot; fallthrough after it
                    continue
                if op == OP_J:
                    tgt = ((a + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                    seen.add(a + 4); maxa = max(maxa, a + 4)
                    if start <= tgt < limit:
                        todo.append(tgt)
                    else:
                        tails.add(tgt)
                    break
                if op == 0 and (w & 0x3F) == 8:          # jr
                    seen.add(a + 4); maxa = max(maxa, a + 4)
                    if ((w >> 21) & 31) != 31:
                        jt = self.find_jumptable(a, start, limit)
                        if jt:
                            self.jumptables[a] = jt
                            todo.extend(jt[1])
                        else:
                            self.notes.append(f"unresolved jr at {a:#x} in {start:#x}")
                    break
                if op == 0 and (w & 0x3F) == 0xD:        # break
                    break
                a += 4
        return maxa + 4, tails

    # ------------------------------------------------------------------
    def run(self):
        kinds = dict(self.seeds())

        def region_end(a):
            for s, n in self.code_ranges:
                if s <= a < s + n:
                    return s + n
            return a

        # Fixpoint: trace every known function with the final bounds; tail-call
        # targets become new functions, untouched non-padding code becomes orphans.
        while True:
            self.jumptables, self.notes = {}, []
            starts = sorted(kinds)
            ends, new = {}, {}
            for i, s in enumerate(starts):
                limit = min(starts[i + 1] if i + 1 < len(starts) else 1 << 32, region_end(s))
                end, tails = self.trace(s, limit)
                ends[s] = (end, limit)
                for t in tails:
                    if self.is_code(t) and not (self.stub_lo <= t < self.stub_hi) and t not in kinds:
                        new[t] = "tailcall"
            for s in starts:
                a, limit = ends[s]
                while a < limit and self.word(a) == 0:
                    a += 4
                if a < limit and a not in kinds:
                    new.setdefault(a, "orphan")
            if not new:
                break
            # settle tail calls first; orphans only from a stable strong set
            tails_only = {a: k for a, k in new.items() if k == "tailcall"}
            kinds.update(tails_only or new)

        out = []
        for i, s in enumerate(starts):
            end, limit = ends[s]
            out.append({"start": s, "end": limit, "traced_end": end, "kind": kinds[s], "name": f"sub_{s:08X}"})
        return out


def main():
    img = psp_prx.load(sys.argv[1] if len(sys.argv) > 1 else ROOT / "extracted/PSP_GAME/SYSDIR/BOOT.BIN",
                       ROOT / "data" / "nids.json")
    an = Analyzer(img)
    funcs = an.run()
    from collections import Counter
    kinds = Counter(f["kind"] for f in funcs)
    covered = sum(f["end"] - f["start"] for f in funcs)
    total = sum(n for _, n in an.code_ranges)
    # rodata pointers into code that are neither jumptable targets nor function starts
    jt_targets = {t for _, ts in an.jumptables.values() for t in ts}
    fstarts = {f["start"] for f in funcs}
    stray = sorted({t for a, t in img.ptr_relocs.items() if an.is_code(t)} - jt_targets - fstarts)
    stats = {"functions": len(funcs), "kinds": kinds, "covered": covered, "code_bytes": total,
             "jumptables": len(an.jumptables), "jumptable_targets": len(jt_targets),
             "stray_code_pointers": len(stray), "notes": len(an.notes)}
    json.dump({"functions": funcs,
               "jumptables": {f"{k:#x}": {"table": v[0], "targets": v[1]} for k, v in an.jumptables.items()},
               "stray_code_pointers": stray, "notes": an.notes, "stats": stats},
              open(ROOT / "data" / "functions.json", "w"), indent=1)
    print(json.dumps(stats, indent=1, default=dict))
    for n in an.notes[:20]:
        print(" ", n)
    print("stray samples:", [hex(x) for x in stray[:20]])


if __name__ == "__main__":
    main()
