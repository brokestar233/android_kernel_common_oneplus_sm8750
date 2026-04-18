// SPDX-License-Identifier: GPL-2.0
/*
 * cambyses_simd_neon.c — NEON SIMD routines for Cambyses
 *
 * 1. u16 argmax using UMAXV horizontal max + CMEQ + bitmask extraction.
 *
 * 2. u64 → pul16 batch conversion via vcvtq_f64_u64 (ARMv8.2-A+)
 *    or scalar fallback.
 *
 * Must be called within cambyses_simd_begin/end.
 * Compiled with: CFLAGS_REMOVE += -mgeneral-regs-only
 *                CFLAGS += $(CC_FLAGS_FPU)
 *
 * Separate TU to prevent auto-vectorization contamination of scalar code.
 */

#include "cambyses.h"
#include <linux/types.h>

#ifdef CONFIG_SCHED_CAMBYSES_SIMD

#include <asm/neon-intrinsics.h>

static const uint16x8_t pos_weights = {1, 2, 4, 8, 16, 32, 64, 128};

/* 8 entries — 1 Q register load, direct UMAXV */
int cambyses_simd_argmax_neon_8(const u16 *scores)
{
	uint16x8_t s0;
	uint16_t max_val;
	uint16x8_t bcast, c0;
	uint16_t bits0;

	s0 = vld1q_u16(&scores[0]);

	max_val = vmaxvq_u16(s0);
	bcast = vdupq_n_u16(max_val);

	c0 = vceqq_u16(s0, bcast);
	bits0 = vaddvq_u16(vandq_u16(c0, pos_weights));

	return __builtin_ctz(bits0);
}

/* 16 entries — 2 Q register loads, 1-stage reduction */
int cambyses_simd_argmax_neon_16(const u16 *scores)
{
	uint16x8_t s0, s1, m;
	uint16_t max_val;
	uint16x8_t bcast, c0, c1;
	uint16_t bits0, bits1;
	uint32_t combined;

	s0 = vld1q_u16(&scores[0]);
	s1 = vld1q_u16(&scores[8]);

	m = vmaxq_u16(s0, s1);

	max_val = vmaxvq_u16(m);
	bcast = vdupq_n_u16(max_val);

	c0 = vceqq_u16(s0, bcast);
	c1 = vceqq_u16(s1, bcast);

	bits0 = vaddvq_u16(vandq_u16(c0, pos_weights));
	bits1 = vaddvq_u16(vandq_u16(c1, pos_weights));

	combined = (uint32_t)bits0 | ((uint32_t)bits1 << 8);

	return __builtin_ctz(combined);
}

/* 32 entries — 4 Q register loads, 3-stage vmaxq reduction */
int cambyses_simd_argmax_neon_32(const u16 *scores)
{
	uint16x8_t s0, s1, s2, s3;
	uint16x8_t m01, m23, m;
	uint16_t max_val;
	uint16x8_t bcast, c0, c1, c2, c3;
	uint16_t bits0, bits1, bits2, bits3;
	uint32_t combined;

	s0 = vld1q_u16(&scores[0]);
	s1 = vld1q_u16(&scores[8]);
	s2 = vld1q_u16(&scores[16]);
	s3 = vld1q_u16(&scores[24]);

	m01 = vmaxq_u16(s0, s1);
	m23 = vmaxq_u16(s2, s3);
	m   = vmaxq_u16(m01, m23);

	max_val = vmaxvq_u16(m);
	bcast = vdupq_n_u16(max_val);

	c0 = vceqq_u16(s0, bcast);
	c1 = vceqq_u16(s1, bcast);
	c2 = vceqq_u16(s2, bcast);
	c3 = vceqq_u16(s3, bcast);

	bits0 = vaddvq_u16(vandq_u16(c0, pos_weights));
	bits1 = vaddvq_u16(vandq_u16(c1, pos_weights));
	bits2 = vaddvq_u16(vandq_u16(c2, pos_weights));
	bits3 = vaddvq_u16(vandq_u16(c3, pos_weights));

	combined = (uint32_t)bits0 | ((uint32_t)bits1 << 8)
		 | ((uint32_t)bits2 << 16) | ((uint32_t)bits3 << 24);

	return __builtin_ctz(combined);
}

/*
 * u64 → pul16 batch conversion (NEON scalar fallback).
 *
 * vcvtq_f64_u64 requires ARMv8.2-A+ which is not universally available.
 * Use scalar CLZ-based conversion for maximum compatibility.
 * The NEON argmax path above provides the main SIMD benefit.
 */
void cambyses_simd_u64_to_pul16_neon(const u64 *raw, u16 *out, int nr)
{
	int i;

	for (i = 0; i < nr; i++) {
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
