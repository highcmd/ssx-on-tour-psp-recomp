// Software GE renderer: executes GE drawing commands and writes pixels straight
// into guest VRAM, so render-to-texture and CPU readbacks behave like the PSP.
// Register semantics follow PPSSPP's GPU/ge_constants.h and its software
// renderer (public, GPL); see NOTES.md.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <array>
#include <utility>
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <pthread.h>
#include <unordered_map>
#include <vector>

#include "ge_gl.h"
#include "kernel.h"

// ============================================================ state
static u32 R[256];                     // last value of every command (24-bit payload)
static float worldM[12], viewM[12], projM[16], tgenM[12], boneM[8][12];
static int worldN, viewN, projN, tgenN, boneN;
static float morphW[8];
static u8 clut[1024];
static u32 s_vaddr, s_iaddr;
static u64 s_prims;
static int s_traceState;   // 0 idle, 1 armed (wait FINISH), 2 tracing, 3 done
extern u32 g_ge_base, g_ge_offset;
extern int g_render_scale;

static inline float f24(u32 v) { u32 b = v << 8; float f; memcpy(&f, &b, 4); return f; }
static inline bool on(u32 cmd) { return R[cmd] & 1; }

std::map<std::string, u64> g_gestats;
void ge_dump_stats() {
	for (auto &[k, v] : g_gestats) rt_log(LOG_GE, "  %8llu  %s", (unsigned long long)v, k.c_str());
	extern u64 g_cmd_hist[256];
	for (int i = 0; i < 256; i++) if (g_cmd_hist[i] > 1000000) rt_log(LOG_GE, "  cmd %02x: %llu", i, (unsigned long long)g_cmd_hist[i]);
	rt_log(LOG_GE, "GE: %llu prims", (unsigned long long)s_prims);
}
u64 ge_prim_count() { return s_prims; }

// ============================================================ vectors
struct V4 { float x, y, z, w; };
struct Vtx {
	float x, y, z, w;      // clip space (or screen when through)
	float sx, sy, sz;      // screen
	float u, v;            // texcoord (normalised 0..1 for transformed, texels for through)
	float r, g, b, a;      // 0..255
	float fog;             // 0..1 (1 = no fog)
};

static inline void mul43(const float *m, const float *in, float *out) {
	out[0] = m[0] * in[0] + m[3] * in[1] + m[6] * in[2] + m[9];
	out[1] = m[1] * in[0] + m[4] * in[1] + m[7] * in[2] + m[10];
	out[2] = m[2] * in[0] + m[5] * in[1] + m[8] * in[2] + m[11];
}
static inline void mul43n(const float *m, const float *in, float *out) {
	out[0] = m[0] * in[0] + m[3] * in[1] + m[6] * in[2];
	out[1] = m[1] * in[0] + m[4] * in[1] + m[7] * in[2];
	out[2] = m[2] * in[0] + m[5] * in[1] + m[8] * in[2];
}
static inline V4 mul44(const float *m, const float *in) {
	return {m[0] * in[0] + m[4] * in[1] + m[8] * in[2] + m[12], m[1] * in[0] + m[5] * in[1] + m[9] * in[2] + m[13],
	        m[2] * in[0] + m[6] * in[1] + m[10] * in[2] + m[14], m[3] * in[0] + m[7] * in[1] + m[11] * in[2] + m[15]};
}

// ============================================================ vertex decoding
struct VFormat {
	int tc, col, nrm, pos, wt, idx, nw, nm;
	bool through;
	int size, oW, oT, oC, oN, oP;
};

static VFormat decode_vtype(u32 vt) {
	VFormat f;
	f.tc = vt & 3; f.col = (vt >> 2) & 7; f.nrm = (vt >> 5) & 3; f.pos = (vt >> 7) & 3;
	f.wt = (vt >> 9) & 3; f.idx = (vt >> 11) & 3; f.nw = ((vt >> 14) & 7) + 1; f.nm = ((vt >> 18) & 7) + 1;
	f.through = (vt >> 23) & 1;
	static const int sz124[4] = {0, 1, 2, 4};
	static const int colsz[8] = {0, 0, 0, 0, 2, 2, 2, 4};
	int off = 0, maxal = 1;
	auto take = [&](int elem, int count) {
		if (!elem) return -1;
		off = (off + elem - 1) & ~(elem - 1);
		int o = off;
		off += elem * count;
		maxal = std::max(maxal, elem);
		return o;
	};
	f.oW = f.wt ? take(sz124[f.wt], f.nw) : -1;
	f.oT = f.tc ? take(sz124[f.tc], 2) : -1;
	f.oC = f.col >= 4 ? take(colsz[f.col], 1) : -1;
	f.oN = f.nrm ? take(sz124[f.nrm], 3) : -1;
	f.oP = take(sz124[f.pos], 3);
	f.size = (off + maxal - 1) & ~(maxal - 1);
	return f;
}

static inline float rd(const u8 *p, int fmt, int i, bool normS) {
	switch (fmt) {
	case 1: return normS ? (s8)p[i] / 128.0f : (float)(s8)p[i];
	case 2: { s16 v; memcpy(&v, p + 2 * i, 2); return normS ? v / 32768.0f : (float)v; }
	default: { float v; memcpy(&v, p + 4 * i, 4); return v; }
	}
}
static inline float rdu(const u8 *p, int fmt, int i, bool norm) {
	switch (fmt) {
	case 1: return norm ? p[i] / 128.0f : (float)p[i];
	case 2: { u16 v; memcpy(&v, p + 2 * i, 2); return norm ? v / 32768.0f : (float)v; }
	default: { float v; memcpy(&v, p + 4 * i, 4); return v; }
	}
}

static void read_color(const u8 *p, int fmt, float *c) {
	u32 v;
	switch (fmt) {
	case 4: { u16 x; memcpy(&x, p, 2); c[0] = (x & 31) * 255 / 31.f; c[1] = ((x >> 5) & 63) * 255 / 63.f; c[2] = (x >> 11) * 255 / 31.f; c[3] = 255; return; }
	case 5: { u16 x; memcpy(&x, p, 2); c[0] = (x & 31) * 255 / 31.f; c[1] = ((x >> 5) & 31) * 255 / 31.f; c[2] = ((x >> 10) & 31) * 255 / 31.f; c[3] = (x >> 15) ? 255.f : 0.f; return; }
	case 6: { u16 x; memcpy(&x, p, 2); c[0] = (x & 15) * 17.f; c[1] = ((x >> 4) & 15) * 17.f; c[2] = ((x >> 8) & 15) * 17.f; c[3] = (x >> 12) * 17.f; return; }
	default: memcpy(&v, p, 4); c[0] = v & 255; c[1] = (v >> 8) & 255; c[2] = (v >> 16) & 255; c[3] = v >> 24; return;
	}
}

static inline void unpack_rgb(u32 v, float *c) { c[0] = v & 255; c[1] = (v >> 8) & 255; c[2] = (v >> 16) & 255; }

// Lighting (world space), per PSP: emissive + ambient + sum of lights.
static void light_vertex(const float *posW, const float *nrmW, float *col) {
	float matAmb[4], matDif[3], matSpe[3], emi[3], alc[3];
	u32 mu = R[0x53];
	if (mu & 1) memcpy(matAmb, col, 16); else { unpack_rgb(R[0x55], matAmb); matAmb[3] = R[0x58] & 255; }
	if (mu & 2) memcpy(matDif, col, 12); else unpack_rgb(R[0x56], matDif);
	if (mu & 4) memcpy(matSpe, col, 12); else unpack_rgb(R[0x57], matSpe);
	unpack_rgb(R[0x54], emi);
	unpack_rgb(R[0x5c], alc);
	float out[4] = {emi[0] + alc[0] * matAmb[0] / 255, emi[1] + alc[1] * matAmb[1] / 255, emi[2] + alc[2] * matAmb[2] / 255,
	                (R[0x5d] & 255) * matAmb[3] / 255};
	float spec[3] = {0, 0, 0};
	float n[3] = {nrmW[0], nrmW[1], nrmW[2]};
	if (on(0x51)) { n[0] = -n[0]; n[1] = -n[1]; n[2] = -n[2]; }
	float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (nl > 0) { n[0] /= nl; n[1] /= nl; n[2] /= nl; }
	float spow = f24(R[0x5b]);
	for (int i = 0; i < 4; i++) {
		if (!on(0x18 + i)) continue;
		u32 lt = R[0x5f + i];
		int comp = lt & 3, type = (lt >> 8) & 3;
		float L[3] = {f24(R[0x63 + i * 3]), f24(R[0x64 + i * 3]), f24(R[0x65 + i * 3])};
		float att = 1;
		if (type != 0) {
			L[0] -= posW[0]; L[1] -= posW[1]; L[2] -= posW[2];
			float d = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
			if (d > 0) { L[0] /= d; L[1] /= d; L[2] /= d; }
			float ka = f24(R[0x7b + i * 3]), kb = f24(R[0x7c + i * 3]), kc = f24(R[0x7d + i * 3]);
			float den = ka + kb * d + kc * d * d;
			att = den > 0 ? std::min(1.0f, 1.0f / den) : 1.0f;
			if (type == 2) {
				float D[3] = {f24(R[0x6f + i * 3]), f24(R[0x70 + i * 3]), f24(R[0x71 + i * 3])};
				float dl = sqrtf(D[0] * D[0] + D[1] * D[1] + D[2] * D[2]);
				float cosA = dl > 0 ? -(L[0] * D[0] + L[1] * D[1] + L[2] * D[2]) / dl : 0;
				float cutoff = f24(R[0x8b + i]);
				att = cosA >= cutoff ? att * powf(std::max(cosA, 0.0f), f24(R[0x87 + i])) : 0;
			}
		} else {
			float d = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
			if (d > 0) { L[0] /= d; L[1] /= d; L[2] /= d; }
		}
		float ndl = n[0] * L[0] + n[1] * L[1] + n[2] * L[2];
		if (comp == 2) ndl = ndl > 0 ? powf(ndl, spow) : 0;
		float lamb[3], ldif[3], lspe[3];
		unpack_rgb(R[0x8f + i * 3], lamb);
		unpack_rgb(R[0x90 + i * 3], ldif);
		unpack_rgb(R[0x91 + i * 3], lspe);
		for (int k = 0; k < 3; k++) {
			out[k] += att * (lamb[k] * matAmb[k] / 255 + std::max(ndl, 0.0f) * ldif[k] * matDif[k] / 255);
		}
		if (comp == 1 && ndl > 0) {
			float H[3] = {L[0], L[1], L[2] + 1};
			float hl = sqrtf(H[0] * H[0] + H[1] * H[1] + H[2] * H[2]);
			float ndh = hl > 0 ? (n[0] * H[0] + n[1] * H[1] + n[2] * H[2]) / hl : 0;
			if (ndh > 0) {
				float s = powf(ndh, spow) * att;
				for (int k = 0; k < 3; k++) spec[k] += s * lspe[k] * matSpe[k] / 255;
			}
		}
	}
	for (int k = 0; k < 3; k++) col[k] = std::min(255.0f, out[k] + spec[k]);
	col[3] = std::min(255.0f, out[3]);
}

static Vtx transform_attrs(const VFormat &f, const u8 *p, float *pos, float *nrm, float *col, float *tc);
static Vtx transform_vertex(const VFormat &f, const u8 *p) {
	Vtx v{};
	float pos[3] = {0, 0, 0}, nrm[3] = {0, 0, 1}, col[4], tc[2] = {0, 0};
	// morph targets: each copy has the full layout
	for (int m = 0; m < f.nm; m++) {
		const u8 *q = p + m * f.size;
		float w = f.nm > 1 ? morphW[m] : 1.0f;
		for (int i = 0; i < 3; i++) pos[i] += w * rd(q + f.oP, f.pos, i, !f.through);
		if (f.oN >= 0) for (int i = 0; i < 3; i++) nrm[i] = (m ? nrm[i] : 0) + w * rd(q + f.oN, f.nrm, i, true);
		if (f.oT >= 0) for (int i = 0; i < 2; i++) tc[i] = (m ? tc[i] : 0) + w * rdu(q + f.oT, f.tc, i, !f.through);
	}
	if (f.oC >= 0) read_color(p + f.oC, f.col, col);
	else { unpack_rgb(R[0x55], col); col[3] = R[0x58] & 255; }

	if (f.through) {
		v.sx = pos[0] * g_render_scale; v.sy = pos[1] * g_render_scale; v.sz = pos[2];
		// through mode: z is unsigned (u8/u16), only x/y are signed
		if (f.pos == 2) { u16 z; memcpy(&z, p + f.oP + 4, 2); v.sz = z; }
		else if (f.pos == 1) v.sz = p[f.oP + 2];
		v.w = 1;
		v.u = tc[0]; v.v = tc[1];
		v.r = col[0]; v.g = col[1]; v.b = col[2]; v.a = col[3];
		v.fog = 1;
		return v;
	}
	return transform_attrs(f, p, pos, nrm, col, tc);
}

