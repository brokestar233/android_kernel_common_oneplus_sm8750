// SPDX-License-Identifier: GPL-2.0
/*
 * cambyses_simd_avx2.c — AVX2 SIMD routines for Cambyses
 *
 * 1. u16 argmax via PHMINPOSUW (SSE4.1, always available under -mavx2)
 *    Scores are u16 pul16 values; XOR 0xFFFF converts max→min for PHMINPOSUW.
 *
 * 2. u64 → pul16 batch conversion via IEEE 754 double trick
 *    AVX-512DQ path: _mm512_cvtepu64_pd (8 × u64 → 8 × u16)
 *    AVX2 fallback:  manual u64→double via magic number trick (4 × u64 → 4 × u16)
 *
 * Must be called within cambyses_simd_begin/end.
 * Compiled with: CFLAGS += -mavx -mavx2
 *
 * Separate TU to prevent auto-vectorization contamination of scalar code.
 */

#include "cambyses.h"
#include <linux/types.h>

#ifdef CONFIG_SCHED_CAMBYSES_SIMD

/*
 * XOR mask: u16 max → u16 min conversion for PHMINPOSUW.
 *   high pul16 score → low u16 after XOR → selected by PHMINPOSUW
 *   tombstone 0 → 0xFFFF → never selected
 */
static const v8hi phminpos_xor = {
	(short)0xFFFF, (short)0xFFFF, (short)0xFFFF, (short)0xFFFF,
	(short)0xFFFF, (short)0xFFFF, (short)0xFFFF, (short)0xFFFF
};

/*
 * argmax of a single XMM (8 × u16) via PHMINPOSUW.
 * Returns lane index (0–7) of the maximum u16 value.
 */
static __always_inline int argmax_xmm(v8hi s)
{
	v8hi r = (v8hi)__builtin_ia32_phminposuw128(s ^ phminpos_xor);

	return (unsigned short)r[1];
}

/* 8 entries — 1 PHMINPOSUW (direct result) */
int cambyses_simd_argmax_avx2_8(const u16 *scores)
{
	return argmax_xmm(*(const v8hi_u *)scores);
}

/* 16 entries — 2 PHMINPOSUW + 1 scalar comparison */
int cambyses_simd_argmax_avx2_16(const u16 *scores)
{
	v8hi r0, r1;
	u16 v0, v1;

	r0 = (v8hi)__builtin_ia32_phminposuw128(
		*(const v8hi_u *)&scores[0] ^ phminpos_xor);
	r1 = (v8hi)__builtin_ia32_phminposuw128(
		*(const v8hi_u *)&scores[8] ^ phminpos_xor);

	v0 = (u16)r0[0]; v1 = (u16)r1[0];

	if (v0 <= v1)
		return (u16)r0[1];
	return 8 + (u16)r1[1];
}

/* 32 entries — 4 PHMINPOSUW + 3 scalar comparisons */
int cambyses_simd_argmax_avx2_32(const u16 *scores)
{
	v8hi r0, r1, r2, r3;
	u16 v0, v1, v2, v3;

	r0 = (v8hi)__builtin_ia32_phminposuw128(
		*(const v8hi_u *)&scores[0]  ^ phminpos_xor);
	r1 = (v8hi)__builtin_ia32_phminposuw128(
		*(const v8hi_u *)&scores[8]  ^ phminpos_xor);
	r2 = (v8hi)__builtin_ia32_phminposuw128(
		*(const v8hi_u *)&scores[16] ^ phminpos_xor);
	r3 = (v8hi)__builtin_ia32_phminposuw128(
		*(const v8hi_u *)&scores[24] ^ phminpos_xor);

	v0 = (u16)r0[0]; v1 = (u16)r1[0];
	v2 = (u16)r2[0]; v3 = (u16)r3[0];

	if (v0 <= v1 && v0 <= v2 && v0 <= v3)
		return (u16)r0[1];
	if (v1 <= v2 && v1 <= v3)
		return 8 + (u16)r1[1];
	if (v2 <= v3)
		return 16 + (u16)r2[1];
	return 24 + (u16)r3[1];
}

