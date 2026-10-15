// Software GE renderer: executes GE drawing commands and writes pixels straight
// into guest VRAM, so render-to-texture and CPU readbacks behave like the PSP.
// Register semantics follow PPSSPP's GPU/ge_constants.h and its software
// renderer (public, GPL); see NOTES.md.
#include <algorithm>
#include <array>
#include <utility>
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <pthread.h>
#include <unordered_map>
#include <vector>

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

static inline float f24(u32 v) { u32 b = v << 8; float f; memcpy(&f, &b, 4); return f; }
static inline bool on(u32 cmd) { return R[cmd] & 1; }

void ge_dump_stats() {
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
		v.sx = pos[0]; v.sy = pos[1]; v.sz = pos[2];
		// through mode: z is unsigned (u8/u16), only x/y are signed
		if (f.pos == 2) { u16 z; memcpy(&z, p + f.oP + 4, 2); v.sz = z; }
		else if (f.pos == 1) v.sz = p[f.oP + 2];
		v.w = 1;
		v.u = tc[0]; v.v = tc[1];
		v.r = col[0]; v.g = col[1]; v.b = col[2]; v.a = col[3];
		v.fog = 1;
		return v;
	}
	// skinning
	if (f.oW >= 0) {
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
		memcpy(pos, sp, 12);
		memcpy(nrm, sn, 12);
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
	v.sx = f24(R[0x45]) + f24(R[0x42]) * v.x * iw - ox;
	v.sy = f24(R[0x46]) + f24(R[0x43]) * v.y * iw - oy;
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
};
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

static void decode_dxt(const TexInfo &t, std::vector<u32> &out) {
	int bw = std::max<u32>(t.bufw, 4) / 4;
	int blockBytes = t.fmt == 8 ? 8 : 16;
	for (u32 by = 0; by < t.h / 4 + (t.h < 4); by++)
		for (u32 bx = 0; bx < t.w / 4 + (t.w < 4); bx++) {
			const u8 *b = gptr(t.addr + (by * bw + bx) * blockBytes);
			const u8 *cb = t.fmt == 8 ? b : b + 8;  // DXT3/5: color block after alpha
			u16 c1, c2;
			memcpy(&c1, cb + 4, 2);
			memcpy(&c2, cb + 6, 2);
			int r1 = (c1 >> 8) & 0xF8, g1 = (c1 >> 3) & 0xFC, b1 = (c1 << 3) & 0xF8;
			int r2 = (c2 >> 8) & 0xF8, g2 = (c2 >> 3) & 0xFC, b2 = (c2 << 3) & 0xF8;
			u32 a = t.fmt == 8 ? 0xFF000000u : 0;
			auto mk = [&](int r, int g, int bb) { return a | (bb << 16) | (g << 8) | r; };
			u32 cols[4] = {mk(r1, g1, b1), mk(r2, g2, b2), 0, 0};
			if (c1 > c2 || t.fmt != 8) {
				cols[2] = mk((2 * r1 + r2) / 3, (2 * g1 + g2) / 3, (2 * b1 + b2) / 3);
				cols[3] = mk((r1 + 2 * r2) / 3, (g1 + 2 * g2) / 3, (b1 + 2 * b2) / 3);
			} else {
				cols[2] = mk((r1 + r2) / 2, (g1 + g2) / 2, (b1 + b2) / 2);
				cols[3] = 0;
			}
			for (int y = 0; y < 4; y++) {
				u32 py = by * 4 + y;
				if (py >= t.h) break;
				u8 line = cb[y];
				for (int x = 0; x < 4; x++) {
					u32 pxx = bx * 4 + x;
					u32 c = cols[(line >> (2 * x)) & 3];
					if (t.fmt == 9) {  // DXT3: 4-bit explicit alpha, 8 bytes, row of 4 = u16
						u16 arow; memcpy(&arow, b + y * 2, 2);
						c = (c & 0xFFFFFF) | (((arow >> (4 * x)) & 15) * 17u << 24);
					} else if (t.fmt == 10) {  // DXT5: interpolated alpha
						u8 a0 = b[6], a1 = b[7];
						u64 bits = 0; memcpy(&bits, b, 6);
						int code = (bits >> (3 * (y * 4 + x))) & 7;
						int av;
						if (a0 > a1) av = code == 0 ? a0 : code == 1 ? a1 : ((8 - code) * a0 + (code - 1) * a1) / 7;
						else av = code == 0 ? a0 : code == 1 ? a1 : code == 6 ? 0 : code == 7 ? 255 : ((6 - code) * a0 + (code - 1) * a1) / 5;
						c = (c & 0xFFFFFF) | ((u32)av << 24);
					}
					if (pxx < t.w) out[py * t.w + pxx] = c;
				}
			}
		}
}