// Transform already-decoded model-space attributes (non-through). p (the raw
// vertex, for skinning weights) may be null, e.g. for tessellated patch points.
static Vtx transform_attrs(const VFormat &f, const u8 *p, float *pos, float *nrm, float *col, float *tc) {
	Vtx v{};
	// skinning
	if (f.oW >= 0 && p) {
		float sp[3] = {0, 0, 0}, sn[3] = {0, 0, 0};
		for (int i = 0; i < f.nw; i++) {
			float w = rdu(p + f.oW, f.wt, i, true);
			if (f.wt == 1) w = p[f.oW + i] / 128.0f;
			if (w == 0) continue;
			float tp[3], tn[3];
			mul43(boneM[i], pos, tp);
			mul43n(boneM[i], nrm, tn);
			for (int k = 0; k < 3; k++) { sp[k] += w * tp[k]; sn[k] += w * tn[k]; }
		}
		for (int k = 0; k < 3; k++) { pos[k] = sp[k]; nrm[k] = sn[k]; }
	}
	float wp[3], wn[3], vp[3];
	mul43(worldM, pos, wp);
	mul43n(worldM, nrm, wn);
	mul43(viewM, wp, vp);
	V4 c = mul44(projM, vp);
	v.x = c.x; v.y = c.y; v.z = c.z; v.w = c.w;

	if (on(0x17)) light_vertex(wp, wn, col);
	v.r = col[0]; v.g = col[1]; v.b = col[2]; v.a = col[3];

	// texture coordinates
	int tmap = R[0xc0] & 3;
	if (tmap == 0) {
		v.u = tc[0] * f24(R[0x48]) + f24(R[0x4a]);
		v.v = tc[1] * f24(R[0x49]) + f24(R[0x4b]);
	} else if (tmap == 1) {
		int src = (R[0xc0] >> 8) & 3;
		float in[3] = {pos[0], pos[1], pos[2]};
		if (src == 1) { in[0] = tc[0]; in[1] = tc[1]; in[2] = 0; }
		else if (src == 2) { float l = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]); for (int k = 0; k < 3; k++) in[k] = l > 0 ? nrm[k] / l : 0; }
		else if (src == 3) memcpy(in, nrm, 12);
		float o[3];
		mul43(tgenM, in, o);
		v.u = o[2] != 0 ? o[0] / o[2] : o[0];
		v.v = o[2] != 0 ? o[1] / o[2] : o[1];
	} else {
		// environment map: uses light directions as basis
		int l0 = R[0xc1] & 3, l1 = (R[0xc1] >> 8) & 3;
		float n[3] = {wn[0], wn[1], wn[2]};
		float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (nl > 0) for (int k = 0; k < 3; k++) n[k] /= nl;
		auto ldir = [&](int i, float *d) { d[0] = f24(R[0x63 + i * 3]); d[1] = f24(R[0x64 + i * 3]); d[2] = f24(R[0x65 + i * 3]); float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); if (l > 0) for (int k = 0; k < 3; k++) d[k] /= l; };
		float a[3], b[3];
		ldir(l0, a); ldir(l1, b);
		v.u = (1 + (n[0] * a[0] + n[1] * a[1] + n[2] * a[2])) * 0.5f;
		v.v = (1 + (n[0] * b[0] + n[1] * b[1] + n[2] * b[2])) * 0.5f;
	}

	// fog (view-space depth)
	if (on(0x1f)) v.fog = std::clamp((vp[2] + f24(R[0xcd])) * f24(R[0xce]), 0.0f, 1.0f);
	else v.fog = 1;
	return v;
}

// Clip space -> screen (pixels relative to the drawing offset).
static void to_screen(Vtx &v) {
	float iw = v.w != 0 ? 1.0f / v.w : 0;
	float ox = (R[0x4c] & 0xFFFF) / 16.0f, oy = (R[0x4d] & 0xFFFF) / 16.0f;
	v.sx = (f24(R[0x45]) + f24(R[0x42]) * v.x * iw - ox) * g_render_scale;
	v.sy = (f24(R[0x46]) + f24(R[0x43]) * v.y * iw - oy) * g_render_scale;
	v.sz = f24(R[0x47]) + f24(R[0x44]) * v.z * iw;
}

// ============================================================ textures
struct TexInfo {
	u32 addr, bufw, w, h, fmt;
	bool swz;
};
struct DecodedTex {
	std::vector<u32> px;   // RGBA8 (0xAABBGGRR)
	u64 hash = 0;
	u64 clutHash = 0;
	GLTexRef gl;           // GPU copy (GL renderer)
};
static DecodedTex *s_lastTex;   // entry returned by the last get_texture()
static std::unordered_map<u64, DecodedTex> s_texcache;
static u64 s_clut_hash;

static inline u64 hash_bytes(const u8 *p, size_t n) {
	u64 h = 0xcbf29ce484222325ull ^ n;
	size_t i = 0;
	for (; i + 8 <= n; i += 8) { u64 x; memcpy(&x, p + i, 8); h = (h ^ x) * 0x100000001b3ull; h ^= h >> 29; }
	for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
	return h;
}

static inline u32 c565(u16 x) { return 0xFF000000u | (((x >> 11) * 255 / 31) << 16) | ((((x >> 5) & 63) * 255 / 63) << 8) | ((x & 31) * 255 / 31); }
static inline u32 c5551(u16 x) { return ((x >> 15) ? 0xFF000000u : 0) | ((((x >> 10) & 31) * 255 / 31) << 16) | ((((x >> 5) & 31) * 255 / 31) << 8) | ((x & 31) * 255 / 31); }
static inline u32 c4444(u16 x) { return ((x >> 12) * 17u << 24) | (((x >> 8) & 15) * 17u << 16) | (((x >> 4) & 15) * 17u << 8) | ((x & 15) * 17u); }

static u32 clut_lookup(u32 idx) {
	u32 cm = R[0xc5];
	int cfmt = cm & 3, shift = (cm >> 2) & 31, mask = (cm >> 8) & 255, csa = (cm >> 16) & 31;
	u32 i = ((idx >> shift) & mask) | (csa << 4);
	if (cfmt == 3) { u32 v; memcpy(&v, clut + (i & 255) * 4, 4); return v; }
	u16 x; memcpy(&x, clut + (i & 511) * 2, 2);
	return cfmt == 0 ? c565(x) : cfmt == 1 ? c5551(x) : c4444(x);
}

static const int s_bpp_bits[16] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0};

// Byte offset of texel (x, y) in a (possibly swizzled) texture of `bits` bpp.
static inline u32 texel_offset(u32 x, u32 y, u32 bufw, int bits, bool swz) {
	u32 rowBytes = bufw * bits / 8;
	u32 bx = x * bits / 8;
	if (!swz) return y * rowBytes + bx;
	u32 blocksPerRow = rowBytes / 16;
	return ((y >> 3) * blocksPerRow + (bx >> 4)) * 128 + (y & 7) * 16 + (bx & 15);
}

// PSP DXT blocks put the colour part FIRST (lines[4], color1, color2), then
// the alpha part (DXT3: 4 x u16 rows of 4-bit alpha; DXT5: u32 + u16 = 48 bits
// of 3-bit indices, then alpha1, alpha2). Layout per PPSSPP TextureDecoder.
static void decode_dxt(const TexInfo &t, std::vector<u32> &out) {
	int bw = std::max<u32>(t.bufw, 4) / 4;
	int blockBytes = t.fmt == 8 ? 8 : 16;
	for (u32 by = 0; by < (t.h + 3) / 4; by++)
		for (u32 bx = 0; bx < (t.w + 3) / 4; bx++) {
			const u8 *b = gptr(t.addr + (by * bw + bx) * blockBytes);
			u16 c1, c2;
			memcpy(&c1, b + 4, 2);
			memcpy(&c2, b + 6, 2);
			int r1 = (c1 >> 8) & 0xF8, g1 = (c1 >> 3) & 0xFC, b1 = (c1 << 3) & 0xF8;
			int r2 = (c2 >> 8) & 0xF8, g2 = (c2 >> 3) & 0xFC, b2 = (c2 << 3) & 0xF8;
			u32 a = t.fmt == 8 ? 0xFF000000u : 0;
			auto mk = [&](int r, int g, int bb) { return a | ((u32)bb << 16) | ((u32)g << 8) | (u32)r; };
			u32 cols[4] = {mk(r1, g1, b1), mk(r2, g2, b2), 0, 0};
			if (c1 > c2) {
				cols[2] = mk((2 * r1 + r2) / 3, (2 * g1 + g2) / 3, (2 * b1 + b2) / 3);
				cols[3] = mk((r1 + 2 * r2) / 3, (g1 + 2 * g2) / 3, (b1 + 2 * b2) / 3);
			} else {
				cols[2] = mk((r1 + r2) / 2, (g1 + g2) / 2, (b1 + b2) / 2);
				cols[3] = 0;
			}
			// DXT5 alpha palette
			u8 alpha[8] = {0};
			u64 aidx = 0;
			if (t.fmt == 10) {
				u32 lo; u16 hi;
				memcpy(&lo, b + 8, 4);
				memcpy(&hi, b + 12, 2);
				aidx = ((u64)hi << 32) | lo;
				int a1 = b[14], a2 = b[15];
				alpha[0] = (u8)a1; alpha[1] = (u8)a2;
				if (a1 > a2) {
					for (int n = 1; n <= 6; n++) alpha[n + 1] = (u8)((a1 * ((7 - n) << 8) / 7 + a2 * (n << 8) / 7 + 31) >> 8);
				} else {
					for (int n = 1; n <= 4; n++) alpha[n + 1] = (u8)((a1 * ((5 - n) << 8) / 5 + a2 * (n << 8) / 5 + 31) >> 8);
					alpha[6] = 0;
					alpha[7] = 255;
				}
			}
			for (int y = 0; y < 4; y++) {
				u32 py = by * 4 + y;
				if (py >= t.h) break;
				u8 line = b[y];
				u16 arow = 0;
				if (t.fmt == 9) memcpy(&arow, b + 8 + y * 2, 2);
				u32 arow5 = (u32)(aidx >> (y * 12));
				for (int x = 0; x < 4; x++) {
					u32 pxx = bx * 4 + x;
					u32 c = cols[(line >> (2 * x)) & 3];
					if (t.fmt == 9) c = (c & 0xFFFFFF) | (((arow >> (4 * x)) & 15) << 28);
					else if (t.fmt == 10) c = (c & 0xFFFFFF) | ((u32)alpha[(arow5 >> (3 * x)) & 7] << 24);
					if (pxx < t.w) out[py * t.w + pxx] = c;
				}
			}
		}
}

void ge_flush();
static bool jobs_pending();
static bool jobs_overlap(u32 addr, u32 size);
static bool running_overlap(u32 addr, u32 size);
struct Shadow;
static Shadow *shadow_covering(u32 vaddr, u32 size);
static void resolve_shadow(Shadow &sh);
static bool shadow_dirty(const Shadow *sh);
extern u64 g_flush_why[8];
static const u32 *get_texture(const TexInfo &t) {
	// render-to-texture: draw pending pixels first if they target this texture's memory
	if ((t.addr & 0x0F000000) == 0x04000000 &&
	    (jobs_overlap(t.addr & 0x1FFFFFFF, 0x80000) || running_overlap(t.addr & 0x1FFFFFFF, 0x80000))) { g_flush_why[1]++; ge_flush(); }
	if (g_render_scale > 1 && (t.addr & 0x0F000000) == 0x04000000) {
		Shadow *sh = shadow_covering(t.addr & 0x1FFFFFFF, t.bufw * t.h * 4);
		if (sh && shadow_dirty(sh)) { ge_flush(); resolve_shadow(*sh); }
	}
	int bits = s_bpp_bits[t.fmt];
	bool isClut = t.fmt >= 4 && t.fmt <= 7;
	u32 rowBytes = t.bufw * bits / 8;
	size_t bytes = t.fmt >= 8 ? (size_t)std::max<u32>(t.bufw, 4) / 4 * ((t.h + 3) / 4) * (t.fmt == 8 ? 8 : 16)
	                          : (size_t)rowBytes * (t.swz ? ((t.h + 7) & ~7u) : t.h);
	u64 key = ((u64)t.addr << 32) ^ ((u64)t.fmt << 28) ^ ((u64)t.w << 16) ^ ((u64)t.h << 4) ^ ((u64)t.bufw << 40) ^ t.swz;
	u64 h = hash_bytes(gptr(t.addr), std::min<size_t>(bytes, 0x200000));
	DecodedTex &d = s_texcache[key];
	s_lastTex = &d;
	if (!d.px.empty() && d.hash == h && (!isClut || d.clutHash == s_clut_hash)) return d.px.data();
	if (jobs_pending()) g_flush_why[2]++;
	ge_flush();   // queued draws may still sample the old decode of this entry
	d.px.assign((size_t)t.w * t.h, 0);
	d.hash = h;
	d.clutHash = s_clut_hash;
	struct DumpAtEnd { const TexInfo &t; DecodedTex &d; ~DumpAtEnd() {
		static bool on = getenv("SSX_TEXDUMP") != nullptr;
		if (!on || (t.fmt < 8 && rt_time_us() < 40000000)) return;
		static std::map<u64, bool> done;
		u64 k = ((u64)t.addr << 8) ^ t.fmt;
		if (done[k]) return;
		done[k] = true;
		char nm[96];
		snprintf(nm, sizeof(nm), "tex_%08x_f%u_%ux%u_s%d.ppm", t.addr, t.fmt, t.w, t.h, (int)t.swz);
		FILE *f = fopen(nm, "wb");
		fprintf(f, "P6\n%u %u\n255\n", t.w, t.h);
		for (u32 i = 0; i < t.w * t.h; i++) { u32 c = d.px[i]; u8 px[3] = {(u8)c, (u8)(c >> 8), (u8)(c >> 16)}; fwrite(px, 1, 3, f); }
		fclose(f);
	} } dumper{t, d};
	if (t.fmt >= 8) { decode_dxt(t, d.px); return d.px.data(); }

	const u8 *base = gptr(t.addr);
	for (u32 y = 0; y < t.h; y++)
		for (u32 x = 0; x < t.w; x++) {
			u32 o = texel_offset(x, y, t.bufw, bits, t.swz);
			const u8 *p = base + o;
			u32 c;
			switch (t.fmt) {
			case 0: { u16 v; memcpy(&v, p, 2); c = c565(v); break; }
			case 1: { u16 v; memcpy(&v, p, 2); c = c5551(v); break; }
			case 2: { u16 v; memcpy(&v, p, 2); c = c4444(v); break; }
			case 3: memcpy(&c, p, 4); break;
			case 4: c = clut_lookup((x & 1) ? (*p >> 4) : (*p & 15)); break;
			case 5: c = clut_lookup(*p); break;
			case 6: { u16 v; memcpy(&v, p, 2); c = clut_lookup(v); break; }
			default: { u32 v; memcpy(&v, p, 4); c = clut_lookup(v); break; }
			}
			d.px[y * t.w + x] = c;
		}
	return d.px.data();
}

