#pragma once
// Everything recompiled guest code (build/gen/recomp_*.cpp) needs.
// Semantics mirror PPSSPP's interpreter (Core/MIPS/MIPSInt.cpp).
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/MIPSIntVFPU.h"
#include "cpu_state.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef void (*RecompFunc)(CPUState *c);
typedef void (*HLEFunc)(CPUState *c);

struct RecompEntry {
	u32 start, end;
	RecompFunc fn;
};
struct ImportEntry {
	u32 stub;
	const char *lib;
	u32 nid;
	const char *name;
};

// --- generated tables (recomp_table.cpp) ---
extern const RecompEntry g_recomp_funcs[];
extern const size_t g_recomp_func_count;
extern const ImportEntry g_import_list[];
extern const size_t g_import_count;
extern HLEFunc g_imports[];
extern const u32 g_image_base, g_image_size, g_image_entry, g_image_gp, g_modinfo_addr;
extern const u32 g_ctors_addr, g_ctors_size;

// --- runtime services ---
extern u8 *g_mem;
void rt_call(CPUState *c, u32 addr);
[[noreturn]] void rt_unhandled(CPUState *c, u32 pc, const char *what);
void rt_break(CPUState *c, u32 pc);
void rt_halt(CPUState *c, u32 pc);
void rt_syscall(CPUState *c, u32 pc, u32 op);

// --- memory: guest address & 0x1FFFFFFF indexes a 512 MB reservation ---
#define GADDR(a) (g_mem + ((u32)(a) & 0x1FFFFFFFu))
static inline u32 LW(u32 a) { u32 v; memcpy(&v, GADDR(a), 4); return v; }
static inline u32 LH(u32 a) { u16 v; memcpy(&v, GADDR(a), 2); return v; }
static inline u32 LB(u32 a) { return *GADDR(a); }
static inline void SW(u32 a, u32 v) { memcpy(GADDR(a), &v, 4); }
static inline void SH(u32 a, u16 v) { memcpy(GADDR(a), &v, 2); }
static inline void SB(u32 a, u8 v) { *GADDR(a) = v; }

static inline u32 LWL(u32 a, u32 old) {
	u32 shift = (a & 3) * 8, mem = LW(a & ~3u);
	return (old & (0x00ffffffu >> shift)) | (mem << (24 - shift));
}
static inline u32 LWR(u32 a, u32 old) {
	u32 shift = (a & 3) * 8, mem = LW(a & ~3u);
	return (old & (0xffffff00u << (24 - shift))) | (mem >> shift);
}
static inline void SWL(u32 a, u32 v) {
	u32 shift = (a & 3) * 8, mem = LW(a & ~3u);
	SW(a & ~3u, (v >> (24 - shift)) | (mem & (0xffffff00u << shift)));
}
static inline void SWR(u32 a, u32 v) {
	u32 shift = (a & 3) * 8, mem = LW(a & ~3u);
	SW(a & ~3u, (v << shift) | (mem & (0x00ffffffu >> (24 - shift))));
}

// --- ALU helpers ---
static inline u32 ROTR(u32 v, u32 s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }
static inline u32 CLZ(u32 v) { return v ? __builtin_clz(v) : 32; }
static inline u32 WSBH(u32 v) { return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); }
static inline u32 BITREV(u32 v) {
	u32 r = 0;
	for (int i = 0; i < 32; i++) r |= ((v >> i) & 1) << (31 - i);
	return r;
}
static inline void DIV(CPUState *c, u32 ua, u32 ub) {
	s32 a = (s32)ua, b = (s32)ub;
	if (a == (s32)0x80000000 && b == -1) { c->lo = 0x80000000u; c->hi = (u32)-1; }
	else if (b != 0) { c->lo = (u32)(a / b); c->hi = (u32)(a % b); }
	else { c->lo = a < 0 ? 1 : (u32)-1; c->hi = (u32)a; }
}
static inline void DIVU(CPUState *c, u32 a, u32 b) {
	if (b != 0) { c->lo = a / b; c->hi = a % b; }
	else { c->lo = a <= 0xFFFF ? 0xFFFF : (u32)-1; c->hi = a; }
}

// --- FPU helpers ---
static inline bool F_ISNAN(float f) { u32 u; memcpy(&u, &f, 4); return (u & 0x7F800000u) == 0x7F800000u && (u & 0x7FFFFFu); }
static inline bool F_ISNANINF(float f) { u32 u; memcpy(&u, &f, 4); return (u & 0x7F800000u) == 0x7F800000u; }
static inline bool F_ISINF(float f) { u32 u; memcpy(&u, &f, 4); return (u & 0x7FFFFFFFu) == 0x7F800000u; }

static inline double F_RINT(double d) {  // round half to even, like PPSSPP round_ieee_754
	float i = (float)floor(d);
	d -= i;
	if (d < 0.5f) return i;
	if (d > 0.5f) return i + 1.0f;
	float ip; modff(i / 2.0f, &ip);
	return (2.0f * ip == i) ? i : i + 1.0f;
}

// fn: 12 round.w, 13 trunc.w, 14 ceil.w, 15 floor.w, 36 cvt.w
static inline s32 FCVT(CPUState *c, int fn, float f) {
	if (F_ISNANINF(f)) return F_ISINF(f) && f < 0.0f ? (s32)0x80000000 : 0x7FFFFFFF;
	switch (fn) {
	case 12: return (s32)floorf(f + 0.5f);
	case 13:
		if (f >= 0.0f) { s32 r = (s32)floorf(f); return r == (s32)0x80000000 ? 0x7FFFFFFF : r; }
		return (s32)ceilf(f);
	case 14: return (s32)ceilf(f);
	case 15: return (s32)floorf(f);
	default:
		switch (c->fcr31 & 3) {
		case 0: return (s32)F_RINT(f);
		case 1: return (s32)f;
		case 2: return (s32)ceilf(f);
		default: return (s32)floorf(f);
		}
	}
}

static inline u32 FCMP(int cond, float s, float t) {
	bool nan = F_ISNAN(s) || F_ISNAN(t);
	switch (cond & 7) {
	case 0: return 0;
	case 1: return nan;
	case 2: return !nan && s == t;
	case 3: return s == t || nan;
	case 4: return s < t;
	case 5: return s < t || nan;
	case 6: return s <= t;
	default: return s <= t || nan;
	}
}

static inline u32 CFC1(CPUState *c, int fs) {
	if (fs == 31) {
		c->fcr31 = (c->fcr31 & ~(1u << 23)) | ((c->fpcond & 1) << 23);
		return c->fcr31;
	}
	if (fs == 0) return 0x00003351;
	return 0;
}
static inline void CTC1(CPUState *c, int fs, u32 v) {
	if (fs == 31) {
		c->fcr31 = v & 0x0181FFFF;
		c->fpcond = (v >> 23) & 1;
	}
}