void ge_flush();
static bool jobs_pending();
static bool jobs_overlap(u32 addr, u32 size);
static bool running_overlap(u32 addr, u32 size);
extern u64 g_flush_why[8];
static const u32 *get_texture(const TexInfo &t) {
	// render-to-texture: draw pending pixels first if they target this texture's memory
	if ((t.addr & 0x0F000000) == 0x04000000 &&
	    (jobs_overlap(t.addr & 0x1FFFFFFF, 0x80000) || running_overlap(t.addr & 0x1FFFFFFF, 0x80000))) { g_flush_why[1]++; ge_flush(); }
	int bits = s_bpp_bits[t.fmt];
	bool isClut = t.fmt >= 4 && t.fmt <= 7;
	u32 rowBytes = t.bufw * bits / 8;
	size_t bytes = t.fmt >= 8 ? (size_t)std::max<u32>(t.bufw, 4) / 4 * ((t.h + 3) / 4) * (t.fmt == 8 ? 8 : 16)
	                          : (size_t)rowBytes * (t.swz ? ((t.h + 7) & ~7u) : t.h);
	u64 key = ((u64)t.addr << 32) ^ ((u64)t.fmt << 28) ^ ((u64)t.w << 16) ^ ((u64)t.h << 4) ^ ((u64)t.bufw << 40) ^ t.swz;
	u64 h = hash_bytes(gptr(t.addr), std::min<size_t>(bytes, 0x200000));
	DecodedTex &d = s_texcache[key];
	if (!d.px.empty() && d.hash == h && (!isClut || d.clutHash == s_clut_hash)) return d.px.data();
	if (jobs_pending()) g_flush_why[2]++;
	ge_flush();   // queued draws may still sample the old decode of this entry
	d.px.assign((size_t)t.w * t.h, 0);
	d.hash = h;
	d.clutHash = s_clut_hash;
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
	u8 *fb;
	u16 *zb;
	u32 stride, zstride, fmt;
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

static Target current_target() {
	Target t;
	u32 fbp = (R[0x9c] & 0xFFFFF0) | ((R[0x9d] & 0xFF0000) << 8);
	u32 zbp = (R[0x9e] & 0xFFFFF0) | ((R[0x9f] & 0xFF0000) << 8);
	t.fb = gptr(0x04000000 | (fbp & 0x1FFFFF));
	t.zb = (u16 *)gptr(0x04000000 | (zbp & 0x1FFFFF));
	t.stride = R[0x9d] & 0x7FC;
	t.zstride = R[0x9f] & 0x7FC;
	t.fmt = R[0xd2] & 3;
	if (!t.stride) t.stride = 512;
	if (!t.zstride) t.zstride = 512;
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
};
static Pipe P;

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
	p.sx1 = std::min<int>(p.sx1, p.tgt.stride - 1);
	p.sy1 = std::min(p.sy1, 511);
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
		p.texels = get_texture(t);
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
	p.zWrite = p.clear ? p.clearDepth : !(R[0xe7] & 1);
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
	p.minZ = R[0xd6] & 0xFFFF; p.maxZ = R[0xd7] & 0xFFFF;
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
static inline u32 sample_t(const Pipe &p, float s, float t) {
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
static inline void fb_write_t(const Target &t, int x, int y, u32 c, u32 mask) {
	if (!(F & F_GENERIC) && (F & F_FB32)) {
		u32 *q = (u32 *)(t.fb + (y * t.stride + x) * 4);
		*q = mask ? ((*q & mask) | (c & ~mask)) : c;
		return;
	}
	fb_write(t, x, y, c, mask);
}

static thread_local u64 t_pixels;
template <unsigned F>
static inline void shade_t(const Pipe &p, int x, int y, float r, float g, float b, float a, float z, float s, float t, float fog) {
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
	fb_write_t<F>(p.tgt, x, y, c, p.writeMask);
}

// ============================================================ rasterisation
// Each worker rasterises only the rows of its bands, so per-pixel order is kept.
static const int BAND_SHIFT = 3;
static inline bool my_row(int y, int tid, int n) { return ((y >> BAND_SHIFT) % n) == tid; }
// Does [y0, y1] contain any row of this worker's bands?
static inline bool my_rows(int y0, int y1, int tid, int n) {
	if (n == 1) return true;
	int b0 = y0 >> BAND_SHIFT, b1 = y1 >> BAND_SHIFT;
	if (b1 - b0 + 1 >= n) return true;
	for (int b = b0; b <= b1; b++) if (b % n == tid) return true;
	return false;
}

template <unsigned F>
static void raster_sprite_t(const Pipe &P, const Vtx &a, const Vtx &b, int tid, int nth) {
	float x0 = a.sx, y0 = a.sy, x1 = b.sx, y1 = b.sy;
	float u0 = a.u, v0 = a.v, u1 = b.u, v1 = b.v;
	if (x0 > x1) { std::swap(x0, x1); std::swap(u0, u1); }
	if (y0 > y1) { std::swap(y0, y1); std::swap(v0, v1); }
	int ix0 = std::max((int)ceilf(x0 - 0.5f), P.sx0), ix1 = std::min((int)ceilf(x1 - 0.5f) - 1, P.sx1);
	int iy0 = std::max((int)ceilf(y0 - 0.5f), P.sy0), iy1 = std::min((int)ceilf(y1 - 0.5f) - 1, P.sy1);
	if (iy0 > iy1 || !my_rows(iy0, iy1, tid, nth)) return;
	float du = x1 > x0 ? (u1 - u0) / (x1 - x0) : 0, dv = y1 > y0 ? (v1 - v0) / (y1 - y0) : 0;
	float sw = P.through ? 1.0f / P.tw : 1.0f, shh = P.through ? 1.0f / P.th : 1.0f;
	if (!P.tex) sw = shh = 0;
	for (int y = iy0; y <= iy1; y++) {
		if (!my_row(y, tid, nth)) continue;
		float vv = v0 + (y + 0.5f - y0) * dv;
		for (int x = ix0; x <= ix1; x++) {
			float uu = u0 + (x + 0.5f - x0) * du;
			shade_t<F>(P, x, y, b.r, b.g, b.b, b.a, b.sz, uu * sw, vv * shh, b.fog);
		}
	}
}

template <unsigned F>
static void raster_triangle_t(const Pipe &P, const Vtx &v0, const Vtx &v1, const Vtx &v2, const Vtx &flat, int tid, int nth) {
	{
		int qy0 = std::max((int)floorf(std::min({v0.sy, v1.sy, v2.sy})), P.sy0);
		int qy1 = std::min((int)ceilf(std::max({v0.sy, v1.sy, v2.sy})), P.sy1);
		if (qy0 > qy1 || !my_rows(qy0, qy1, tid, nth)) return;
	}
	float area = (v1.sx - v0.sx) * (v2.sy - v0.sy) - (v2.sx - v0.sx) * (v1.sy - v0.sy);
	if (area == 0) return;
	int minx = std::max((int)floorf(std::min({v0.sx, v1.sx, v2.sx})), P.sx0);
	int maxx = std::min((int)ceilf(std::max({v0.sx, v1.sx, v2.sx})), P.sx1);
	int miny = std::max((int)floorf(std::min({v0.sy, v1.sy, v2.sy})), P.sy0);
	int maxy = std::min((int)ceilf(std::max({v0.sy, v1.sy, v2.sy})), P.sy1);
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
		if (!my_row(y, tid, nth)) continue;
		float py = y + 0.5f, px = minx + 0.5f;
		float e0 = ((v2.sx - v1.sx) * (py - v1.sy) - (v2.sy - v1.sy) * (px - v1.sx)) * ia;
		float e1 = ((v0.sx - v2.sx) * (py - v2.sy) - (v0.sy - v2.sy) * (px - v2.sx)) * ia;
		for (int x = minx; x <= maxx; x++, e0 += d0, e1 += d1) {
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

static inline void raster_triangle(const Pipe &p, const Vtx &a, const Vtx &b, const Vtx &c, const Vtx &f, int tid, int nth) {
	if (p.spec == F_GENERIC) raster_triangle_t<F_GENERIC>(p, a, b, c, f, tid, nth);
	else s_triFns[p.spec](p, a, b, c, f, tid, nth);
}
static inline void raster_sprite(const Pipe &p, const Vtx &a, const Vtx &b, int tid, int nth) {
	if (p.spec == F_GENERIC) raster_sprite_t<F_GENERIC>(p, a, b, tid, nth);
	else s_sprFns[p.spec](p, a, b, tid, nth);
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
	u32 fb = (u32)(p.tgt.fb - g_mem), zb = (u32)((u8 *)p.tgt.zb - g_mem);
	u32 rows = (u32)p.sy1 + 1;
	Range a{fb, fb + p.tgt.stride * rows * (p.tgt.fmt == 3 ? 4 : 2)}, b{zb, zb + p.tgt.zstride * rows * 2};
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

static void run_jobs(int tid) {
	for (const Job &j : s_run_jobs) {
		const Pipe &p = s_run_pipes[j.pipe];
		if (j.kind == 0) raster_triangle(p, j.v[0], j.v[1], j.v[2], j.v[3], tid, s_nthreads);
		else raster_sprite(p, j.v[0], j.v[1], tid, s_nthreads);
	}
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

// Hand the current batch to the raster workers and return (they run while we build the next).
void ge_submit() {
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
	extern u64 g_prof_flush;
	u64 t0 = rt_time_us();
	ge_submit();
	ge_wait_running();
	g_prof_flush += rt_time_us() - t0;
}

static void emit_triangle(const Vtx &a, const Vtx &b, const Vtx &c, const Vtx &flat) {
	if (s_pipe_cur < 0) { s_pipes.push_back(P); s_pipe_cur = (int)s_pipes.size() - 1; note_target(P); }
	Job j;
	j.pipe = s_pipe_cur;
	j.kind = 0;
	j.v[0] = a; j.v[1] = b; j.v[2] = c; j.v[3] = flat;
	s_jobs.push_back(j);
	if (s_jobs.size() >= 4096) ge_submit();
}

static void draw_sprite(const Vtx &a, const Vtx &b) {
	if (s_pipe_cur < 0) { s_pipes.push_back(P); s_pipe_cur = (int)s_pipes.size() - 1; note_target(P); }
	Job j;
	j.pipe = s_pipe_cur;
	j.kind = 1;
	j.v[0] = a; j.v[1] = b;
	s_jobs.push_back(j);
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
	if (traceAt >= 0 && s_traceState == 0 && rt_time_us() / 1e6 >= traceAt) s_traceState = 1;
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
			Vtx b = a; b.sx += 1; b.sy += 1;
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
		rt_log(LOG_GE, "T prim %d n %3d vt %06x thr %d tex %d fmt %u %ux%u zt %d/%d zw %d blend %d at %d cull %d tmap %u px %llu | v0 clip(%.1f %.1f %.1f %.1f) scr(%.0f %.0f %.0f) a %.0f",
		       type, count, vt, f.through, P.tex, R[0xc3] & 15, P.tw, P.th, P.depthTest, P.zFunc, P.zWrite, P.blend, P.alphaTest, cullOn ? 1 + cullCCW : 0,
		       R[0xc0] & 3, (unsigned long long)(t_pixels - px0), a.x, a.y, a.z, a.w, a.sx, a.sy, a.sz, a.a);
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
	for (u32 y = 0; y < h; y++)
		memmove(gptr(dst + ((dy + y) * dw + dx) * bpp), gptr(src + ((sy + y) * sw + sx) * bpp), w * bpp);
}

// ============================================================ command entry
void ge_exec_cmd(u32 cmd, u32 op, u32 pc) {
	(void)pc;
	u32 data = op & 0xFFFFFF;
	R[cmd] = data;
	switch (cmd) {
	case 0x01: s_vaddr = ((g_ge_base & 0x0F000000) | data) + g_ge_offset; break;
	case 0x02: s_iaddr = ((g_ge_base & 0x0F000000) | data) + g_ge_offset; break;
	case 0x04: { extern u64 g_prof_ge; u64 t0 = rt_time_us(); draw_prim(data); g_prof_ge += rt_time_us() - t0; break; }
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
