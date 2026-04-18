// SPDX-License-Identifier: GPL-2.0
/*
 * cambyses_simd_ssse3.c — SSSE3 SIMD routines for Cambyses
 *
 * 1. u16 argmax using either:
 *    - PHMINPOSUW (SSE4.1, runtime-gated via static branch) — fast path
 *    - PMAXUW reduction + PCMPEQW + PMOVMSKB + BSF — fallback
 *
 * 2. u64 → pul16 batch conversion via IEEE 754 double trick (SSE2)
 *
 * Must be called within cambyses_simd_begin/end.
 * Compiled with: CFLAGS += -mssse3
 *
 * Separate TU to prevent auto-vectorization contamination of scalar code.
 */

#include "cambyses.h"
#include <linux/types.h>

#ifdef CONFIG_SCHED_CAMBYSES_SIMD

/*
 * PHMINPOSUW via inline asm — the instruction is SSE4.1 but this TU
 * is compiled with -mssse3, so the builtin is not available.
 */
static __always_inline v8hi __phminposuw128(v8hi a)
{
	v8hi result;

	asm("phminposuw %1, %0" : "=x" (result) : "xm" (a));
	return result;
}

static __always_inline int __pmovmskb128(v16qi a)
{
	int result;

	asm("pmovmskb %1, %0" : "=r" (result) : "x" (a));
	return result;
}

/*
 * PMAXUW via inline asm — SSE4.1 instruction, not available with -mssse3.
 * Used in fallback path for unsigned u16 max reduction.
 * On pure SSSE3 (no SSE4.1), we use a scalar horizontal max instead.
 */

/*
 * XOR mask: u16 max → u16 min conversion for PHMINPOSUW.
 */
static const v8hi phminpos_xor = {
	(short)0xFFFF, (short)0xFFFF, (short)0xFFFF, (short)0xFFFF,
	(short)0xFFFF, (short)0xFFFF, (short)0xFFFF, (short)0xFFFF
};

/* ---- 8-entry variant ---- */

int cambyses_simd_argmax_ssse3_8(const u16 *scores)
{
	v8hi s0 = *(const v8hi_u *)&scores[0];

	if (static_branch_likely(&cambyses_has_sse41)) {
		v8hi r = __phminposuw128(s0 ^ phminpos_xor);

		return (u16)r[1];
	} else {
		u16 max_val;
		v8hi bcast, c0;
		int j, mask0;

		max_val = (u16)s0[0];
		for (j = 1; j < 8; j++)
			if ((u16)s0[j] > max_val)
				max_val = (u16)s0[j];

		bcast = (v8hi){(short)max_val, (short)max_val,
			       (short)max_val, (short)max_val,
			       (short)max_val, (short)max_val,
			       (short)max_val, (short)max_val};

		c0 = (s0 == bcast);
		mask0 = __pmovmskb128((v16qi)c0);

		return __builtin_ctz(mask0) >> 1;
	}
}

/* ---- 16-entry variant ---- */

int cambyses_simd_argmax_ssse3_16(const u16 *scores)
{
	v8hi s0 = *(const v8hi_u *)&scores[0];
	v8hi s1 = *(const v8hi_u *)&scores[8];

	if (static_branch_likely(&cambyses_has_sse41)) {
		v8hi r0, r1;
		u16 v0, v1;

		r0 = __phminposuw128(s0 ^ phminpos_xor);
		r1 = __phminposuw128(s1 ^ phminpos_xor);

		v0 = (u16)r0[0]; v1 = (u16)r1[0];

		if (v0 <= v1)
			return (u16)r0[1];
		return 8 + (u16)r1[1];
	} else {
		u16 max_val;
		v8hi bcast, c0, c1;
		int j, mask0, mask1;

		/* Horizontal max across both vectors (scalar) */
		max_val = (u16)s0[0];
		for (j = 1; j < 8; j++)
			if ((u16)s0[j] > max_val)
				max_val = (u16)s0[j];
		for (j = 0; j < 8; j++)
			if ((u16)s1[j] > max_val)
				max_val = (u16)s1[j];

		bcast = (v8hi){(short)max_val, (short)max_val,
			       (short)max_val, (short)max_val,
			       (short)max_val, (short)max_val,
			       (short)max_val, (short)max_val};

		c0 = (s0 == bcast);
		c1 = (s1 == bcast);
		mask0 = __pmovmskb128((v16qi)c0);
		mask1 = __pmovmskb128((v16qi)c1);

		if (mask0)
			return __builtin_ctz(mask0) >> 1;
		return 8 + (__builtin_ctz(mask1) >> 1);
	}
}

/* ---- 32-entry variant ---- */

