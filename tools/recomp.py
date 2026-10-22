#!/usr/bin/env python3
"""Static recompiler: PSP PRX (Allegrex MIPS) -> C++.

Reads the image (psp_prx) and data/functions.json (analyze.py) and writes:
  build/gen/recomp_funcs.h       declarations
  build/gen/recomp_NNN.cpp       function bodies
  build/gen/recomp_table.cpp     guest address -> host function, import table
  build/gen/image.bin            initial guest memory image [base, base+size)

Conventions (see runtime/src/recomp.h):
  - one host function `void F_xxxxxxxx(CPUState *c)` per guest function;
  - intra-function branches are gotos, the delay slot is emitted before the jump
    (condition is latched first), likely branches only run it when taken;
  - jal = native call after setting $ra; `jr $ra` = return;
  - jumps/branches leaving the function are tail calls;
  - `jr reg` with a known jump table = switch; otherwise rt_call() tail call;
  - calls into import stubs go through g_imports[i] (HLE);
  - VFPU (except bvf/bvt) is executed by the ported PPSSPP interpreter (Int_*).
"""
import json, pathlib, re, struct, sys
import rabbitizer as R
import psp_prx

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "build" / "gen"
FUNCS_PER_FILE = 150

VFPU_INT = {
    "vpfxs": "Int_VPFX", "vpfxt": "Int_VPFX", "vpfxd": "Int_VPFX",
    "lv.q": "Int_SVQ", "sv.q": "Int_SVQ", "lv.s": "Int_SV", "sv.s": "Int_SV",
    "lvl.q": "Int_SVQ", "lvr.q": "Int_SVQ", "svl.q": "Int_SVQ", "svr.q": "Int_SVQ",
    "mtv": "Int_Mftv", "mfv": "Int_Mftv", "mtvc": "Int_Mftv", "mfvc": "Int_Mftv",
    "vadd": "Int_VecDo3", "vsub": "Int_VecDo3", "vmul": "Int_VecDo3", "vdiv": "Int_VecDo3",
    "vmin": "Int_Vminmax", "vmax": "Int_Vminmax", "vsge": "Int_Vsge", "vslt": "Int_Vslt",
    "vabs": "Int_VV2Op", "vneg": "Int_VV2Op", "vmov": "Int_VV2Op", "vsat0": "Int_VV2Op",
    "vsat1": "Int_VV2Op", "vrcp": "Int_VV2Op", "vrsq": "Int_VV2Op", "vsin": "Int_VV2Op",
    "vcos": "Int_VV2Op", "vexp2": "Int_VV2Op", "vlog2": "Int_VV2Op", "vsqrt": "Int_VV2Op",
    "vasin": "Int_VV2Op", "vnrcp": "Int_VV2Op", "vnsin": "Int_VV2Op", "vrexp2": "Int_VV2Op",
    "vbfy1": "Int_Vbfy", "vbfy2": "Int_Vbfy", "vcmp": "Int_Vcmp", "vcst": "Int_Vcst",
    "vdot": "Int_VDot", "vhdp": "Int_VHdp", "vscl": "Int_VScl", "vfad": "Int_Vfad", "vavg": "Int_Vavg",
    "vf2in": "Int_Vf2i", "vf2iz": "Int_Vf2i", "vf2iu": "Int_Vf2i", "vf2id": "Int_Vf2i",
    "vi2f": "Int_Vi2f", "vi2uc": "Int_Vi2x", "vi2c": "Int_Vi2x", "vi2us": "Int_Vi2x", "vi2s": "Int_Vi2x",
    "vs2i": "Int_Vx2i", "vus2i": "Int_Vx2i", "vc2i": "Int_Vx2i", "vuc2i": "Int_Vx2i",
    "viim": "Int_Viim", "vfim": "Int_Viim", "vidt": "Int_Vidt", "vzero": "Int_VVectorInit",
    "vone": "Int_VVectorInit", "vmidt": "Int_VMatrixInit", "vmzero": "Int_VMatrixInit",
    "vmone": "Int_VMatrixInit", "vmmov": "Int_Vmmov", "vmmul": "Int_Vmmul", "vmscl": "Int_Vmscl",
    "vtfm2": "Int_Vtfm", "vtfm3": "Int_Vtfm", "vtfm4": "Int_Vtfm", "vhtfm2": "Int_Vtfm",
    "vhtfm3": "Int_Vtfm", "vhtfm4": "Int_Vtfm", "vcrsp": "Int_CrossQuat", "vqmul": "Int_CrossQuat",
    "vcrs": "Int_Vcrs", "vdet": "Int_Vdet", "vocp": "Int_Vocp", "vsocp": "Int_Vsocp", "vsgn": "Int_Vsgn",
    "vcmovt": "Int_Vcmov", "vcmovf": "Int_Vcmov", "vrot": "Int_Vrot", "vsrt1": "Int_Vsrt1",
    "vsrt2": "Int_Vsrt2", "vsrt3": "Int_Vsrt3", "vsrt4": "Int_Vsrt4", "vscmp": "Int_Vscmp",
    "vflush": "Int_Vflush", "vnop": "Int_Vflush", "vsync": "Int_Vflush",
    "vh2f": "Int_Vh2f", "vf2h": "Int_Vf2h", "vmfvc": "Int_Vmfvc", "vmtvc": "Int_Vmtvc",
    "vt4444": "Int_ColorConv", "vt5551": "Int_ColorConv", "vt5650": "Int_ColorConv",
    "vlgb": "Int_Vlgb", "vwbn": "Int_Vwbn", "vsbn": "Int_Vsbn", "vsbz": "Int_Vsbz",
    "vrnds": "Int_Vrnds", "vrndi": "Int_VrndX", "vrndf1": "Int_VrndX", "vrndf2": "Int_VrndX",
}
VFPU_OPS = {18, 24, 25, 27, 50, 52, 53, 54, 55, 58, 60, 61, 62, 63}