// ============================================================ framebuffer access
struct Target {
	u8 *fb;            // where pixels go: VRAM, or the high-resolution shadow (then fmt = 3)
	u16 *zb;
	u32 stride, zstride, fmt;
	u32 vfb, vzb;      // guest VRAM addresses (offsets from g_mem) of the real buffers
	u32 vstride, vzstride, vfmt;
};

static inline u32 fb_read(const Target &t, int x, int y) {
	if (t.fmt == 3) { u32 v; memcpy(&v, t.fb + (y * t.stride + x) * 4, 4); return v; }
	u16 v; memcpy(&v, t.fb + (y * t.stride + x) * 2, 2);
	return t.fmt == 0 ? c565(v) : t.fmt == 1 ? c5551(v) : c4444(v);
}

static inline void fb_write(const Target &t, int x, int y, u32 c, u32 mask) {
	if (t.fmt == 3) {
		u32 *p = (u32 *)(t.fb + (y * t.stride + x) * 4);
		*p = (*p & mask) | (c & ~mask);
		return;
	}
	u32 r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255, a = c >> 24;
	u16 v;
	if (t.fmt == 0) v = (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11);
	else if (t.fmt == 1) v = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >> 7) << 15);
	else v = (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12);
	u16 *p = (u16 *)(t.fb + (y * t.stride + x) * 2);
	u16 m16 = t.fmt == 0 ? (u16)(((mask & 0xF8) >> 3) | ((mask & 0xFC00) >> 5) | ((mask & 0xF80000) >> 8))
	        : t.fmt == 1 ? (u16)(((mask & 0xF8) >> 3) | ((mask & 0xF800) >> 6) | ((mask & 0xF80000) >> 9) | ((mask >> 31) << 15))
	                     : (u16)(((mask & 0xF0) >> 4) | ((mask & 0xF000) >> 8) | ((mask & 0xF00000) >> 12) | ((mask & 0xF0000000) >> 16));
	*p = (*p & m16) | (v & ~m16);
}

// ---------------------------------------------------------------- high-resolution shadows
// With render scale S > 1 every colour/depth buffer the GE draws into gets an
// S x S larger shadow (always RGBA8888 / u16). Drawing happens there; the
// display shows it directly. When the game needs the real VRAM contents
// (texture from a rendered buffer, block transfer) the shadow is averaged down.
int g_render_scale = 1;
struct Shadow {
	u32 base, stride, fmt;        // VRAM offset, stride (pixels), PSP pixel format
	int S;
	std::vector<u32> px;          // (stride*S) x (512*S)
	std::vector<u16> z;
	bool depth = false;
	int maxRow = -1;              // highest VRAM row drawn (for resolve / overlap checks)
	int dx0 = 1 << 30, dy0 = 1 << 30, dx1 = -1, dy1 = -1;   // drawn rectangle (VRAM pixels) since last resolve
	bool needsResolve = false;    // shadow newer than VRAM
	bool vramNewer = false;       // VRAM newer than shadow (block transfer into it)
	u64 lastDrawUs = 0;
};
static std::mutex s_shadow_mtx;
u64 g_sh_create, g_sh_upload, g_sh_resolve;
static std::map<u32, std::unique_ptr<Shadow>> s_shadows;   // colour, by VRAM offset
static std::map<u32, std::unique_ptr<Shadow>> s_zshadows;  // depth

static bool shadow_dirty(const Shadow *sh) { return sh->needsResolve && !sh->depth; }
static u32 vram_bytes(const Shadow &sh) { return sh.stride * (sh.depth ? 2 : (sh.fmt == 3 ? 4 : 2)); }

static Shadow *get_shadow(std::map<u32, std::unique_ptr<Shadow>> &m, u32 base, u32 stride, u32 fmt, bool depth) {
	std::lock_guard<std::mutex> lk(s_shadow_mtx);
	auto &sp = m[base];
	int S = g_render_scale;
	if (!sp || sp->stride != stride || sp->fmt != fmt || sp->S != S) {
		sp = std::make_unique<Shadow>();
		g_sh_create++;
		if (getenv("SSX_SHDBG")) rt_log(LOG_GE, "shadow create %s base %06x stride %u fmt %u", depth ? "Z" : "C", base & 0x1FFFFF, stride, fmt);
		sp->base = base; sp->stride = stride; sp->fmt = fmt; sp->S = S; sp->depth = depth;
		if (depth) sp->z.assign((size_t)stride * S * 512 * S, 0xFFFF);
		else sp->px.assign((size_t)stride * S * 512 * S, fmt == 0 ? 0xFF000000u : 0);
		sp->vramNewer = true;   // start from whatever VRAM holds
	}
	return sp.get();
}

// VRAM -> shadow (nearest upscale)
static void upload_shadow(Shadow &sh) {
	g_sh_upload++;
	int S = sh.S;
	u32 W = sh.stride * S;
	for (u32 y = 0; y < 512; y++) {
		const u8 *row = gptr(0x04000000 | sh.base) + (size_t)y * vram_bytes(sh);
		if ((size_t)(sh.base & 0x1FFFFF) + (size_t)(y + 1) * vram_bytes(sh) > 0x200000) break;
		for (u32 x = 0; x < sh.stride; x++) {
			if (sh.depth) {
				u16 v; memcpy(&v, row + x * 2, 2);
				for (int dy = 0; dy < S; dy++) for (int dx = 0; dx < S; dx++) sh.z[(size_t)(y * S + dy) * W + x * S + dx] = v;
			} else {
				u32 c;
				if (sh.fmt == 3) memcpy(&c, row + x * 4, 4);
				else { u16 v; memcpy(&v, row + x * 2, 2); c = sh.fmt == 0 ? c565(v) : sh.fmt == 1 ? c5551(v) : c4444(v); }
				for (int dy = 0; dy < S; dy++) for (int dx = 0; dx < S; dx++) sh.px[(size_t)(y * S + dy) * W + x * S + dx] = c;
			}
		}
	}
	sh.vramNewer = false;
}

// Run fn(y) for y in [y0, y1] on a few helper threads.
template <class F>
static void parallel_rows(int y0, int y1, F fn) {
	int n = y1 - y0 + 1;
	if (n <= 0) return;
	int nt = std::min(8, std::max(1, n / 16));
	std::vector<std::thread> ts;
	for (int t = 1; t < nt; t++)
		ts.emplace_back([&, t] { for (int y = y0 + t; y <= y1; y += nt) fn(y); });
	for (int y = y0; y <= y1; y += nt) fn(y);
	for (auto &th : ts) th.join();
}

// shadow -> VRAM (box filter)
static void resolve_shadow(Shadow &sh) {
	if (!sh.needsResolve || sh.depth) { sh.needsResolve = false; return; }
	g_sh_resolve++;
	int S = sh.S;
	u32 W = sh.stride * S;
	Target t{};
	t.fmt = sh.fmt;
	t.stride = sh.stride;
	t.fb = gptr(0x04000000 | sh.base);
	int x0 = std::max(sh.dx0, 0), x1 = std::min(sh.dx1, (int)sh.stride - 1);
	int y0 = std::max(sh.dy0, 0), y1 = std::min(sh.dy1, 511);
	parallel_rows(y0, y1, [&](int y) {
		for (int x = x0; x <= x1; x++) {
			u32 acc[4] = {0, 0, 0, 0};
			for (int dy = 0; dy < S; dy++)
				for (int dx = 0; dx < S; dx++) {
					u32 c = sh.px[(size_t)(y * S + dy) * W + x * S + dx];
					for (int k = 0; k < 4; k++) acc[k] += (c >> (8 * k)) & 255;
				}
			u32 c = 0;
			for (int k = 0; k < 4; k++) c |= (acc[k] / (S * S)) << (8 * k);
			fb_write(t, x, y, c, 0);
		}
	});
	sh.needsResolve = false;
	sh.dx0 = sh.dy0 = 1 << 30;
	sh.dx1 = sh.dy1 = -1;
}

static Shadow *shadow_covering(u32 vaddr, u32 size) {
	std::lock_guard<std::mutex> lk(s_shadow_mtx);
	for (auto &[base, sh] : s_shadows) {
		u32 hi = base + vram_bytes(*sh) * (u32)(sh->maxRow + 1);
		if (vaddr < hi && base < vaddr + size) return sh.get();
	}
	return nullptr;
}

// End of a frame (FINISH): bring VRAM up to date with everything drawn, so a
// stale shadow can never overwrite memory the game reuses later.
void ge_resolve_all() {
	if (gl_backend_active()) { gl_ge_frame_done(); return; }
	if (g_render_scale <= 1) return;
	std::vector<Shadow *> todo;
	{
		std::lock_guard<std::mutex> lk(s_shadow_mtx);
		for (auto &[b, sh] : s_shadows) if (sh->needsResolve) todo.push_back(sh.get());
	}
	for (Shadow *sh : todo) resolve_shadow(*sh);
}

// Called from the display: the high-resolution picture for a framebuffer, if it is current.
const u32 *ge_shadow_for_display(u32 fbAddr, u32 *stride, int *scale) {
	if (g_render_scale <= 1) return nullptr;
	std::lock_guard<std::mutex> lk(s_shadow_mtx);
	auto it = s_shadows.find(fbAddr & 0x1FFFFFFF);
	if (it == s_shadows.end() || rt_time_us() - it->second->lastDrawUs > 200000) return nullptr;
	*stride = it->second->stride * it->second->S;
	*scale = it->second->S;
	return it->second->px.data();
}

static Target current_target() {
	Target t;
	u32 fbp = (R[0x9c] & 0xFFFFF0) | ((R[0x9d] & 0xFF0000) << 8);
	u32 zbp = (R[0x9e] & 0xFFFFF0) | ((R[0x9f] & 0xFF0000) << 8);
	t.vfb = 0x04000000 | (fbp & 0x1FFFFF);
	t.vzb = 0x04000000 | (zbp & 0x1FFFFF);
	t.vstride = R[0x9d] & 0x7FC;
	t.vzstride = R[0x9f] & 0x7FC;
	t.vfmt = R[0xd2] & 3;
	if (!t.vstride) t.vstride = 512;
	if (!t.vzstride) t.vzstride = 512;
	if (g_render_scale <= 1 || gl_backend_active()) {
		t.fb = gptr(t.vfb);
		t.zb = (u16 *)gptr(t.vzb);
		t.stride = t.vstride; t.zstride = t.vzstride; t.fmt = t.vfmt;
		return t;
	}
	int S = g_render_scale;
	Shadow *cs = get_shadow(s_shadows, t.vfb, t.vstride, t.vfmt, false);
	Shadow *zs = get_shadow(s_zshadows, t.vzb, t.vzstride, 0, true);
	if (cs->vramNewer) upload_shadow(*cs);
	if (zs->vramNewer) upload_shadow(*zs);
	cs->lastDrawUs = rt_time_us();
	t.fb = (u8 *)cs->px.data();
	t.zb = zs->z.data();
	t.stride = t.vstride * S;
	t.zstride = t.vzstride * S;
	t.fmt = 3;
	return t;
}