int cambyses_simd_argmax_ssse3_32(const u16 *scores)
{
	v8hi s0 = *(const v8hi_u *)&scores[0];
	v8hi s1 = *(const v8hi_u *)&scores[8];
	v8hi s2 = *(const v8hi_u *)&scores[16];
	v8hi s3 = *(const v8hi_u *)&scores[24];

	if (static_branch_likely(&cambyses_has_sse41)) {
		v8hi r0, r1, r2, r3;
		u16 v0, v1, v2, v3;

		r0 = __phminposuw128(s0 ^ phminpos_xor);
		r1 = __phminposuw128(s1 ^ phminpos_xor);
		r2 = __phminposuw128(s2 ^ phminpos_xor);
		r3 = __phminposuw128(s3 ^ phminpos_xor);

		v0 = (u16)r0[0]; v1 = (u16)r1[0];
		v2 = (u16)r2[0]; v3 = (u16)r3[0];

		if (v0 <= v1 && v0 <= v2 && v0 <= v3)
			return (u16)r0[1];
		if (v1 <= v2 && v1 <= v3)
			return 8 + (u16)r1[1];
		if (v2 <= v3)
			return 16 + (u16)r2[1];
		return 24 + (u16)r3[1];
	} else {
		u16 max_val;
		v8hi bcast, c0, c1, c2, c3;
		int j, mask0, mask1, mask2, mask3;

		max_val = (u16)s0[0];
		for (j = 1; j < 8; j++)
			if ((u16)s0[j] > max_val)
				max_val = (u16)s0[j];
		for (j = 0; j < 8; j++) {
			if ((u16)s1[j] > max_val)
				max_val = (u16)s1[j];
			if ((u16)s2[j] > max_val)
				max_val = (u16)s2[j];
			if ((u16)s3[j] > max_val)
				max_val = (u16)s3[j];
		}

		bcast = (v8hi){(short)max_val, (short)max_val,
			       (short)max_val, (short)max_val,
			       (short)max_val, (short)max_val,
			       (short)max_val, (short)max_val};

		c0 = (s0 == bcast);
		c1 = (s1 == bcast);
		c2 = (s2 == bcast);
		c3 = (s3 == bcast);
		mask0 = __pmovmskb128((v16qi)c0);
		mask1 = __pmovmskb128((v16qi)c1);
		mask2 = __pmovmskb128((v16qi)c2);
		mask3 = __pmovmskb128((v16qi)c3);

		if (mask0)
			return __builtin_ctz(mask0) >> 1;
		if (mask1)
			return 8 + (__builtin_ctz(mask1) >> 1);
		if (mask2)
			return 16 + (__builtin_ctz(mask2) >> 1);
		return 24 + (__builtin_ctz(mask3) >> 1);
	}
}

/*
 * u64 → pul16 batch conversion via IEEE 754 double trick (SSE2).
 *
 * Processes 2 × u64 at a time (XMM register = 2 × double).
 */

/* GCC vector types for 128-bit double/integer */
typedef double v2df __attribute__((__vector_size__(16)));
typedef long long v2di __attribute__((__vector_size__(16)));
typedef long long v2di_u __attribute__((__vector_size__(16), __aligned__(8)));
typedef int v4si __attribute__((__vector_size__(16)));

static __always_inline v2df cvt_u64_pd_sse2(v2di v)
{
	/* Split into hi32 and lo32 */
	v2di hi32 = (v2di)__builtin_ia32_psrlqi128((v2di)v, 32);

	v2di magic_lo = (v2di)(v2df){0x1.0p52, 0x1.0p52};
	v2di magic_hi = (v2di)(v2df){0x1.0p84, 0x1.0p84};

	/*
	 * SSE2 doesn't have pblendd; use shufps as a 32-bit blend.
	 * shufps with imm 0x88 picks: dst[0]=a[0], dst[1]=b[0], dst[2]=a[2], dst[3]=b[2]
	 * But we need a simpler approach: use AND/OR masking.
	 */
	v2di lo_mask = (v2di){0x00000000FFFFFFFFULL, 0x00000000FFFFFFFFULL};
	v2di lo_blend = (v & lo_mask) | (magic_lo & ~lo_mask);
	v2di hi_blend = (hi32 & lo_mask) | (magic_hi & ~lo_mask);

	v2df lo = (v2df)lo_blend - (v2df)magic_lo;
	v2df hi = (v2df)hi_blend - (v2df)magic_hi;

	return hi + lo;
}

void cambyses_simd_u64_to_pul16_ssse3(const u64 *raw, u16 *out, int nr)
{
	int i;

	for (i = 0; i + 1 < nr; i += 2) {
		v2di v = *(const v2di_u *)&raw[i];
		v2df d = cvt_u64_pd_sse2(v);
		v2di bits = (v2di)d;

		/* Extract exponent: (bits >> 52) & 0x7FF */
		v2di exp = __builtin_ia32_psrlqi128(bits, 52) &
			   (v2di){0x7FF, 0x7FF};

		/* Extract mantissa top 10 bits: (bits >> 42) & 0x3FF */
		v2di mant = __builtin_ia32_psrlqi128(bits, 42) &
			    (v2di){0x3FF, 0x3FF};

		/* pul16 = ((exp - 1023) << 10) | mant */
		v2di bias = (v2di){1023, 1023};
		v2di pul64 = __builtin_ia32_psllqi128(exp - bias, 10) | mant;

		out[i]     = (u16)pul64[0];
		out[i + 1] = (u16)pul64[1];
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