/*
 * u64 → pul16 batch conversion via IEEE 754 double trick (AVX2).
 *
 * u64→double: uses magic number trick (works for full u64 range by
 * splitting into high 32 and low 32 bits).
 * Then extract exponent and mantissa bits from the double representation.
 *
 * pul16 = ((exponent - 1023) << 10) | mantissa_top_10_bits
 */

/* GCC vector types for 256-bit double/integer */
typedef double v4df __attribute__((__vector_size__(32)));
typedef long long v4di __attribute__((__vector_size__(32)));
typedef long long v4di_u __attribute__((__vector_size__(32), __aligned__(8)));
typedef int v8si __attribute__((__vector_size__(32)));

/*
 * cvt_u64_pd_avx2 — convert 4 × u64 to 4 × double using AVX2.
 * Handles full u64 range by splitting into hi32 and lo32 parts.
 */
static __always_inline v4df cvt_u64_pd_avx2(v4di v)
{
	/*
	 * Magic number trick:
	 *   lo32: blend low 32 bits into 2^52 exponent → subtract 2^52
	 *   hi32: blend high 32 bits into 2^84 exponent → subtract 2^84
	 *   result = hi + lo
	 */
	v4di hi32 = (v4di)__builtin_ia32_psrlqi256((v4di)v, 32);

	/* 2^52 as integer bits = 0x4330000000000000 */
	v4di magic_lo = (v4di)(v4df){0x1.0p52, 0x1.0p52, 0x1.0p52, 0x1.0p52};
	/* 2^84 as integer bits = 0x4530000000000000 */
	v4di magic_hi = (v4di)(v4df){0x1.0p84, 0x1.0p84, 0x1.0p84, 0x1.0p84};

	/*
	 * blend_epi32 mask 0x55 = 01010101b: take even 32-bit lanes from src1,
	 * odd 32-bit lanes from src2.  This puts our lo32/hi32 values into the
	 * mantissa position of the magic double.
	 */
	v4di lo_blend = (v4di)__builtin_ia32_pblendd256(
		(v8si)magic_lo, (v8si)v, 0x55);
	v4di hi_blend = (v4di)__builtin_ia32_pblendd256(
		(v8si)magic_hi, (v8si)hi32, 0x55);

	v4df lo = (v4df)lo_blend - (v4df)magic_lo;
	v4df hi = (v4df)hi_blend - (v4df)magic_hi;

	return hi + lo;
}

void cambyses_simd_u64_to_pul16_avx2(const u64 *raw, u16 *out, int nr)
{
	int i;

	for (i = 0; i + 3 < nr; i += 4) {
		v4di v = *(const v4di_u *)&raw[i];
		v4df d = cvt_u64_pd_avx2(v);
		v4di bits = (v4di)d;

		/* Extract exponent: (bits >> 52) & 0x7FF */
		v4di exp = __builtin_ia32_psrlqi256(bits, 52) &
			   (v4di){0x7FF, 0x7FF, 0x7FF, 0x7FF};

		/* Extract mantissa top 10 bits: (bits >> 42) & 0x3FF */
		v4di mant = __builtin_ia32_psrlqi256(bits, 42) &
			    (v4di){0x3FF, 0x3FF, 0x3FF, 0x3FF};

		/* pul16 = ((exp - 1023) << 10) | mant */
		v4di bias = (v4di){1023, 1023, 1023, 1023};
		v4di pul64 = __builtin_ia32_psllqi256(exp - bias, 10) | mant;

		/* Pack 4 × u64 → 4 × u16 (manual extraction) */
		out[i]     = (u16)pul64[0];
		out[i + 1] = (u16)pul64[1];
		out[i + 2] = (u16)pul64[2];
		out[i + 3] = (u16)pul64[3];
	}

	/* Scalar fallback for remainder */
	for (; i < nr; i++) {
		u64 v = raw[i];
		u8 clz;
		u16 m;

		if (v <= 1) {
			out[i] = (u16)!v;
			continue;
		}
		clz = __builtin_clzll(v);
		m = (u16)((v << clz) >> (64 - 1 - 10));
		out[i] = ((u16)(62 - clz) << 10) + m;
	}
}

#endif /* CONFIG_SCHED_CAMBYSES_SIMD */