// ============================================================ per-primitive pixel pipeline
enum : unsigned { F_TEX = 1, F_LINEAR = 2, F_FOG = 4, F_ATEST = 8, F_ZTEST = 16, F_BLEND = 32, F_FB32 = 64, F_GENERIC = 0x80000000u };
struct Pipe {
	Target tgt;
	int sx0, sy0, sx1, sy1;     // scissor (inclusive)
	bool clear, clearColor, clearAlpha, clearDepth;
	bool tex, texClampU, texClampV, texLinear, texRGBA, colorDouble;
	int texFunc;
	const u32 *texels;
	u32 tw, th;
	bool through;
	bool alphaTest; int atFunc; u32 atRef, atMask;
	bool depthTest; int zFunc; bool zWrite;
	bool blend; int bSrc, bDst, bEq; u32 fixA, fixB;
	bool fog; u32 fogColor;
	bool stencil; int stFunc; u32 stRef, stMask; int sFail, sZFail, sZPass;
	u32 writeMask;   // bits set = keep destination
	u32 texEnv;
	bool gouraud;
	int minZ, maxZ;
	unsigned spec;   // specialisation key (see shade_t), GENERIC for clear/stencil
	u16 mask16;      // writeMask converted to the 16-bit framebuffer format
	struct Shadow *shadow = nullptr;   // colour shadow being drawn into (render scale > 1)
	DecodedTex *texEntry = nullptr;    // GL renderer: texture cache entry
	u64 texVersion = 0;
	u32 fboTexAddr = 0;                // GL renderer: sample this render target
};
static Pipe P;
static std::mutex s_stats_mtx;

static inline bool cmp(int func, u32 a, u32 b) {
	switch (func) {
	case 0: return false; case 1: return true; case 2: return a == b; case 3: return a != b;
	case 4: return a < b; case 5: return a <= b; case 6: return a > b; default: return a >= b;
	}
}

static void setup_pipe(bool through) {
	Pipe &p = P;
	p.tgt = current_target();
	p.sx0 = R[0xd4] & 0x3FF; p.sy0 = (R[0xd4] >> 10) & 0x3FF;
	p.sx1 = R[0xd5] & 0x3FF; p.sy1 = (R[0xd5] >> 10) & 0x3FF;
	{
		int S = g_render_scale;
		p.sx0 *= S; p.sy0 *= S;
		p.sx1 = (p.sx1 + 1) * S - 1; p.sy1 = (p.sy1 + 1) * S - 1;
	}
	p.sx1 = std::min<int>(p.sx1, p.tgt.vstride * g_render_scale - 1);
	p.sy1 = std::min(p.sy1, 512 * g_render_scale - 1);
	p.through = through;
	p.clear = R[0xd3] & 1;
	p.clearColor = R[0xd3] & 0x100; p.clearAlpha = R[0xd3] & 0x200; p.clearDepth = R[0xd3] & 0x400;
	p.tex = !p.clear && on(0x1e);
	p.gouraud = R[0x50] & 1;
	if (p.tex) {
		TexInfo t;
		t.addr = (R[0xa0] & 0xFFFFF0) | ((R[0xa8] & 0x0F0000) << 8);
		t.bufw = R[0xa8] & 0xFFFF;
		t.w = 1u << (R[0xb8] & 0xF);
		t.h = 1u << ((R[0xb8] >> 8) & 0xF);
		t.fmt = R[0xc3] & 0xF;
		t.swz = R[0xc2] & 1;
		if (t.fmt > 10) t.fmt = 3;
		if (!t.bufw) t.bufw = t.w;
		p.fboTexAddr = 0;
		p.texEntry = nullptr;
		if (getenv("SSX_RTTDBG") && gl_backend_active() && (t.addr & 0x0F000000) == 0x04000000) {
			u32 b, st, fm;
			if (gl_find_color_target(t.addr, &b, &st, &fm)) {
				static std::map<u64, int> seen;
				u64 key = ((u64)t.addr << 8) ^ t.fmt ^ ((u64)p.tgt.vfb << 40);
				if (seen[key]++ == 0)
					rt_log(LOG_GE, "RTT? tex %08x fmt %u %ux%u bufw %u inside target %08x stride %u fmt %u (+%x) | drawing into %08x",
					       t.addr, t.fmt, t.w, t.h, t.bufw, b, st, fm, (t.addr & 0x0FFFFFFF) - b, p.tgt.vfb);
			}
		}
		if (gl_backend_active() && (t.addr & 0x0F000000) == 0x04000000 && t.fmt <= 3 &&
		    (t.addr & 0x0FFFFFFF) != (p.tgt.vfb & 0x0FFFFFFF) && gl_has_color_target(t.addr)) {
			p.fboTexAddr = t.addr & 0x0FFFFFFF;   // render-to-texture: sample the GPU target
			p.texels = nullptr;
		} else {
			p.texels = get_texture(t);
			p.texEntry = s_lastTex;
			p.texVersion = s_lastTex ? (s_lastTex->hash ^ (s_lastTex->clutHash * 0x9E3779B97F4A7C15ull)) : 0;
		}
		p.tw = t.w; p.th = t.h;
		p.texClampU = R[0xc7] & 1; p.texClampV = R[0xc7] & 0x100;
		p.texLinear = (R[0xc6] >> 8) & 1;
		p.texFunc = R[0xc9] & 7;
		p.texRGBA = R[0xc9] & 0x100;
		p.colorDouble = R[0xc9] & 0x10000;
		p.texEnv = R[0xca];
	}
	p.alphaTest = !p.clear && on(0x22);
	p.atFunc = R[0xdb] & 7; p.atRef = (R[0xdb] >> 8) & 255; p.atMask = (R[0xdb] >> 16) & 255;
	p.depthTest = !p.clear && on(0x23);
	p.zFunc = R[0xde] & 7;
	p.zWrite = p.clear ? p.clearDepth : (on(0x23) && !(R[0xe7] & 1));   // no Z writes without the depth test
	p.blend = !p.clear && on(0x21);
	p.bSrc = R[0xdf] & 15; p.bDst = (R[0xdf] >> 4) & 15; p.bEq = (R[0xdf] >> 8) & 7;
	p.fixA = R[0xe0]; p.fixB = R[0xe1];
	p.fog = !p.clear && !through && on(0x1f);
	p.fogColor = R[0xcf];
	p.stencil = !p.clear && on(0x24);
	p.stFunc = R[0xdc] & 7; p.stRef = (R[0xdc] >> 8) & 255; p.stMask = (R[0xdc] >> 16) & 255;
	p.sFail = R[0xdd] & 7; p.sZFail = (R[0xdd] >> 8) & 7; p.sZPass = (R[0xdd] >> 16) & 7;
	if (p.clear) {
		p.writeMask = (p.clearColor ? 0 : 0x00FFFFFF) | (p.clearAlpha ? 0 : 0xFF000000);
	} else {
		p.writeMask = (R[0xe8] & 0xFFFFFF) | ((R[0xe9] & 255) << 24);
	}
	if (g_render_scale > 1 && p.tgt.vfmt == 0) p.writeMask |= 0xFF000000u;   // 565: keep shadow alpha opaque
	p.minZ = R[0xd6] & 0xFFFF; p.maxZ = R[0xd7] & 0xFFFF;
	if (g_render_scale > 1) {
		std::lock_guard<std::mutex> lk(s_shadow_mtx);
		auto it = s_shadows.find(p.tgt.vfb);
		p.shadow = it != s_shadows.end() ? it->second.get() : nullptr;
		if (it != s_shadows.end()) it->second->maxRow = std::max(it->second->maxRow, std::min(511, (p.sy1 + 1) / g_render_scale - 1));
	}
	{
		u32 mask = p.writeMask;
		p.mask16 = p.tgt.fmt == 0 ? (u16)(((mask & 0xF8) >> 3) | ((mask & 0xFC00) >> 5) | ((mask & 0xF80000) >> 8))
		         : p.tgt.fmt == 1 ? (u16)(((mask & 0xF8) >> 3) | ((mask & 0xF800) >> 6) | ((mask & 0xF80000) >> 9) | ((mask >> 31) << 15))
		                          : (u16)(((mask & 0xF0) >> 4) | ((mask & 0xF000) >> 8) | ((mask & 0xF00000) >> 12) | ((mask & 0xF0000000) >> 16));
	}
	if (getenv("SSX_GESTATS")) {
		extern std::map<std::string, u64> g_gestats;
		char k[256];
		snprintf(k, sizeof(k), "STST %06x SOP %06x ste %d ", R[0xdc], R[0xdd], p.stencil);
		std::string extra = k;
		snprintf(k, sizeof(k), "clear%d thr%d tex%d fmt%u func%u rgba%d dbl%d lin%d clamp%d%d | at%d f%u ref%u | zt%d f%u zw%d | bl%d %u/%u eq%u fix %06x/%06x | st%d ct%d lop%d | fb%u fog%d mask%08x",
		         p.clear, through, p.tex, R[0xc3] & 15, p.texFunc, p.texRGBA, p.colorDouble, p.texLinear, p.texClampU, p.texClampV,
		         p.alphaTest, p.atFunc, p.atRef, p.depthTest, p.zFunc, p.zWrite, p.blend, p.bSrc, p.bDst, p.bEq, p.fixA, p.fixB,
		         p.stencil, on(0x27), on(0x28), p.tgt.vfmt, p.fog, p.writeMask);
		std::lock_guard<std::mutex> lk(s_stats_mtx);
		g_gestats[(p.stencil || (p.blend && (p.bSrc == 4 || p.bSrc == 5 || p.bDst == 4 || p.bDst == 5 || p.bSrc == 8 || p.bDst == 8)) ? extra : std::string()) + k]++;
		if (p.tex) {
			u32 ta = (R[0xa0] & 0xFFFFF0) | ((R[0xa8] & 0x0F0000) << 8);
			if ((ta & 0x0F000000) == 0x04000000) {
				snprintf(k, sizeof(k), "VRAMTEX addr %06x fmt %u %ux%u bufw %u | fb %06x stride %u fmt %u", ta & 0x1FFFFF, R[0xc3] & 15,
				         1u << (R[0xb8] & 15), 1u << ((R[0xb8] >> 8) & 15), R[0xa8] & 0xFFFF, p.tgt.vfb & 0x1FFFFF, p.tgt.vstride, p.tgt.vfmt);
				g_gestats[k]++;
			}
		}
	}
	if (p.clear || p.stencil) p.spec = F_GENERIC;
	else p.spec = (p.tex ? F_TEX : 0) | (p.tex && p.texLinear ? F_LINEAR : 0) | (p.fog ? F_FOG : 0) | (p.alphaTest ? F_ATEST : 0) |
	              (p.depthTest ? F_ZTEST : 0) | (p.blend ? F_BLEND : 0) | (p.tgt.fmt == 3 ? F_FB32 : 0);
}

static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static inline u32 sample(const Pipe &p, float s, float t) {
	auto fetch = [&](int x, int y) -> u32 {
		if (p.texClampU) x = std::clamp(x, 0, (int)p.tw - 1); else x &= p.tw - 1;
		if (p.texClampV) y = std::clamp(y, 0, (int)p.th - 1); else y &= p.th - 1;
		return p.texels[y * p.tw + x];
	};
	float fu = s * p.tw, fv = t * p.th;
	if (!p.texLinear) return fetch((int)floorf(fu), (int)floorf(fv));
	fu -= 0.5f; fv -= 0.5f;
	int x0 = (int)floorf(fu), y0 = (int)floorf(fv);
	int fx = (int)((fu - x0) * 256), fy = (int)((fv - y0) * 256);
	u32 c00 = fetch(x0, y0), c10 = fetch(x0 + 1, y0), c01 = fetch(x0, y0 + 1), c11 = fetch(x0 + 1, y0 + 1);
	u32 out = 0;
	for (int sh = 0; sh < 32; sh += 8) {
		int a = (c00 >> sh) & 255, b = (c10 >> sh) & 255, c = (c01 >> sh) & 255, d = (c11 >> sh) & 255;
		int top = a * (256 - fx) + b * fx, bot = c * (256 - fx) + d * fx;
		out |= (u32)((top * (256 - fy) + bot * fy) >> 16) << sh;
	}
	return out;
}

static inline int blend_factor(int f, bool src, int sa, int da, int sc, int dc, u32 fix, int ch) {
	switch (f) {
	case 0: return src ? dc : sc;
	case 1: return 255 - (src ? dc : sc);
	case 2: return sa;
	case 3: return 255 - sa;
	case 4: return da;
	case 5: return 255 - da;
	case 6: return 2 * sa;
	case 7: return 255 - std::min(2 * sa, 255);
	case 8: return 2 * da;
	case 9: return 255 - std::min(2 * da, 255);
	default: return (fix >> (8 * ch)) & 255;
	}
}

