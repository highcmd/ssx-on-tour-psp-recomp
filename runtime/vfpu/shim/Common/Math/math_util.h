#pragma once
#include <cmath>
#include <cstring>
#include "Common/CommonTypes.h"
template <class T> inline T clamp_value(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
union FP32 {
	uint32_t u;
	float f;
};

struct FP16 {
	uint16_t u;
};

inline bool my_isinf(float f) {
	FP32 f2u;
	f2u.f = f;
	return f2u.u == 0x7f800000 ||
		f2u.u == 0xff800000;
}

inline bool my_isinf_u(uint32_t u) {
	return u == 0x7f800000 || u == 0xff800000;
}

inline bool my_isnan(float f) {
	FP32 f2u;
	f2u.f = f;
	// NaNs have non-zero mantissa
	return ((f2u.u & 0x7F800000) == 0x7F800000) && (f2u.u & 0x7FFFFF);
}

inline bool my_isnanorinf(float f) {
	FP32 f2u;
	f2u.f = f;
	// NaNs have non-zero mantissa, infs have zero mantissa. That is, we just ignore the mantissa here.
	return ((f2u.u & 0x7F800000) == 0x7F800000);
}

inline float InfToZero(float f) {
	return my_isinf(f) ? 0.0f : f;
}

inline int is_even(float d) {
	float int_part;
	modff(d / 2.0f, &int_part);
	return 2.0f * int_part == d;
}

// Rounds *.5 to closest even number
inline double round_ieee_754(double d) {
	float i = (float)floor(d);
	d -= i;
	if (d < 0.5f)
		return i;
	if (d > 0.5f)
		return i + 1.0f;
	if (is_even(i))
		return i;
	return i + 1.0f;
}

// magic code from ryg: http://fgiesen.wordpress.com/2012/03/28/half-to-float-done-quic/
// See also SSE2 version: https://gist.github.com/rygorous/2144712
inline FP32 half_to_float_fast5(FP16 h)
{
	static const FP32 magic = { (127 + (127 - 15)) << 23 };
	static const FP32 was_infnan = { (127 + 16) << 23 };
	FP32 o;
	o.u = (h.u & 0x7fff) << 13;     // exponent/mantissa bits
	o.f *= magic.f;                 // exponent adjust
	if (o.f >= was_infnan.f)        // make sure Inf/NaN survive (retain the low bits)
		o.u = (255 << 23) | (h.u & 0x03ff);
	o.u |= (h.u & 0x8000) << 16;    // sign bit
	return o;
}

inline float ExpandHalf(uint16_t half) {
	FP16 fp16;
	fp16.u = half;
	FP32 fp = half_to_float_fast5(fp16);
	return fp.f;
}

// More magic code: https://gist.github.com/rygorous/2156668
inline FP16 float_to_half_fast3(FP32 f)
{
	static const FP32 f32infty = { 255 << 23 };
	static const FP32 f16infty = { 31 << 23 };
	static const FP32 magic = { 15 << 23 };
	static const uint32_t sign_mask = 0x80000000u;
	static const uint32_t round_mask = ~0xfffu;
	FP16 o = { 0 };

	uint32_t sign = f.u & sign_mask;
	f.u ^= sign;

	if (f.u >= f32infty.u) // Inf or NaN (all exponent bits set)
		o.u = (f.u > f32infty.u) ? (0x7e00 | (f.u & 0x3ff)) : 0x7c00; // NaN->qNaN and Inf->Inf
	else // (De)normalized number or zero
	{
		f.u &= round_mask;
		f.f *= magic.f;
		f.u -= round_mask;
		if (f.u > f16infty.u) f.u = f16infty.u; // Clamp to signed infinity if overflowed

		o.u = f.u >> 13; // Take the bits!
	}

	o.u |= sign >> 16;
	return o;
}

inline uint16_t ShrinkToHalf(float full) {
	FP32 fp32; fp32.f = full; FP16 fp = float_to_half_fast3(fp32); return fp.u; }