def s16(x):
    return x - 0x10000 if x & 0x8000 else x


def hx(v):
    return f"0x{v & 0xFFFFFFFF:X}u"


class FuncGen:
    def __init__(self, rc, f):
        self.rc, self.img = rc, rc.img
        self.start, self.end = f["start"], f["end"]
        self.name = rc.fname(self.start)
        self.out = []
        self.labels = set()
        self.unhandled = []

    # --- operand helpers --------------------------------------------------
    @staticmethod
    def r(n):
        return "0u" if n == 0 else f"c->r[{n}]"

    def setr(self, n, expr):
        if n != 0:
            self.emit(f"c->r[{n}] = {expr};")

    def emit(self, s):
        self.out.append("\t" + s)

    def inside(self, a):
        return self.start <= a < self.end

    # --- control-flow targets -------------------------------------------
    def collect_labels(self):
        for a in range(self.start, self.end, 4):
            w = self.img.u32(a)
            t = self.rc.branch_target(w, a)
            if t is not None and self.inside(t):
                self.labels.add(t)
            op = w >> 26
            if op == 2:
                t = ((a + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                if self.inside(t):
                    self.labels.add(t)
            jt = self.rc.jumptables.get(a)
            if jt:
                self.labels.update(t for t in jt if self.inside(t))

    def jump_to(self, t):
        """Unconditional transfer to guest address t (goto or tail call)."""
        if self.inside(t):
            return f"goto L_{t:08X};"
        return self.rc.call_expr(t) + " return;"

    # --- main -------------------------------------------------------------
    def gen(self):
        self.collect_labels()
        a = self.start
        while a < self.end:
            if a in self.labels:
                self.out.append(f"L_{a:08X}:;")
            w = self.img.u32(a)
            if self.rc.is_cti(w):
                self.gen_cti(a, w)
                a += 8
                continue
            self.gen_simple(a, w)
            a += 4
        # falling off the end continues into the next function
        last = self.img.u32(self.end - 8) if self.end - self.start >= 8 else 0
        if not self.rc.is_uncond(last):
            self.emit(self.rc.call_expr(self.end) + " return;")
        body = "\n".join(self.out)
        hook = self.rc.hooks.get(self.start)
        if hook:
            # the original body keeps its labels; the public symbol goes through the hook
            return (f"void {self.name}_orig(CPUState *c) {{\n{body}\n}}\n"
                    f"void {hook}(CPUState *c, RecompFunc orig);\n"
                    f"void {self.name}(CPUState *c) {{ {hook}(c, {self.name}_orig); }}\n")
        return f"void {self.name}(CPUState *c) {{\n{body}\n}}\n"

    def delay(self, a):
        """Code for the delay slot instruction at a (as a list of lines)."""
        w = self.img.u32(a)
        if self.rc.is_cti(w):
            self.unhandled.append(f"branch in delay slot at {a:#x}")
            return [f"\trt_unhandled(c, {hx(a)}, \"branch in delay slot\");"]
        save, self.out = self.out, []
        self.gen_simple(a, w)
        lines, self.out = self.out, save
        return lines

    def gen_cti(self, a, w):
        rc = self.rc
        op = w >> 26
        rs, rt, rd = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        d = self.delay(a + 4)
        D = "\n".join("\t" + x for x in d)
        nxt = a + 8

        if op == 2 or op == 3:                    # j / jal
            t = ((a + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
            if op == 3:
                self.emit(f"c->r[31] = {hx(nxt)};")
                self.out += d
                self.emit(rc.call_expr(t))
            else:
                self.out += d
                if self.rc.is_halt_loop(a, w):
                    self.emit(f"rt_halt(c, {hx(a)});")
                self.emit(self.jump_to(t))
        elif op == 0 and (w & 0x3F) == 8:         # jr
            self.emit(f"{{ u32 tgt = {self.r(rs)};")
            self.out += d
            if rs == 31:
                self.emit("\t(void)tgt; return; }")
            elif a in rc.jumptables:
                self.emit("\tswitch (tgt) {")
                for t in sorted(set(rc.jumptables[a])):
                    self.emit(f"\tcase {hx(t)}: goto L_{t:08X};")
                self.emit(f"\tdefault: rt_call(c, tgt); return;\n\t}} }}")
            else:
                self.emit("\trt_call(c, tgt); return; }")
        elif op == 0 and (w & 0x3F) == 9:         # jalr
            self.emit(f"{{ u32 tgt = {self.r(rs)};")
            if rd:
                self.emit(f"\tc->r[{rd}] = {hx(nxt)};")
            self.out += d
            self.emit("\trt_call(c, tgt); }")
        else:                                     # conditional branches
            t = rc.branch_target(w, a)
            cond, likely, link = self.branch_cond(w)
            if t == a and not likely and cond == "true" and not any(x.strip() for x in d):
                self.emit(f"rt_halt(c, {hx(a)});")
            if link:
                self.emit(f"c->r[31] = {hx(nxt)};")
            if likely:
                self.emit(f"if ({cond}) {{")
                self.out += ["\t" + x for x in d]
                self.emit("\t" + self.jump_to(t))
                self.emit("}")
            elif cond == "true":
                self.out += d
                self.emit(self.jump_to(t))
            else:
                self.emit(f"{{ bool taken = {cond};")
                self.out += ["\t" + x for x in d]
                self.emit(f"\tif (taken) {{ {self.jump_to(t)} }} }}")
        # someone may jump straight into the delay slot
        if a + 4 in self.labels:
            self.emit(f"goto L_{nxt:08X};")
            self.out.append(f"L_{a + 4:08X}:;")
            self.out += d
            self.labels.add(nxt)

    def branch_cond(self, w):
        op = w >> 26
        rs, rt = (w >> 21) & 31, (w >> 16) & 31
        S = lambda n: f"(s32){self.r(n)}"
        if op in (4, 20):
            c = "true" if rs == rt else f"{self.r(rs)} == {self.r(rt)}"
            return c, op == 20, False
        if op in (5, 21):
            return f"{self.r(rs)} != {self.r(rt)}", op == 21, False
        if op in (6, 22):
            return f"{S(rs)} <= 0", op == 22, False
        if op in (7, 23):
            return f"{S(rs)} > 0", op == 23, False
        if op == 1:
            base = rt & 3
            c = f"{S(rs)} < 0" if base in (0, 2) else ("true" if rs == 0 else f"{S(rs)} >= 0")
            return c, bool(rt & 2), bool(rt & 16)
        if op == 17:   # bc1f/bc1t/bc1fl/bc1tl
            tf, nd = (w >> 16) & 1, (w >> 17) & 1
            return ("c->fpcond" if tf else "!c->fpcond"), bool(nd), False
        if op == 18:   # bvf/bvt/bvfl/bvtl
            imm3, tf, nd = (w >> 18) & 7, (w >> 16) & 1, (w >> 17) & 1
            bit = f"((c->vfpuCtrl[3] >> {imm3}) & 1)"
            return (bit if tf else f"!{bit}"), bool(nd), False
        raise ValueError(hex(w))

    # --- non-branch instructions -----------------------------------------
    def gen_simple(self, a, w):
        op = w >> 26
        rs, rt, rd, sa = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
        imm, simm = w & 0xFFFF, s16(w & 0xFFFF)
        r, setr, E = self.r, self.setr, self.emit
        S = lambda n: f"(s32){r(n)}"
        addr = f"{r(rs)} + {hx(simm)}" if simm else r(rs)

        if w == 0:
            return
        if op == 0:
            fn = w & 0x3F
            if fn == 0: setr(rd, f"{r(rt)} << {sa}")
            elif fn == 2:
                if rs == 1: setr(rd, f"ROTR({r(rt)}, {sa})")
                else: setr(rd, f"{r(rt)} >> {sa}")
            elif fn == 3: setr(rd, f"(u32)({S(rt)} >> {sa})")
            elif fn == 4: setr(rd, f"{r(rt)} << ({r(rs)} & 31)")
            elif fn == 6:
                if sa == 1: setr(rd, f"ROTR({r(rt)}, {r(rs)} & 31)")
                else: setr(rd, f"{r(rt)} >> ({r(rs)} & 31)")
            elif fn == 7: setr(rd, f"(u32)({S(rt)} >> ({r(rs)} & 31))")
            elif fn == 10: E(f"if ({r(rt)} == 0) {{ {self._set(rd, r(rs))} }}")
            elif fn == 11: E(f"if ({r(rt)} != 0) {{ {self._set(rd, r(rs))} }}")
            elif fn == 12: E(f"rt_syscall(c, {hx(a)}, {hx(w)});")
            elif fn == 13: E(f"rt_break(c, {hx(a)});")
            elif fn == 15: pass
            elif fn == 16: setr(rd, "c->hi")
            elif fn == 17: E(f"c->hi = {r(rs)};")
            elif fn == 18: setr(rd, "c->lo")
            elif fn == 19: E(f"c->lo = {r(rs)};")
            elif fn == 22: setr(rd, f"CLZ({r(rs)})")
            elif fn == 23: setr(rd, f"CLZ(~{r(rs)})")
            elif fn == 24: E(f"{{ s64 m = (s64){S(rs)} * {S(rt)}; c->lo = (u32)m; c->hi = (u32)(m >> 32); }}")
            elif fn == 25: E(f"{{ u64 m = (u64){r(rs)} * {r(rt)}; c->lo = (u32)m; c->hi = (u32)(m >> 32); }}")
            elif fn == 26: E(f"DIV(c, {r(rs)}, {r(rt)});")
            elif fn == 27: E(f"DIVU(c, {r(rs)}, {r(rt)});")
            elif fn in (28, 29, 46, 47):
                signed = fn in (28, 46)
                m = f"(s64){S(rs)} * {S(rt)}" if signed else f"(u64){r(rs)} * {r(rt)}"
                sign = "+" if fn in (28, 29) else "-"
                E(f"{{ u64 acc = ((u64)c->hi << 32) | c->lo; acc = acc {sign} (u64)({m}); c->lo = (u32)acc; c->hi = (u32)(acc >> 32); }}")
            elif fn in (32, 33): setr(rd, f"{r(rs)} + {r(rt)}")
            elif fn in (34, 35): setr(rd, f"{r(rs)} - {r(rt)}")
            elif fn == 36: setr(rd, f"{r(rs)} & {r(rt)}")
            elif fn == 37: setr(rd, f"{r(rs)} | {r(rt)}")
            elif fn == 38: setr(rd, f"{r(rs)} ^ {r(rt)}")
            elif fn == 39: setr(rd, f"~({r(rs)} | {r(rt)})")
            elif fn == 42: setr(rd, f"({S(rs)} < {S(rt)}) ? 1u : 0u")
            elif fn == 43: setr(rd, f"({r(rs)} < {r(rt)}) ? 1u : 0u")
            elif fn == 44: setr(rd, f"(u32)std::max({S(rs)}, {S(rt)})")
            elif fn == 45: setr(rd, f"(u32)std::min({S(rs)}, {S(rt)})")
            else: self.bad(a, w)
        elif op in (8, 9): setr(rt, f"{r(rs)} + {hx(simm)}")
        elif op == 10: setr(rt, f"({S(rs)} < {simm}) ? 1u : 0u")
        elif op == 11: setr(rt, f"({r(rs)} < {hx(simm)}) ? 1u : 0u")
        elif op == 12: setr(rt, f"{r(rs)} & {hx(imm)}")
        elif op == 13: setr(rt, f"{r(rs)} | {hx(imm)}")
        elif op == 14: setr(rt, f"{r(rs)} ^ {hx(imm)}")
        elif op == 15: setr(rt, hx(imm << 16))
        elif op == 17: self.gen_fpu(a, w)
        elif op == 28:
            fn = w & 0x3F
            if fn == 0 and w == 0x70000000: E(f"rt_halt(c, {hx(a)});")
            else: self.bad(a, w)
        elif op == 31:
            fn = w & 0x3F
            if fn == 0:
                size = rd + 1
                setr(rt, f"({r(rs)} >> {sa}) & {hx((1 << size) - 1)}")
            elif fn == 4:
                size = rd + 1 - sa
                mask = ((1 << size) - 1) << sa
                setr(rt, f"({r(rt)} & {hx(~mask)}) | (({r(rs)} << {sa}) & {hx(mask)})")
            elif fn == 32:
                sub = sa
                if sub == 16: setr(rd, f"(u32)(s32)(s8){r(rt)}")
                elif sub == 24: setr(rd, f"(u32)(s32)(s16){r(rt)}")
                elif sub == 2: setr(rd, f"WSBH({r(rt)})")
                elif sub == 3: setr(rd, f"__builtin_bswap32({r(rt)})")
                elif sub == 20: setr(rd, f"BITREV({r(rt)})")
                else: self.bad(a, w)
            else: self.bad(a, w)
        elif op == 32: setr(rt, f"(u32)(s32)(s8)LB({addr})")
        elif op == 33: setr(rt, f"(u32)(s32)(s16)LH({addr})")
        elif op == 34: E(f"{self._lhs(rt)} = LWL({addr}, {r(rt)});" if rt else "")
        elif op == 35: setr(rt, f"LW({addr})")
        elif op == 36: setr(rt, f"LB({addr})")
        elif op == 37: setr(rt, f"LH({addr})")
        elif op == 38: E(f"{self._lhs(rt)} = LWR({addr}, {r(rt)});" if rt else "")
        elif op == 40: E(f"SB({addr}, (u8){r(rt)});")
        elif op == 41: E(f"SH({addr}, (u16){r(rt)});")
        elif op == 42: E(f"SWL({addr}, {r(rt)});")
        elif op == 43: E(f"SW({addr}, {r(rt)});")
        elif op == 46: E(f"SWR({addr}, {r(rt)});")
        elif op == 47: pass                                    # cache
        elif op == 48: setr(rt, f"LW({addr})"); E("c->llBit = 1;")   # ll
        elif op == 56: E(f"SW({addr}, {r(rt)});"); setr(rt, "1u")      # sc (single core)
        elif op == 49: E(f"c->fi[{rt}] = LW({addr});")
        elif op == 57: E(f"SW({addr}, c->fi[{rt}]);")
        elif op in VFPU_OPS: self.gen_vfpu(a, w)
        else: self.bad(a, w)

    def _lhs(self, n):
        return f"c->r[{n}]"

    def _set(self, n, expr):
        return f"c->r[{n}] = {expr};" if n else ""

    def gen_fpu(self, a, w):
        fmt = (w >> 21) & 31
        ft, fs, fd = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
        rt = ft
        E = self.emit
        if fmt == 0: self.setr(rt, f"c->fi[{fs}]")
        elif fmt == 2:
            if rt:
                E(f"c->r[{rt}] = CFC1(c, {fs});")
        elif fmt == 4: E(f"c->fi[{fs}] = {self.r(rt)};")
        elif fmt == 6: E(f"CTC1(c, {fs}, {self.r(rt)});")
        elif fmt == 16:
            fn = w & 0x3F
            F = lambda n: f"c->f[{n}]"
            if fn == 0: E(f"c->f[{fd}] = {F(fs)} + {F(ft)};")
            elif fn == 1: E(f"c->f[{fd}] = {F(fs)} - {F(ft)};")
            elif fn == 2: E(f"c->f[{fd}] = {F(fs)} * {F(ft)};")
            elif fn == 3: E(f"c->f[{fd}] = {F(fs)} / {F(ft)};")
            elif fn == 4: E(f"c->f[{fd}] = sqrtf({F(fs)});")
            elif fn == 5: E(f"c->f[{fd}] = fabsf({F(fs)});")
            elif fn == 6: E(f"c->f[{fd}] = {F(fs)};")
            elif fn == 7: E(f"c->f[{fd}] = -{F(fs)};")
            elif fn in (12, 13, 14, 15, 36):
                E(f"c->fs[{fd}] = FCVT(c, {fn}, {F(fs)});")
            elif 48 <= fn <= 63: E(f"c->fpcond = FCMP({fn & 15}, {F(fs)}, {F(ft)});")
            else: self.bad(a, w)
        elif fmt == 20 and (w & 0x3F) == 32:
            E(f"c->f[{fd}] = (float)c->fs[{fs}];")
        else:
            self.bad(a, w)

    def gen_vfpu(self, a, w):
        ins = R.Instruction(w, vram=a, category=R.InstrCategory.R4000ALLEGREX)
        name = ins.getOpcodeName()
        base = name if name in VFPU_INT else re.sub(r"\.[spqt]$", "", name)
        fn = VFPU_INT.get(base)
        if fn is None:
            self.bad(a, w, name)
            return
        self.rc.vfpu_used.add(fn)
        self.emit(f"MIPSInt::{fn}(MIPSOpcode({hx(w)}));  // {ins.disassemble().strip()}")

    def bad(self, a, w, name=None):
        ins = R.Instruction(w, vram=a, category=R.InstrCategory.R4000ALLEGREX)
        name = name or ins.getOpcodeName()
        self.unhandled.append(f"{a:#x} {w:08x} {ins.disassemble()}")
        self.emit(f"rt_unhandled(c, {hx(a)}, \"{name}\");")


class Recompiler:
    def __init__(self, img, fdata):
        self.img = img
        self.funcs = fdata["functions"]
        self.fstarts = {f["start"] for f in self.funcs}
        self.jumptables = {int(k, 16): v["targets"] for k, v in fdata["jumptables"].items()}
        self.import_idx = {imp.stub: i for i, imp in enumerate(img.imports)}
        self.vfpu_used = set()
        hooks_path = ROOT / "data" / "hooks.json"
        hooks = json.load(open(hooks_path)) if hooks_path.exists() else {}
        self.hooks = {int(k, 16): v for k, v in hooks.items() if not k.startswith("_")}

    def fname(self, a):
        return f"F_{a:08X}"

    def call_expr(self, t):
        if t in self.import_idx:
            i = self.import_idx[t]
            return f"g_imports[{i}](c);  /* {self.img.imports[i].name} */"
        if t in self.fstarts:
            return f"{self.fname(t)}(c);"
        return f"rt_call(c, {hx(t)});"

    @staticmethod
    def branch_target(w, a):
        op = w >> 26
        t = (a + 4 + (s16(w & 0xFFFF) << 2)) & 0xFFFFFFFF
        if op in (4, 5, 6, 7, 20, 21, 22, 23):
            return t
        if op == 1 and ((w >> 16) & 31) in (0, 1, 2, 3, 16, 17, 18, 19):
            return t
        if op in (17, 18) and ((w >> 21) & 31) == 8:
            return t
        return None

    def is_cti(self, w):
        op = w >> 26
        if op in (2, 3):
            return True
        if op == 0 and (w & 0x3F) in (8, 9):
            return True
        return self.branch_target(w, 0) is not None

    def is_uncond(self, w):
        op = w >> 26
        if op == 2:
            return True
        if op == 0 and (w & 0x3F) == 8:
            return True
        if op == 4 and ((w >> 21) & 31) == 0 and ((w >> 16) & 31) == 0:
            return True
        return False

    def is_halt_loop(self, a, w):
        return (((a + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)) == a

    def run(self):
        OUT.mkdir(parents=True, exist_ok=True)
        for old in OUT.glob("recomp_[0-9]*.cpp"):   # a previous run may have made more files
            old.unlink()
        unhandled = []
        files = []
        for k in range(0, len(self.funcs), FUNCS_PER_FILE):
            chunk = self.funcs[k:k + FUNCS_PER_FILE]
            parts = ['#include "recomp.h"\n#include "recomp_funcs.h"\n']
            for f in chunk:
                g = FuncGen(self, f)
                parts.append(g.gen())
                unhandled += g.unhandled
            fn = OUT / f"recomp_{k // FUNCS_PER_FILE:03d}.cpp"
            self.write_if_changed(fn, "\n".join(parts))
            files.append(fn.name)

        decl = ["#pragma once", '#include "recomp.h"']
        decl += [f"void {self.fname(f['start'])}(CPUState *c);" for f in self.funcs]
        self.write_if_changed(OUT / "recomp_funcs.h", "\n".join(decl) + "\n")

        tab = ['#include "recomp.h"', '#include "recomp_funcs.h"', "",
               "const RecompEntry g_recomp_funcs[] = {"]
        tab += [f"\t{{{hx(f['start'])}, {hx(f['end'])}, {self.fname(f['start'])}}}," for f in self.funcs]
        tab += ["};", f"const size_t g_recomp_func_count = {len(self.funcs)};", "",
                "const ImportEntry g_import_list[] = {"]
        tab += [f'\t{{{hx(i.stub)}, "{i.lib}", {hx(i.nid)}, "{i.name}"}},' for i in self.img.imports]
        tab += ["};", f"const size_t g_import_count = {len(self.img.imports)};",
                f"HLEFunc g_imports[{len(self.img.imports)}];", "",
                f"const u32 g_image_base = {hx(self.img.base)};",
                f"const u32 g_image_size = {hx(self.img.size)};",
                f"const u32 g_image_entry = {hx(self.img.entry)};",
                f"const u32 g_image_gp = {hx(self.img.gp)};",
                f"const u32 g_modinfo_addr = {hx(self.img.modinfo_addr)};"]
        ctors = self.img.sections.get(".ctors")
        tab.append(f"const u32 g_ctors_addr = {hx(ctors[0] if ctors else 0)};")
        tab.append(f"const u32 g_ctors_size = {hx(ctors[1] if ctors else 0)};")
        self.write_if_changed(OUT / "recomp_table.cpp", "\n".join(tab) + "\n")

        (OUT / "image.bin").write_bytes(bytes(self.img.mem))
        (OUT / "sources.txt").write_text("\n".join(files + ["recomp_table.cpp"]) + "\n")
        (OUT / "unhandled.txt").write_text("\n".join(unhandled) + "\n")
        print(f"{len(self.funcs)} functions in {len(files)} files; "
              f"{len(unhandled)} unhandled instructions; VFPU handlers: {len(self.vfpu_used)}")
        for u in unhandled[:30]:
            print("  ", u)

    @staticmethod
    def write_if_changed(path, text):
        if not path.exists() or path.read_text() != text:
            path.write_text(text)


def main():
    img = psp_prx.load(ROOT / "extracted/PSP_GAME/SYSDIR/BOOT.BIN", ROOT / "data/nids.json")
    fdata = json.load(open(ROOT / "data/functions.json"))
    Recompiler(img, fdata).run()


if __name__ == "__main__":
    main()