// Shade one pixel. r,g,b,a: 0..255 primitive color; z: 0..65535; s,t: texcoord.
// A pipeline flag: known at compile time in specialised variants, read at run time in the generic one.
#define SPEC(field, bit) ((F & F_GENERIC) ? (field) : ((F & (bit)) != 0))

template <bool LINEAR>
__attribute__((always_inline)) static inline u32 sample_t(const Pipe &p, float s, float t) {
	int mu = p.tw - 1, mv = p.th - 1;
	auto fetch = [&](int x, int y) -> u32 {
		x = p.texClampU ? std::clamp(x, 0, mu) : (x & mu);
		y = p.texClampV ? std::clamp(y, 0, mv) : (y & mv);
		return p.texels[y * p.tw + x];
	};
	float fu = s * p.tw, fv = t * p.th;
	if (!LINEAR) return fetch((int)floorf(fu), (int)floorf(fv));
	fu -= 0.5f; fv -= 0.5f;
	int x0 = (int)floorf(fu), y0 = (int)floorf(fv);
	int fx = (int)((fu - x0) * 256), fy = (int)((fv - y0) * 256);
	u32 c00 = fetch(x0, y0), c10 = fetch(x0 + 1, y0), c01 = fetch(x0, y0 + 1), c11 = fetch(x0 + 1, y0 + 1);
	u32 out = 0;
	for (int sh = 0; sh < 32; sh += 8) {
		int a = (c00 >> sh) & 255, b = (c10 >> sh) & 255, c = (c01 >> sh) & 255, d = (c11 >> sh) & 255;
		int top = a * (256 - fx) + b * fx, bot = c * (256 - fx) + d * fx;
		out |= (u32)((top * (256 - fy) + bot * fy) >> 16) << sh;
	}
	return out;
}

template <unsigned F>
static inline u32 fb_read_t(const Target &t, int x, int y) {
	if (!(F & F_GENERIC) && (F & F_FB32)) { u32 v; memcpy(&v, t.fb + (y * t.stride + x) * 4, 4); return v; }
	return fb_read(t, x, y);
}
template <unsigned F>
__attribute__((always_inline)) static inline void fb_write_t(const Target &t, int x, int y, u32 c, u32 mask, u16 mask16) {
	if (!(F & F_GENERIC) && (F & F_FB32)) {
		u32 *q = (u32 *)(t.fb + (y * t.stride + x) * 4);
		*q = mask ? ((*q & mask) | (c & ~mask)) : c;
		return;
	}
	if (F & F_GENERIC) { fb_write(t, x, y, c, mask); return; }
	u32 r = c & 255, g = (c >> 8) & 255, b = (c >> 16) & 255, a = c >> 24;
	u16 v;
	if (t.fmt == 0) v = (r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11);
	else if (t.fmt == 1) v = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((a >> 7) << 15);
	else v = (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | ((a >> 4) << 12);
	u16 *q = (u16 *)(t.fb + (y * t.stride + x) * 2);
	*q = mask16 ? (u16)((*q & mask16) | (v & ~mask16)) : v;
}

static thread_local u64 t_pixels;
template <unsigned F>
__attribute__((always_inline)) static inline void shade_t(const Pipe &p, int x, int y, float r, float g, float b, float a, float z, float s, float t, float fog) {
	t_pixels++;
	int iz = (int)z;
	if (!p.through && !p.clear && (iz < p.minZ || iz > p.maxZ)) return;
	iz = std::clamp(iz, 0, 65535);
	u16 *zp = &p.tgt.zb[y * p.tgt.zstride + x];
	int cr = (int)r, cg = (int)g, cb = (int)b, ca = (int)a;
	if (p.clear) {
		u32 c = (u32)clamp255(cr) | ((u32)clamp255(cg) << 8) | ((u32)clamp255(cb) << 16) | ((u32)clamp255(ca) << 24);
		fb_write(p.tgt, x, y, c, p.writeMask);
		if (p.clearDepth) *zp = (u16)iz;
		return;
	}
	if (SPEC(p.tex, F_TEX)) {
		u32 tc = (F & F_GENERIC) ? sample(p, s, t) : sample_t<(F & F_LINEAR) != 0>(p, s, t);
		int tr = tc & 255, tg = (tc >> 8) & 255, tb = (tc >> 16) & 255, ta = tc >> 24;
		int dbl = p.colorDouble ? 2 : 1;
		switch (p.texFunc) {
		case 0:  // modulate
			cr = (cr + 1) * tr * dbl / 256; cg = (cg + 1) * tg * dbl / 256; cb = (cb + 1) * tb * dbl / 256;
			if (p.texRGBA) ca = (ca + 1) * ta / 256;
			break;
		case 1:  // decal
			if (p.texRGBA) {
				cr = ((cr + 1) * (255 - ta) + (tr + 1) * ta) * dbl / 256;
				cg = ((cg + 1) * (255 - ta) + (tg + 1) * ta) * dbl / 256;
				cb = ((cb + 1) * (255 - ta) + (tb + 1) * ta) * dbl / 256;
			} else { cr = tr * dbl; cg = tg * dbl; cb = tb * dbl; }
			break;
		case 2: {  // blend with env color
			int er = p.texEnv & 255, eg = (p.texEnv >> 8) & 255, eb = (p.texEnv >> 16) & 255;
			cr = ((255 - tr) * cr + tr * er + 255) * dbl / 256;
			cg = ((255 - tg) * cg + tg * eg + 255) * dbl / 256;
			cb = ((255 - tb) * cb + tb * eb + 255) * dbl / 256;
			if (p.texRGBA) ca = (ca + 1) * ta / 256;
			break;
		}
		case 3:  // replace
			cr = tr * dbl; cg = tg * dbl; cb = tb * dbl;
			if (p.texRGBA) ca = ta;
			break;
		default:  // add
			cr = cr + tr * dbl; cg = cg + tg * dbl; cb = cb + tb * dbl;
			if (p.texRGBA) ca = (ca + 1) * ta / 256;
			break;
		}
		cr = clamp255(cr); cg = clamp255(cg); cb = clamp255(cb); ca = clamp255(ca);
	}
	if (SPEC(p.fog, F_FOG)) {
		int f = (int)(fog * 255);
		cr = (cr * f + (int)(p.fogColor & 255) * (255 - f) + 255) / 256;
		cg = (cg * f + (int)((p.fogColor >> 8) & 255) * (255 - f) + 255) / 256;
		cb = (cb * f + (int)((p.fogColor >> 16) & 255) * (255 - f) + 255) / 256;
	}
	if (SPEC(p.alphaTest, F_ATEST) && !cmp(p.atFunc, (u32)ca & p.atMask, p.atRef & p.atMask)) return;
	u32 dst = (SPEC(p.blend, F_BLEND) || p.stencil) ? fb_read_t<F>(p.tgt, x, y) : 0;
	if (p.stencil) {
		u32 ds = dst >> 24;
		if (!cmp(p.stFunc, p.stRef & p.stMask, ds & p.stMask)) return;
	}
	if (SPEC(p.depthTest, F_ZTEST) && !cmp(p.zFunc, (u32)iz, *zp)) return;
	if (p.zWrite) *zp = (u16)iz;
	if (SPEC(p.blend, F_BLEND)) {
		int dr = dst & 255, dg = (dst >> 8) & 255, db = (dst >> 16) & 255, da = dst >> 24;
		int sc[3] = {cr, cg, cb}, dc[3] = {dr, dg, db}, out[3];
		for (int k = 0; k < 3; k++) {
			int sf = blend_factor(p.bSrc, true, ca, da, sc[k], dc[k], p.fixA, k);
			int df = blend_factor(p.bDst, false, ca, da, sc[k], dc[k], p.fixB, k);
			int sv = ((sc[k] * 2 + 1) * (sf * 2 + 1)) / 1024, dv = ((dc[k] * 2 + 1) * (df * 2 + 1)) / 1024;
			switch (p.bEq) {
			case 0: out[k] = sv + dv; break;
			case 1: out[k] = sv - dv; break;
			case 2: out[k] = dv - sv; break;
			case 3: out[k] = std::min(sc[k], dc[k]); break;
			case 4: out[k] = std::max(sc[k], dc[k]); break;
			default: out[k] = abs(sc[k] - dc[k]); break;
			}
		}
		cr = clamp255(out[0]); cg = clamp255(out[1]); cb = clamp255(out[2]);
	}
	// alpha channel of the framebuffer holds stencil on the PSP
	u32 outA = (u32)ca;
	if (p.stencil) {
		int op = p.sZPass;
		u32 ds = dst >> 24;
		switch (op) {
		case 0: outA = ds; break; case 1: outA = 0; break; case 2: outA = p.stRef; break;
		case 3: outA = ~ds & 255; break; case 4: outA = std::min<u32>(ds + 1, 255); break; default: outA = ds ? ds - 1 : 0; break;
		}
	}
	u32 c = (u32)cr | ((u32)cg << 8) | ((u32)cb << 16) | (outA << 24);
	fb_write_t<F>(p.tgt, x, y, c, p.writeMask, p.mask16);
}

// ============================================================ rasterisation
// Each worker rasterises only the rows of its bands, so per-pixel order is kept.
static const int BAND_SHIFT = 4;           // 16-row bands
static const int NBANDS = (512 * 6) >> BAND_SHIFT;   // up to render scale 6
template <unsigned F>
static void raster_sprite_t(const Pipe &P, const Vtx &a, const Vtx &b, int ylo, int yhi) {
	float x0 = a.sx, y0 = a.sy, x1 = b.sx, y1 = b.sy;
	float u0 = a.u, v0 = a.v, u1 = b.u, v1 = b.v;
	if (x0 > x1) { std::swap(x0, x1); std::swap(u0, u1); }
	if (y0 > y1) { std::swap(y0, y1); std::swap(v0, v1); }
	int ix0 = std::max((int)ceilf(x0 - 0.5f), P.sx0), ix1 = std::min((int)ceilf(x1 - 0.5f) - 1, P.sx1);
	int iy0 = std::max((int)ceilf(y0 - 0.5f), P.sy0), iy1 = std::min((int)ceilf(y1 - 0.5f) - 1, P.sy1);
	iy0 = std::max(iy0, ylo); iy1 = std::min(iy1, yhi);
	if (iy0 > iy1) return;
	float du = x1 > x0 ? (u1 - u0) / (x1 - x0) : 0, dv = y1 > y0 ? (v1 - v0) / (y1 - y0) : 0;
	float sw = P.through ? 1.0f / P.tw : 1.0f, shh = P.through ? 1.0f / P.th : 1.0f;
	if (!P.tex) sw = shh = 0;
	for (int y = iy0; y <= iy1; y++) {
		float vv = v0 + (y + 0.5f - y0) * dv;
		for (int x = ix0; x <= ix1; x++) {
			float uu = u0 + (x + 0.5f - x0) * du;
			shade_t<F>(P, x, y, b.r, b.g, b.b, b.a, b.sz, uu * sw, vv * shh, b.fog);
		}
	}
}

template <unsigned F>
static void raster_triangle_t(const Pipe &P, const Vtx &v0, const Vtx &v1, const Vtx &v2, const Vtx &flat, int ylo, int yhi) {
	{
		int qy0 = std::max((int)floorf(std::min({v0.sy, v1.sy, v2.sy})), P.sy0);
		int qy1 = std::min((int)ceilf(std::max({v0.sy, v1.sy, v2.sy})), P.sy1);
		if (qy0 > yhi || qy1 < ylo || qy0 > qy1) return;
	}
	float area = (v1.sx - v0.sx) * (v2.sy - v0.sy) - (v2.sx - v0.sx) * (v1.sy - v0.sy);
	if (area == 0) return;
	int minx = std::max((int)floorf(std::min({v0.sx, v1.sx, v2.sx})), P.sx0);
	int maxx = std::min((int)ceilf(std::max({v0.sx, v1.sx, v2.sx})), P.sx1);
	int miny = std::max({(int)floorf(std::min({v0.sy, v1.sy, v2.sy})), P.sy0, ylo});
	int maxy = std::min({(int)ceilf(std::max({v0.sy, v1.sy, v2.sy})), P.sy1, yhi});
	if (minx > maxx || miny > maxy) return;
	float ia = 1.0f / area;
	float w0 = P.through ? 1 : 1 / v0.w, w1 = P.through ? 1 : 1 / v1.w, w2 = P.through ? 1 : 1 / v2.w;
	float sw = P.through && P.tex ? 1.0f / P.tw : 1.0f, shh = P.through && P.tex ? 1.0f / P.th : 1.0f;
	bool g = P.gouraud;
	auto edge_bias = [](const Vtx &a, const Vtx &b, float sign) {
		float dy = b.sy - a.sy, dx = b.sx - a.sx;
		bool topLeft = (dy * sign < 0) || (dy == 0 && dx * sign > 0);
		return topLeft ? 0.0f : -1e-5f;
	};
	float sgn = area > 0 ? 1.0f : -1.0f;
	float b0 = edge_bias(v1, v2, sgn), b1 = edge_bias(v2, v0, sgn), b2 = edge_bias(v0, v1, sgn);
	// edge functions are affine in x: e(x+1) = e(x) + dE
	float d0 = -(v2.sy - v1.sy) * ia, d1 = -(v0.sy - v2.sy) * ia;
	for (int y = miny; y <= maxy; y++) {
		float py = y + 0.5f, px = minx + 0.5f;
		float e0 = ((v2.sx - v1.sx) * (py - v1.sy) - (v2.sy - v1.sy) * (px - v1.sx)) * ia;
		float e1 = ((v0.sx - v2.sx) * (py - v2.sy) - (v0.sy - v2.sy) * (px - v2.sx)) * ia;
		// Exact covered span of this row: each edge function is affine in x.
		float lo = 0, hi = (float)(maxx - minx);
		auto clip_edge = [&](float e, float d, float b) {
			if (d > 1e-12f) lo = std::max(lo, (b - e) / d);
			else if (d < -1e-12f) hi = std::min(hi, (b - e) / d);
			else if (e < b) hi = -1;
		};
		clip_edge(e0, d0, b0);
		clip_edge(e1, d1, b1);
		clip_edge(1.0f - e0 - e1, -d0 - d1, b2);
		if (hi < lo) continue;
		int xs = std::max(0, (int)ceilf(lo) - 1), xe = std::min(maxx - minx, (int)floorf(hi) + 1);
		e0 += d0 * xs; e1 += d1 * xs;
		for (int x = minx + xs; x <= minx + xe; x++, e0 += d0, e1 += d1) {
			float e2 = 1.0f - e0 - e1;
			if (e0 < b0 || e1 < b1 || e2 < b2) continue;
			float z = e0 * v0.sz + e1 * v1.sz + e2 * v2.sz;
			float q0 = e0 * w0, q1 = e1 * w1, q2 = e2 * w2;
			float qi = 1.0f / (q0 + q1 + q2);
			float s = (q0 * v0.u + q1 * v1.u + q2 * v2.u) * qi * sw;
			float t = (q0 * v0.v + q1 * v1.v + q2 * v2.v) * qi * shh;
			float fog = e0 * v0.fog + e1 * v1.fog + e2 * v2.fog;
			if (g)
				shade_t<F>(P, x, y, e0 * v0.r + e1 * v1.r + e2 * v2.r, e0 * v0.g + e1 * v1.g + e2 * v2.g,
				      e0 * v0.b + e1 * v1.b + e2 * v2.b, e0 * v0.a + e1 * v1.a + e2 * v2.a, z, s, t, fog);
			else
				shade_t<F>(P, x, y, flat.r, flat.g, flat.b, flat.a, z, s, t, fog);
		}
	}
}

typedef void (*TriFn)(const Pipe &, const Vtx &, const Vtx &, const Vtx &, const Vtx &, int, int);
typedef void (*SprFn)(const Pipe &, const Vtx &, const Vtx &, int, int);
template <unsigned... Ks>
static constexpr std::array<TriFn, sizeof...(Ks)> make_tri(std::integer_sequence<unsigned, Ks...>) { return {raster_triangle_t<Ks>...}; }
template <unsigned... Ks>
static constexpr std::array<SprFn, sizeof...(Ks)> make_spr(std::integer_sequence<unsigned, Ks...>) { return {raster_sprite_t<Ks>...}; }
static const auto s_triFns = make_tri(std::make_integer_sequence<unsigned, 128>{});
static const auto s_sprFns = make_spr(std::make_integer_sequence<unsigned, 128>{});

static inline void raster_triangle(const Pipe &p, const Vtx &a, const Vtx &b, const Vtx &c, const Vtx &f, int ylo, int yhi) {
	if (p.spec == F_GENERIC) raster_triangle_t<F_GENERIC>(p, a, b, c, f, ylo, yhi);
	else s_triFns[p.spec](p, a, b, c, f, ylo, yhi);
}
static inline void raster_sprite(const Pipe &p, const Vtx &a, const Vtx &b, int ylo, int yhi) {
	if (p.spec == F_GENERIC) raster_sprite_t<F_GENERIC>(p, a, b, ylo, yhi);
	else s_sprFns[p.spec](p, a, b, ylo, yhi);
}

// ============================================================ deferred, multi-threaded drawing
struct Job {
	u32 pipe;
	u8 kind;   // 0 triangle, 1 sprite
	Vtx v[4];  // tri: v0 v1 v2 flat; sprite: a b
};
static std::vector<Job> s_jobs = [] { std::vector<Job> v; v.reserve(65536); return v; }();
static std::vector<Pipe> s_pipes;
static bool jobs_pending() { return !s_jobs.empty(); }
struct Range { u32 lo, hi; };
static std::vector<Range> s_dirty;   // guest memory written by queued jobs (colour + depth)
static bool jobs_overlap(u32 addr, u32 size) {
	if (s_jobs.empty()) return false;
	for (const Range &r : s_dirty)
		if (addr < r.hi && r.lo < addr + size) return true;
	return false;
}
static void note_target(const Pipe &p) {
	u32 fb = p.tgt.vfb & 0x1FFFFFFF, zb = p.tgt.vzb & 0x1FFFFFFF;
	u32 rows = (u32)(p.sy1 + 1) / g_render_scale;
	Range a{fb, fb + p.tgt.vstride * rows * (p.tgt.vfmt == 3 ? 4 : 2)}, b{zb, zb + p.tgt.vzstride * rows * 2};
	for (const Range &r : {a, b}) {
		bool known = false;
		for (const Range &d : s_dirty) if (d.lo == r.lo && d.hi == r.hi) known = true;
		if (!known) s_dirty.push_back(r);
	}
}
static int s_pipe_cur = -1;      // index of P in s_pipes for the current prim

static std::vector<std::thread> s_workers;
static std::mutex s_mtx;
static std::condition_variable s_cv_start, s_cv_done;
static u64 s_gen;
static int s_pending;
static int s_nthreads = 0;
// Batch being rasterised by the workers while the GE thread builds the next one.
static std::vector<Job> s_run_jobs;
static std::vector<Pipe> s_run_pipes;
static std::vector<Range> s_run_dirty;

std::atomic<u64> g_raster_px, g_raster_ns;
static std::vector<u32> s_band[NBANDS], s_run_band[NBANDS];
static std::atomic<int> s_next_band;
static void run_jobs(int tid) {
	(void)tid;
	u64 px0 = t_pixels;
	auto t0 = std::chrono::steady_clock::now();
	while (true) {
		int b = s_next_band.fetch_add(1);
		if (b >= NBANDS) break;
		int ylo = b << BAND_SHIFT, yhi = ylo + (1 << BAND_SHIFT) - 1;
		for (u32 idx : s_run_band[b]) {
			const Job &j = s_run_jobs[idx];
			const Pipe &p = s_run_pipes[j.pipe];
			if (j.kind == 0) raster_triangle(p, j.v[0], j.v[1], j.v[2], j.v[3], ylo, yhi);
			else raster_sprite(p, j.v[0], j.v[1], ylo, yhi);
		}
	}
	g_raster_px += t_pixels - px0;
	g_raster_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
}

static void worker_main(int tid) {
	char nm[16];
	snprintf(nm, sizeof(nm), "raster%d", tid);
	pthread_setname_np(pthread_self(), nm);
	u64 seen = 0;
	while (true) {
		{
			std::unique_lock<std::mutex> lk(s_mtx);
			s_cv_start.wait(lk, [&] { return s_gen != seen; });
			seen = s_gen;
		}
		run_jobs(tid);
		{
			std::lock_guard<std::mutex> lk(s_mtx);
			if (--s_pending == 0) s_cv_done.notify_all();
		}
	}
}

static void ge_wait_running() {
	std::unique_lock<std::mutex> lk(s_mtx);
	s_cv_done.wait(lk, [] { return s_pending == 0; });
}

static bool running_overlap(u32 addr, u32 size) {
	{
		std::lock_guard<std::mutex> lk(s_mtx);
		if (s_pending == 0) return false;
	}
	for (const Range &r : s_run_dirty)
		if (addr < r.hi && r.lo < addr + size) return true;
	return false;
}

u64 g_ge_flushes, g_ge_jobs;
u64 g_flush_why[8];

static void gl_flush_jobs();
// Hand the current batch to the raster workers and return (they run while we build the next).
void ge_submit() {
	if (gl_backend_active()) { gl_flush_jobs(); return; }
	if (s_jobs.empty()) { s_pipes.clear(); s_dirty.clear(); s_pipe_cur = -1; return; }
	if (s_nthreads == 0) {
		const char *e = getenv("SSX_GE_THREADS");
		int n = e ? atoi(e) : (int)std::min(15u, std::max(2u, std::thread::hardware_concurrency() - 1));
		s_nthreads = std::max(1, n);
		for (int i = 0; i < s_nthreads; i++) std::thread(worker_main, i).detach();
	}
	g_ge_flushes++;
	g_ge_jobs += s_jobs.size();
	ge_wait_running();
	s_run_jobs.swap(s_jobs);
	s_run_pipes.swap(s_pipes);
	s_run_dirty.swap(s_dirty);
	for (int b = 0; b < NBANDS; b++) { s_run_band[b].swap(s_band[b]); s_band[b].clear(); }
	s_next_band = 0;
	s_jobs.clear();
	s_pipes.clear();
	s_dirty.clear();
	s_pipe_cur = -1;
	{
		std::lock_guard<std::mutex> lk(s_mtx);
		s_pending = s_nthreads;
		s_gen++;
	}
	s_cv_start.notify_all();
}

// Full sync: everything queued so far is in guest memory when this returns.
void ge_flush() {
	if (gl_backend_active()) { gl_flush_jobs(); return; }
	extern u64 g_prof_flush;
	u64 t0 = rt_time_us();
	ge_submit();
	ge_wait_running();
	g_prof_flush += rt_time_us() - t0;
}

// Grow the drawn rectangle of the current colour shadow (scaled coords -> VRAM pixels).
static void note_drawn(float fx0, int y0, float fx1, int y1) {
	Shadow *sh = P.shadow;
	if (!sh) return;
	int S = g_render_scale;
	int x0 = std::max((int)floorf(fx0), P.sx0) / S, x1 = std::min((int)ceilf(fx1), P.sx1) / S;
	sh->needsResolve = true;
	sh->dx0 = std::min(sh->dx0, x0); sh->dx1 = std::max(sh->dx1, x1);
	sh->dy0 = std::min(sh->dy0, y0 / S); sh->dy1 = std::max(sh->dy1, y1 / S);
}

static void emit_triangle(const Vtx &a, const Vtx &b, const Vtx &c, const Vtx &flat) {
	if (s_pipe_cur < 0) { s_pipes.push_back(P); s_pipe_cur = (int)s_pipes.size() - 1; note_target(P); }
	Job j;
	j.pipe = s_pipe_cur;
	j.kind = 0;
	j.v[0] = a; j.v[1] = b; j.v[2] = c; j.v[3] = flat;
	int y0 = std::max((int)floorf(std::min({a.sy, b.sy, c.sy})), P.sy0);
	int y1 = std::min((int)ceilf(std::max({a.sy, b.sy, c.sy})), P.sy1);
	if (y0 > y1 || y1 < 0 || y0 > 512 * g_render_scale - 1) return;
	u32 idx = (u32)s_jobs.size();
	s_jobs.push_back(j);
	for (int bnd = std::max(y0, 0) >> BAND_SHIFT; bnd <= (std::min(y1, 512 * g_render_scale - 1) >> BAND_SHIFT); bnd++) s_band[bnd].push_back(idx);
	note_drawn(std::min({a.sx, b.sx, c.sx}), y0, std::max({a.sx, b.sx, c.sx}), y1);
	if (s_jobs.size() >= 4096) ge_submit();
}

static void draw_sprite(const Vtx &a, const Vtx &b) {
	if (s_pipe_cur < 0) { s_pipes.push_back(P); s_pipe_cur = (int)s_pipes.size() - 1; note_target(P); }
	Job j;
	j.pipe = s_pipe_cur;
	j.kind = 1;
	j.v[0] = a; j.v[1] = b;
	int y0 = std::max((int)floorf(std::min(a.sy, b.sy)), P.sy0), y1 = std::min((int)ceilf(std::max(a.sy, b.sy)), P.sy1);
	if (y0 > y1 || y1 < 0 || y0 > 512 * g_render_scale - 1) return;
	u32 idx = (u32)s_jobs.size();
	s_jobs.push_back(j);
	for (int bnd = std::max(y0, 0) >> BAND_SHIFT; bnd <= (std::min(y1, 512 * g_render_scale - 1) >> BAND_SHIFT); bnd++) s_band[bnd].push_back(idx);
	note_drawn(std::min(a.sx, b.sx), y0, std::max(a.sx, b.sx), y1);
}


// ============================================================ GL renderer: jobs -> GPU draws
static GLDrawState gl_state_for(const Pipe &p) {
	GLDrawState st{};
	st.fbAddr = p.tgt.vfb & 0x0FFFFFFF;
	st.fbStride = p.tgt.vstride;
	st.fbFmt = p.tgt.vfmt;
	st.zbAddr = p.tgt.vzb & 0x0FFFFFFF;
	st.sx0 = p.sx0; st.sy0 = p.sy0; st.sx1 = p.sx1; st.sy1 = p.sy1;
	st.clear = p.clear; st.clearColor = p.clearColor; st.clearAlpha = p.clearAlpha; st.clearDepth = p.clearDepth;
	st.tex = p.tex;
	st.texRef = p.texEntry ? &p.texEntry->gl : nullptr;
	st.texPixels = p.texels;
	st.texW = p.tw; st.texH = p.th;
	st.texVersion = p.texVersion;
	st.fboTexAddr = p.fboTexAddr;
	st.texLinear = p.texLinear; st.clampU = p.texClampU; st.clampV = p.texClampV;
	st.texFunc = p.texFunc; st.texRGBA = p.texRGBA; st.colorDouble = p.colorDouble; st.texEnv = p.texEnv;
	st.fog = p.fog; st.fogColor = p.fogColor;
	st.alphaTest = p.alphaTest; st.atFunc = p.atFunc; st.atRef = p.atRef; st.atMask = p.atMask;
	st.depthTest = p.depthTest; st.zFunc = p.zFunc; st.zWrite = p.zWrite;
	st.rangeTest = !p.through && !p.clear && (p.minZ > 0 || p.maxZ < 65535);
	st.minZ = p.minZ; st.maxZ = p.maxZ;
	st.blend = p.blend; st.bSrc = p.bSrc; st.bDst = p.bDst; st.bEq = p.bEq; st.fixA = p.fixA; st.fixB = p.fixB;
	st.stencil = p.stencil; st.stFunc = p.stFunc; st.stRef = p.stRef; st.stMask = p.stMask;
	st.sFail = p.sFail; st.sZFail = p.sZFail; st.sZPass = p.sZPass;
	st.writeMask = p.writeMask;
	return st;
}

static inline u32 pack_color(float r, float g, float b, float a) {
	auto c = [](float v) { return (u32)std::clamp((int)(v + 0.5f), 0, 255); };
	return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
}

static inline GLVert gl_vert(const Pipe &p, const Vtx &a, const Vtx &col, bool persp, float fog, float sz) {
	float T = (float)gl_target_width();
	float w = persp ? std::max(a.w, 1e-6f) : 1.0f;
	float xn = 2.0f * a.sx / T - 1.0f, yn = 2.0f * a.sy / T - 1.0f, zn = 2.0f * (sz / 65535.0f) - 1.0f;
	GLVert g;
	g.x = xn * w; g.y = yn * w; g.z = zn * w; g.w = w;
	g.u = a.u; g.v = a.v;
	if (p.through && p.tex && p.tw && p.th) { g.u /= (float)p.tw; g.v /= (float)p.th; }
	g.color = pack_color(col.r, col.g, col.b, col.a);
	g.fog = fog;
	return g;
}

static void gl_flush_jobs() {
	static std::vector<GLVert> verts;
	size_t i = 0;
	while (i < s_jobs.size()) {
		u32 pi = s_jobs[i].pipe;
		const Pipe &p = s_pipes[pi];
		verts.clear();
		size_t j = i;
		for (; j < s_jobs.size() && s_jobs[j].pipe == pi; j++) {
			const Job &jb = s_jobs[j];
			if (jb.kind == 0) {
				bool persp = !p.through;
				for (int k = 0; k < 3; k++) {
					const Vtx &col = p.gouraud ? jb.v[k] : jb.v[3];
					verts.push_back(gl_vert(p, jb.v[k], col, persp, jb.v[k].fog, jb.v[k].sz));
				}
			} else {
				// sprite: colour, depth and fog of the second vertex, texcoords from both corners
				const Vtx &a = jb.v[0], &b = jb.v[1];
				Vtx c00 = a, c10 = a, c01 = a, c11 = b;
				c10.sx = b.sx; c10.u = b.u;
				c01.sy = b.sy; c01.v = b.v;
				const Vtx *quad[6] = {&c00, &c10, &c01, &c10, &c11, &c01};
				for (const Vtx *q : quad) verts.push_back(gl_vert(p, *q, b, false, b.fog, b.sz));
			}
		}
		gl_ge_draw(gl_state_for(p), verts.data(), (int)verts.size());
		i = j;
	}
	s_jobs.clear();
	s_pipes.clear();
	s_dirty.clear();
	for (int b = 0; b < NBANDS; b++) s_band[b].clear();
	s_pipe_cur = -1;
}

// Clip a triangle against the near plane (z > -w) in clip space, then rasterise.
static void clip_and_draw_slow(Vtx a, Vtx b, Vtx c, bool cullOn, int cullCCW);
static inline void clip_and_draw(const Vtx &a, const Vtx &b, const Vtx &c, bool cullOn, int cullCCW) {
	if (P.through) { emit_triangle(a, b, c, c); return; }
	if (a.z + a.w < 0 || b.z + b.w < 0 || c.z + c.w < 0) { clip_and_draw_slow(a, b, c, cullOn, cullCCW); return; }
	// screen positions were computed once per vertex in draw_prim
	if (cullOn) {
		float area = (b.sx - a.sx) * (c.sy - a.sy) - (c.sx - a.sx) * (b.sy - a.sy);
		if (cullCCW ? area < 0 : area > 0) return;
	}
	// trivially outside the scissor?
	if (std::max({a.sx, b.sx, c.sx}) < P.sx0 || std::min({a.sx, b.sx, c.sx}) > P.sx1 + 1 ||
	    std::max({a.sy, b.sy, c.sy}) < P.sy0 || std::min({a.sy, b.sy, c.sy}) > P.sy1 + 1) return;
	emit_triangle(a, b, c, c);
}

static void clip_and_draw_slow(Vtx a, Vtx b, Vtx c, bool cullOn, int cullCCW) {
	auto lerp = [](const Vtx &p, const Vtx &q, float t) {
		Vtx r;
		r.x = p.x + (q.x - p.x) * t; r.y = p.y + (q.y - p.y) * t; r.z = p.z + (q.z - p.z) * t; r.w = p.w + (q.w - p.w) * t;
		r.u = p.u + (q.u - p.u) * t; r.v = p.v + (q.v - p.v) * t;
		r.r = p.r + (q.r - p.r) * t; r.g = p.g + (q.g - p.g) * t; r.b = p.b + (q.b - p.b) * t; r.a = p.a + (q.a - p.a) * t;
		r.fog = p.fog + (q.fog - p.fog) * t;
		return r;
	};
	Vtx in[8] = {a, b, c}, out[8];
	int n = 3;
	if (!P.through) {
		int m = 0;
		for (int i = 0; i < n; i++) {
			const Vtx &p = in[i], &q = in[(i + 1) % n];
			float dp = p.z + p.w, dq = q.z + q.w;
			if (dp >= 0) out[m++] = p;
			if ((dp >= 0) != (dq >= 0)) out[m++] = lerp(p, q, dp / (dp - dq));
		}
		if (m < 3) return;
		for (int i = 0; i < m; i++) { in[i] = out[i]; to_screen(in[i]); }
		n = m;
		if (cullOn) {
			float area = (in[1].sx - in[0].sx) * (in[2].sy - in[0].sy) - (in[2].sx - in[0].sx) * (in[1].sy - in[0].sy);
			if (cullCCW ? area < 0 : area > 0) return;
		}
	}
	for (int i = 1; i + 1 < n; i++) emit_triangle(in[0], in[i], in[i + 1], c);
}

static void draw_prim(u32 op) {
	int type = (op >> 16) & 7;
	int count = op & 0xFFFF;
	u32 vt = R[0x12];
	VFormat f = decode_vtype(vt);
	setup_pipe(f.through);
	s_pipe_cur = -1;
	s_prims++;
	static std::vector<Vtx> vs;
	vs.resize(count);
	int idxSize = f.idx == 1 ? 1 : f.idx == 2 ? 2 : f.idx == 3 ? 4 : 0;
	if (!idxSize) {
		for (int i = 0; i < count; i++) vs[i] = transform_vertex(f, gptr(s_vaddr + i * f.size * f.nm));
	} else {
		// indexed strips reuse vertices: transform each distinct index once
		static std::vector<u32> stamp;
		static std::vector<Vtx> cache;
		static u32 gen;
		if (++gen == 0) { std::fill(stamp.begin(), stamp.end(), 0); gen = 1; }
		for (int i = 0; i < count; i++) {
			u32 index = idxSize == 1 ? LB(s_iaddr + i) : idxSize == 2 ? LH(s_iaddr + 2 * i) : LW(s_iaddr + 4 * i);
			if (index < 65536) {
				if (index >= stamp.size()) { stamp.resize(index + 1, 0); cache.resize(index + 1); }
				if (stamp[index] != gen) { cache[index] = transform_vertex(f, gptr(s_vaddr + index * f.size * f.nm)); stamp[index] = gen; }
				vs[i] = cache[index];
			} else {
				vs[i] = transform_vertex(f, gptr(s_vaddr + index * f.size * f.nm));
			}
		}
	}
	if (!f.through)
		for (int i = 0; i < count; i++)
			if (vs[i].z + vs[i].w >= 0) to_screen(vs[i]);
	if (idxSize) s_iaddr += count * idxSize;
	else s_vaddr += count * f.size * f.nm;

	static double traceAt = getenv("SSX_GETRACE_AT") ? atof(getenv("SSX_GETRACE_AT")) : -1;
	if (traceAt >= 0 && s_traceState == 0 && (s_prims & 63) == 0 && rt_time_us() / 1e6 >= traceAt) s_traceState = 1;
	if (s_traceState == 2) ge_flush();
	u64 px0 = t_pixels;
	static int dbg = getenv("SSX_GEDEBUG") ? atoi(getenv("SSX_GEDEBUG")) : 0;
	if (dbg > 0) {
		dbg--;
		Vtx a = vs[0];
		if (!f.through) to_screen(a);
		rt_log(LOG_GE, "prim type %d n %d vt %06x clear %d tex %d scissor %d,%d-%d,%d fb %p fmt %u stride %u | v0 clip %.2f %.2f %.2f %.2f scr %.1f %.1f %.1f col %.0f %.0f %.0f %.0f uv %.3f %.3f",
		       type, count, vt, P.clear, P.tex, P.sx0, P.sy0, P.sx1, P.sy1, (void *)P.tgt.fb, P.tgt.fmt, P.tgt.stride,
		       a.x, a.y, a.z, a.w, a.sx, a.sy, a.sz, a.r, a.g, a.b, a.a, a.u, a.v);
		if (count > 1) { Vtx b = vs[1]; if (!f.through) to_screen(b); rt_log(LOG_GE, "   v1 scr %.1f %.1f %.1f col %.0f %.0f %.0f %.0f uv %.3f %.3f  vaddr %08x size %d", b.sx, b.sy, b.sz, b.r, b.g, b.b, b.a, b.u, b.v, s_vaddr, f.size); }
	}
	bool cullOn = !f.through && on(0x1d) && !P.clear;
	int cullCCW = R[0x9b] & 1;
	switch (type) {
	case 3:  // triangles
		for (int i = 0; i + 2 < count; i += 3) clip_and_draw(vs[i], vs[i + 1], vs[i + 2], cullOn, cullCCW);
		break;
	case 4:  // strip
		for (int i = 0; i + 2 < count; i++) {
			if (i & 1) clip_and_draw(vs[i + 1], vs[i], vs[i + 2], cullOn, cullCCW);
			else clip_and_draw(vs[i], vs[i + 1], vs[i + 2], cullOn, cullCCW);
		}
		break;
	case 5:  // fan
		for (int i = 1; i + 1 < count; i++) clip_and_draw(vs[0], vs[i], vs[i + 1], cullOn, cullCCW);
		break;
	case 6:  // sprites
		for (int i = 0; i + 1 < count; i += 2) {
			Vtx a = vs[i], b = vs[i + 1];
			if (!f.through) { to_screen(a); to_screen(b); }
			draw_sprite(a, b);
		}
		break;
	case 1: case 2: case 0:  // lines / points: draw as thin sprites
		for (int i = 0; i < count; i++) {
			Vtx a = vs[i];
			if (!f.through) to_screen(a);
			Vtx b = a; b.sx += g_render_scale; b.sy += g_render_scale;
			draw_sprite(a, b);
		}
		break;
	}
	if (s_traceState == 2) {
		ge_flush();
		Vtx a = vs[0];
		if (!f.through) to_screen(a);
		if (P.clear || type == 4) rt_log(LOG_GE, "T   viewport z: scale %f pos %f  minz %u maxz %u  zbuf %p clearDepth %d v0.sz %.1f",
		       f24(R[0x44]), f24(R[0x47]), R[0xd6] & 0xFFFF, R[0xd7] & 0xFFFF, (void *)P.tgt.zb, P.clearDepth, a.sz);
		{
			float bx0 = 1e9, by0 = 1e9, bx1 = -1e9, by1 = -1e9;
			for (auto vv : vs) { if (!f.through) to_screen(vv); bx0 = std::min(bx0, vv.sx); bx1 = std::max(bx1, vv.sx); by0 = std::min(by0, vv.sy); by1 = std::max(by1, vv.sy); }
			rt_log(LOG_GE, "T   bbox %.0f,%.0f - %.0f,%.0f", bx0, by0, bx1, by1);
		}
		rt_log(LOG_GE, "T   texaddr %08x bufw %u swz %u clut %06x cmode %06x", (R[0xa0] & 0xFFFFF0) | ((R[0xa8] & 0x0F0000) << 8), R[0xa8] & 0xFFFF, R[0xc2] & 1, (R[0xb0] & 0xFFFFF0) | ((R[0xb1] & 0x0F0000) << 8), R[0xc5]);
		rt_log(LOG_GE, "T prim %d n %3d vt %06x thr %d tex %d fmt %u %ux%u zt %d/%d zw %d blend %d at %d cull %d tmap %u px %llu | v0 clip(%.1f %.1f %.1f %.1f) scr(%.0f %.0f %.0f) a %.0f",
		       type, count, vt, f.through, P.tex, R[0xc3] & 15, P.tw, P.th, P.depthTest, P.zFunc, P.zWrite, P.blend, P.alphaTest, cullOn ? 1 + cullCCW : 0,
		       R[0xc0] & 3, (unsigned long long)(t_pixels - px0), a.x, a.y, a.z, a.w, a.sx, a.sy, a.sz, a.a);
	}
}


// ============================================================ Bezier patches (BEZIER, cmd 0x05)
// Control points form an nu x nv grid of cubic patches (3n+1 points per side);
// each patch is tessellated PATCHDIVISION times and drawn as triangles.
// Semantics follow PPSSPP GPU/Common/SplineCommon.cpp.
struct CtrlPt { float pos[3], nrm[3], col[4], tc[2]; };

static void draw_bezier(u32 op) {
	int nu = op & 0xFF, nv = (op >> 8) & 0xFF;
	VFormat f = decode_vtype(R[0x12]);
	int idxSize = f.idx == 1 ? 1 : f.idx == 2 ? 2 : f.idx == 3 ? 4 : 0;
	int npts = nu * nv;
	auto advance = [&] {
		if (idxSize) s_iaddr += npts * idxSize;
		else s_vaddr += npts * f.size * f.nm;
	};
	if (nu < 4 || nv < 4 || f.through) { advance(); return; }
	setup_pipe(false);
	s_pipe_cur = -1;
	s_prims++;
	static std::vector<CtrlPt> cp;
	cp.resize(npts);
	for (int i = 0; i < npts; i++) {
		u32 index = idxSize == 1 ? LB(s_iaddr + i) : idxSize == 2 ? LH(s_iaddr + 2 * i) : idxSize == 4 ? LW(s_iaddr + 4 * i) : (u32)i;
		const u8 *q = gptr(s_vaddr + index * f.size * f.nm);
		CtrlPt &c = cp[i];
		for (int k = 0; k < 3; k++) c.pos[k] = rd(q + f.oP, f.pos, k, true);
		for (int k = 0; k < 3; k++) c.nrm[k] = f.oN >= 0 ? rd(q + f.oN, f.nrm, k, true) : 0;
		if (f.oC >= 0) read_color(q + f.oC, f.col, c.col);
		else { unpack_rgb(R[0x55], c.col); c.col[3] = R[0x58] & 255; }
		c.tc[0] = f.oT >= 0 ? rdu(q + f.oT, f.tc, 0, true) : 0;
		c.tc[1] = f.oT >= 0 ? rdu(q + f.oT, f.tc, 1, true) : 0;
		if (f.oW >= 0) {  // skin control points before tessellating (as PPSSPP does)
			float sp[3] = {0, 0, 0}, sn[3] = {0, 0, 0};
			for (int b = 0; b < f.nw; b++) {
				float w = f.wt == 1 ? q[f.oW + b] / 128.0f : rdu(q + f.oW, f.wt, b, true);
				if (w == 0) continue;
				float tp[3], tn[3];
				mul43(boneM[b], c.pos, tp);
				mul43n(boneM[b], c.nrm, tn);
				for (int k = 0; k < 3; k++) { sp[k] += w * tp[k]; sn[k] += w * tn[k]; }
			}
			for (int k = 0; k < 3; k++) { c.pos[k] = sp[k]; c.nrm[k] = sn[k]; }
		}
	}
	advance();

	int tu = std::max<int>(1, R[0x36] & 0x7F), tv = std::max<int>(1, (R[0x36] >> 8) & 0x7F);
	static int dbgN = getenv("SSX_BZDEBUG") ? atoi(getenv("SSX_BZDEBUG")) : 0;
	if (dbgN > 0 && rt_time_us() > 80000000) {
		dbgN--;
		rt_log(LOG_GE, "bezier %dx%d vt %06x idx %d div %dx%d prim %u facing %u cull %d/%u tex %d fmt %u | cp0 %.3f %.3f %.3f tc %.3f %.3f col %.0f %.0f %.0f %.0f | cpN %.3f %.3f %.3f",
		       nu, nv, R[0x12], idxSize, tu, tv, R[0x37] & 3, R[0x38] & 1, on(0x1d), R[0x9b] & 1, P.tex, R[0xc3] & 15,
		       cp[0].pos[0], cp[0].pos[1], cp[0].pos[2], cp[0].tc[0], cp[0].tc[1], cp[0].col[0], cp[0].col[1], cp[0].col[2], cp[0].col[3],
		       cp[npts - 1].pos[0], cp[npts - 1].pos[1], cp[npts - 1].pos[2]);
	}
	bool reverseNormals = R[0x38] & 1;
	int npu = (nu - 1) / 3, npv = (nv - 1) / 3;
	auto bern = [](float t, float *b, float *d) {
		float it = 1 - t;
		b[0] = it * it * it; b[1] = 3 * t * it * it; b[2] = 3 * t * t * it; b[3] = t * t * t;
		d[0] = -3 * it * it; d[1] = 9 * t * t - 12 * t + 3; d[2] = 3 * (2 - 3 * t) * t; d[3] = 3 * t * t;
	};
	bool cullOn = on(0x1d) && !P.clear;
	int cullCCW = R[0x9b] & 1;
	static std::vector<Vtx> grid;
	grid.resize((tu + 1) * (tv + 1));
	for (int pv = 0; pv < npv; pv++)
		for (int pu = 0; pu < npu; pu++) {
			const CtrlPt *base = &cp[pv * 3 * nu + pu * 3];
			for (int j = 0; j <= tv; j++) {
				float bv[4], dv[4];
				bern((float)j / tv, bv, dv);
				for (int i = 0; i <= tu; i++) {
					float bu[4], du[4];
					bern((float)i / tu, bu, du);
					float pos[3] = {0, 0, 0}, nrm[3] = {0, 0, 0}, col[4] = {0, 0, 0, 0}, tc[2] = {0, 0};
					float dpu[3] = {0, 0, 0}, dpv[3] = {0, 0, 0};
					for (int y = 0; y < 4; y++)
						for (int x = 0; x < 4; x++) {
							const CtrlPt &c = base[y * nu + x];
							float w = bu[x] * bv[y];
							for (int k = 0; k < 3; k++) {
								pos[k] += w * c.pos[k];
								nrm[k] += w * c.nrm[k];
								dpu[k] += du[x] * bv[y] * c.pos[k];
								dpv[k] += bu[x] * dv[y] * c.pos[k];
							}
							for (int k = 0; k < 4; k++) col[k] += w * c.col[k];
							tc[0] += w * c.tc[0];
							tc[1] += w * c.tc[1];
						}
					if (f.oT < 0) { tc[0] = pu + (float)i / tu; tc[1] = pv + (float)j / tv; }
					if (f.oN < 0) {
						nrm[0] = dpu[1] * dpv[2] - dpu[2] * dpv[1];
						nrm[1] = dpu[2] * dpv[0] - dpu[0] * dpv[2];
						nrm[2] = dpu[0] * dpv[1] - dpu[1] * dpv[0];
						if (reverseNormals) for (int k = 0; k < 3; k++) nrm[k] = -nrm[k];
					}
					Vtx &v = grid[j * (tu + 1) + i];
					v = transform_attrs(f, nullptr, pos, nrm, col, tc);
					if (v.z + v.w >= 0) to_screen(v);
				}
			}
			for (int j = 0; j < tv; j++)
				for (int i = 0; i < tu; i++) {
					const Vtx &a = grid[j * (tu + 1) + i], &b = grid[j * (tu + 1) + i + 1];
					const Vtx &c = grid[(j + 1) * (tu + 1) + i], &d = grid[(j + 1) * (tu + 1) + i + 1];
					clip_and_draw(a, c, b, cullOn, cullCCW);
					clip_and_draw(b, c, d, cullOn, cullCCW);
				}
		}
}

// ============================================================ block transfer (TRXKICK)
void ge_flush();
static void block_transfer() {
	if (!s_jobs.empty()) g_flush_why[3]++;
	ge_flush();
	u32 src = (R[0xb2] & 0xFFFFF0) | ((R[0xb3] & 0xFF0000) << 8);
	u32 dst = (R[0xb4] & 0xFFFFF0) | ((R[0xb5] & 0xFF0000) << 8);
	u32 sw = R[0xb3] & 0xFFFF, dw = R[0xb5] & 0xFFFF;
	u32 sx = R[0xeb] & 0x3FF, sy = (R[0xeb] >> 10) & 0x3FF;
	u32 dx = R[0xec] & 0x3FF, dy = (R[0xec] >> 10) & 0x3FF;
	u32 w = (R[0xee] & 0x3FF) + 1, h = ((R[0xee] >> 10) & 0x3FF) + 1;
	u32 bpp = (R[0xea] & 1) ? 4 : 2;
	if (g_render_scale > 1) {
		if (Shadow *sh = shadow_covering(src & 0x1FFFFFFF, sw * (sy + h) * bpp)) resolve_shadow(*sh);
		if (Shadow *sh = shadow_covering(dst & 0x1FFFFFFF, dw * (dy + h) * bpp)) sh->vramNewer = true;
	}
	for (u32 y = 0; y < h; y++)
		memmove(gptr(dst + ((dy + y) * dw + dx) * bpp), gptr(src + ((sy + y) * sw + sx) * bpp), w * bpp);
}

// ============================================================ command entry
u64 g_cmd_hist[256];
void ge_exec_cmd(u32 cmd, u32 op, u32 pc) {
	(void)pc;
	g_cmd_hist[cmd]++;
	u32 data = op & 0xFFFFFF;
	R[cmd] = data;
	switch (cmd) {
	case 0x01: s_vaddr = ((g_ge_base & 0x0F000000) | data) + g_ge_offset; break;
	case 0x02: s_iaddr = ((g_ge_base & 0x0F000000) | data) + g_ge_offset; break;
	case 0x04: draw_prim(data); break;
	case 0x05: draw_bezier(data); break;
	case 0x2a: boneN = data; break;
	case 0x2b: if (boneN < 96) boneM[boneN / 12][boneN % 12] = f24(data); boneN++; break;
	case 0x2c: case 0x2d: case 0x2e: case 0x2f: case 0x30: case 0x31: case 0x32: case 0x33:
		morphW[cmd - 0x2c] = f24(data); break;
	case 0x3a: worldN = data & 15; break;
	case 0x3b: if (worldN < 12) worldM[worldN] = f24(data); worldN++; break;
	case 0x3c: viewN = data & 15; break;
	case 0x3d: if (viewN < 12) viewM[viewN] = f24(data); viewN++; break;
	case 0x3e: projN = data & 15; break;
	case 0x3f: if (projN < 16) projM[projN] = f24(data); projN++; break;
	case 0x40: tgenN = data & 15; break;
	case 0x41: if (tgenN < 12) tgenM[tgenN] = f24(data); tgenN++; break;
	case 0xc4: {  // CLOAD: copy palette into the GE
		u32 a = (R[0xb0] & 0xFFFFF0) | ((R[0xb1] & 0x0F0000) << 8);
		u32 n = std::min<u32>((data & 0x3F) * 32, sizeof(clut));
		memcpy(clut, gptr(a), n);
		s_clut_hash = hash_bytes(clut, n) ^ R[0xc5];
		break;
	}
	case 0xc5: s_clut_hash ^= 0x9E3779B97F4A7C15ull * (data + 1); break;
	case 0xea: block_transfer(); break;
	case 0x0f:  // FINISH: frame boundary for tracing
		if (s_traceState == 1) { s_traceState = 2; rt_log(LOG_GE, "T ---- frame trace start"); }
		else if (s_traceState == 2) { s_traceState = 3; rt_log(LOG_GE, "T ---- frame trace end"); }
		break;
	default: break;
	}
}
