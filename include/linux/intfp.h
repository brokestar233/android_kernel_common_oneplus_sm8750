#ifndef _INTFP_H
#define _INTFP_H
/*
 * Integer-based Fixed-Point and Pseudo-Logarithmic Number Library (intfp)
 * Version: 1.4
 * Copyright (C) 2025 Masahito Suzuki
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 *
 * @file intfp.h
 * @brief A C99 header-only library for high-performance fixed-point and
 *        pseudo-logarithmic arithmetic, optimized for embedded systems.
 *
 * @details
 * This library provides a powerful set of tools for numerical computation on
 * resource-constrained platforms, such as microcontrollers without a Floating-
 * Point Unit (FPU) or systems where memory and storage are at a premium.
 *
 * It offers three specialized numerical formats:
 *
 * 1.  **Linear Fixed-Point:** A standard integer-based representation for
 *     fractional numbers, serving as the foundation for other formats.
 *
 * 2.  **Packed Unsigned Log (pul):** A memory-efficient, unsigned,
 *     pseudo-logarithmic format. It excels at compressing large numerical
 *     values into a smaller bit-width for storage or transmission. Note that
 *     this format is not directly suited for arithmetic operations.
 *
 * 3.  **Signed Pseudo-Logarithmic (log):** A calculable, signed, pseudo-
 *     logarithmic format. Its key advantage is transforming expensive
 *     multiplication and division operations into highly efficient addition
 *     and subtraction. This is ideal for applications like digital signal
 *     processing (DSP) and control systems.
 *
 * --- Key Features ---
 * - **High-Speed Computation:** Utilizes compiler intrinsics (`__builtin_clz`)
 *   for extremely fast conversion to a pseudo-logarithmic representation,
 *   avoiding costly standard math library calls.
 * - **Memory Efficiency:** The `pul` format significantly reduces the data
 *   footprint of large integer datasets.
 * - **High Flexibility:** Employs extensive preprocessor macros to generate
 *   conversion functions for a wide range of bit-width combinations (e.g.,
 *   u64 to pul16, log8 to log32), allowing fine-tuning for specific needs.
 * - **Practical Utilities:** Includes ready-to-use functions for Exponentially
 *   Weighted Moving Average (EWMA) and radix conversion (e.g., to decibels).
 *
 * @warning
 * The `log` format is a high-speed approximation. It represents a value `v` as
 * `e + m`, where `e = floor(log2(v))` and `m` is a linear fraction, not the
 * true `e + log2(1 + m)`. This introduces a predictable, non-linear error.
 * This trade-off is fundamental to its performance.
 *
 * @note
 * This library relies on GCC/Clang-specific built-in functions like
 * `__builtin_clz` and `__builtin_clzll`. Porting to other compilers may
 * require providing equivalent implementations.
 */

/**
 * @brief Calculates the number of bits in a 32-bit value (Find Last Set).
 * @param v The 32-bit unsigned integer.
 * @return The position of the most significant bit + 1.
 */
#define intfp_fls32(v) (32U - __builtin_clz((u32)v))

/**
 * @brief Internal helper to calculate floor(log2(v)).
 * @param v The value.
 * @param bits The bit-width of the value (32 or 64).
 * @return The integer part of the base-2 logarithm of v.
 */
#define __intfp_log2(v, bits) ((bits == 64)? \
	(64U-1 - __builtin_clzll((u64)v)): \
	(32U-1 - __builtin_clz  ((u32)v)))

/**
 * @brief Internal helper to count leading zeros.
 * @param v The value (must be non-zero).
 * @param bits The bit-width of the value (32 or 64).
 * @return The number of leading zero bits.
 *
 * Used in conversion functions to allow the compiler to use the CLZ result
 * directly as a shift amount, avoiding the round-trip through log2:
 *   log2(v) = bits-1 - clz(v),  shift = bits-1 - log2(v) = clz(v)
 */
#define __intfp_clz(v, bits) ((bits == 64)? \
	__builtin_clzll((u64)v): \
	(__builtin_clz  ((u32)v) - (32 - (bits))))

/**
 * @brief Generates a bitmask for the lower 'h+1' bits.
 * @param h The highest bit position to include in the mask (0-indexed).
 * @param bits The total bit-width of the mask (32 or 64).
 * @return A bitmask of type u##bits with the lower h+1 bits set to 1.
 */
#define intfp_bitmask(h, bits) ((bits == 64)? \
	(~(u64)0 >> (64-1 - (h))): \
	(~(u32)0 >> (32-1 - (h))))

/* Min/Max constants for various integer types */
#define intfp_unsigned_min(bits) ((u##bits)1)
#define intfp_unsigned_max(bits) ((u##bits)-1)
#define intfp_signed_min(bits) ((s##bits)((u##bits)1 << (bits-1)))
#define intfp_signed_max(bits) ((s##bits)((u##bits)-1 & ~((u##bits)1 << (bits-1))))

/**
 * @brief Defines the special representation for the value '0' in 'pul' format.
 * In 'pul' representation, 0 is mapped to 1 to distinguish it from the
 * value '1', which is mapped to 0.
 */
#define intfp_pul_0(bits) intfp_unsigned_min(bits)

/**
 * @brief Defines the special representation for the value '0' in 'log' format.
 * In 'log' representation, 0 is mapped to the most negative number to reserve
 * the rest of the range for valid logarithmic values.
 */
#define intfp_log_0(bits) intfp_signed_min(bits)

/**
 * @brief Calculates the optimal number of exponent bits for a 'pul' conversion.
 *
 * This function determines the ideal number of bits to allocate for the exponent
 * field in a 'pul' representation to maximize the precision of the mantissa.
 *
 * @param int_bits The bit-width of the source integer type (e.g., 64 for u64).
 * @param pul_bits The bit-width of the destination 'pul' type (e.g., 32 for pul32).
 * @return The optimal number of exponent bits for the 'pul' format.
 */
u8 intfp_pul_fpmax(u8 int_bits, u8 pul_bits) {
	return pul_bits - intfp_fls32(int_bits-1);
}

/**
 * @brief Calculates the optimal number of exponent bits for a 'log' conversion.
 *
 * This function determines the ideal number of bits to allocate for the exponent
 * field in a 'log' representation. It accounts for the sign bit to maximize
 * the precision of the mantissa.
 *
 * @param int_bits The bit-width of the source integer type (e.g., 64 for u64).
 * @param pul_bits The bit-width of the destination 'log' type (e.g., 32 for log32).
 * @return The optimal number of exponent bits for the 'log' format.
 */
u8 intfp_log_fpmax(u8 int_bits, u8 pul_bits) {
	return pul_bits-1 - intfp_fls32(int_bits-1);
}

/**
 * @brief Pre-computed correction tables for improved log-domain precision.
 *
 * These tables store the quadratic correction c·m·(1-m) in Q0.16 fixed-point,
 * indexed by the top 8 bits of the fractional mantissa (256 entries).
 * This replaces two integer multiplications with a single table lookup,
 * reducing correction overhead from ~10 cycles to ~4 cycles.
 *
 * encode: c = 89/256 ≈ 0.3477,  lut[i] = round(89 * i * (256-i) / 256)
 * decode: c = 88/256 ≈ 0.3438,  lut[i] = round(88 * i * (256-i) / 256)
 * Max value: 5696 (encode) / 5632 (decode) at i=128, well within u16 range.
 */
const u16 __intfp_enc_corr_lut[256] = {
	    0,    89,   177,   264,   350,   436,   521,   606,   690,   773,   855,   937,  1018,  1098,  1178,  1257,
	 1335,  1413,  1489,  1565,  1641,  1716,  1790,  1863,  1936,  2008,  2079,  2150,  2219,  2289,  2357,  2425,
	 2492,  2558,  2624,  2689,  2753,  2817,  2880,  2942,  3004,  3065,  3125,  3184,  3243,  3301,  3358,  3415,
	 3471,  3526,  3581,  3635,  3688,  3740,  3792,  3843,  3894,  3943,  3992,  4041,  4088,  4135,  4182,  4227,
	 4272,  4316,  4360,  4402,  4444,  4486,  4526,  4566,  4606,  4644,  4682,  4719,  4756,  4792,  4827,  4861,
	 4895,  4928,  4960,  4992,  5023,  5053,  5083,  5112,  5140,  5167,  5194,  5220,  5245,  5270,  5294,  5317,
	 5340,  5362,  5383,  5404,  5423,  5443,  5461,  5479,  5496,  5512,  5528,  5543,  5557,  5570,  5583,  5596,
	 5607,  5618,  5628,  5637,  5646,  5654,  5661,  5668,  5674,  5679,  5683,  5687,  5690,  5693,  5695,  5696,
	 5696,  5696,  5695,  5693,  5690,  5687,  5683,  5679,  5674,  5668,  5661,  5654,  5646,  5637,  5628,  5618,
	 5607,  5596,  5583,  5570,  5557,  5543,  5528,  5512,  5496,  5479,  5461,  5443,  5423,  5404,  5383,  5362,
	 5340,  5317,  5294,  5270,  5245,  5220,  5194,  5167,  5140,  5112,  5083,  5053,  5023,  4992,  4960,  4928,
	 4895,  4861,  4827,  4792,  4756,  4719,  4682,  4644,  4606,  4566,  4526,  4486,  4444,  4402,  4360,  4316,
	 4272,  4227,  4182,  4135,  4088,  4041,  3992,  3943,  3894,  3843,  3792,  3740,  3688,  3635,  3581,  3526,
	 3471,  3415,  3358,  3301,  3243,  3184,  3125,  3065,  3004,  2942,  2880,  2817,  2753,  2689,  2624,  2558,
	 2492,  2425,  2357,  2289,  2219,  2150,  2079,  2008,  1936,  1863,  1790,  1716,  1641,  1565,  1489,  1413,
	 1335,  1257,  1178,  1098,  1018,   937,   855,   773,   690,   606,   521,   436,   350,   264,   177,    89,
};
const u16 __intfp_dec_corr_lut[256] = {
	    0,    88,   175,   261,   346,   431,   516,   599,   682,   764,   846,   926,  1006,  1086,  1165,  1243,
	 1320,  1397,  1473,  1548,  1622,  1696,  1770,  1842,  1914,  1985,  2056,  2125,  2194,  2263,  2331,  2398,
	 2464,  2530,  2595,  2659,  2722,  2785,  2848,  2909,  2970,  3030,  3090,  3148,  3206,  3264,  3321,  3377,
	 3432,  3487,  3541,  3594,  3646,  3698,  3750,  3800,  3850,  3899,  3948,  3995,  4042,  4089,  4135,  4180,
	 4224,  4268,  4311,  4353,  4394,  4435,  4476,  4515,  4554,  4592,  4630,  4666,  4702,  4738,  4773,  4807,
	 4840,  4873,  4905,  4936,  4966,  4996,  5026,  5054,  5082,  5109,  5136,  5161,  5186,  5211,  5235,  5258,
	 5280,  5302,  5323,  5343,  5362,  5381,  5400,  5417,  5434,  5450,  5466,  5480,  5494,  5508,  5521,  5533,
	 5544,  5555,  5565,  5574,  5582,  5590,  5598,  5604,  5610,  5615,  5620,  5623,  5626,  5629,  5631,  5632,
	 5632,  5632,  5631,  5629,  5626,  5623,  5620,  5615,  5610,  5604,  5598,  5590,  5582,  5574,  5565,  5555,
	 5544,  5533,  5521,  5508,  5494,  5480,  5466,  5450,  5434,  5417,  5400,  5381,  5362,  5343,  5323,  5302,
	 5280,  5258,  5235,  5211,  5186,  5161,  5136,  5109,  5082,  5054,  5026,  4996,  4966,  4936,  4905,  4873,
	 4840,  4807,  4773,  4738,  4702,  4666,  4630,  4592,  4554,  4515,  4476,  4435,  4394,  4353,  4311,  4268,
	 4224,  4180,  4135,  4089,  4042,  3995,  3948,  3899,  3850,  3800,  3750,  3698,  3646,  3594,  3541,  3487,
	 3432,  3377,  3321,  3264,  3206,  3148,  3090,  3030,  2970,  2909,  2848,  2785,  2722,  2659,  2595,  2530,
	 2464,  2398,  2331,  2263,  2194,  2125,  2056,  1985,  1914,  1842,  1770,  1696,  1622,  1548,  1473,  1397,
	 1320,  1243,  1165,  1086,  1006,   926,   846,   764,   682,   599,   516,   431,   346,   261,   175,    88,
};

/**
 * @brief Generates the core conversion functions between integer, fixed-point,
 * 'pul', and 'log' representations.
 * @param hbits The bit-width of the source/destination integer or fixed-point type.
 * @param lbits The bit-width of the destination/source 'pul' or 'log' type.
 */
#define INTFP_DECL_HBITS_LBITS(hbits, lbits) \
/* --- Standard Integer <-> Fixed-Point Conversions --- */ \
/** @brief Converts an integer to a fixed-point value by left-shifting. */ \
u##hbits u##lbits##_to_u##hbits##fp(u##lbits v, u8 fp) { \
	return (u##hbits)v << fp; \
} \
/** @brief Converts a fixed-point value back to an integer by right-shifting. */ \
u##lbits u##hbits##fp_to_u##lbits(u##hbits v, u8 fp) { \
	return v >> fp; \
} \
/** @brief Converts a signed integer to a signed fixed-point value. */ \
s##hbits s##lbits##_to_s##hbits##fp(s##lbits v, u8 fp) { \
	return (s##hbits)v << fp; \
} \
/** @brief Converts a signed fixed-point value back to a signed integer. */ \
s##lbits s##hbits##fp_to_s##lbits(s##hbits v, u8 fp) { \
	return v >> fp; \
} \
\
/* --- Packed Unsigned Log ('pul') Representation --- */ \
/** \
 * The 'pul' format approximates an unsigned integer in a compact, logarithmic form. \
 * It is composed of an exponent and a mantissa: | exponent | mantissa | \
 * - Ideal for storage or network transmission to save space. \
 * - Provides high precision for its bit size. \
 * - Non-calculatable: must be converted back to a linear format for arithmetic. \
 * - Special encoding: value 0 -> 1, value 1 -> 0. \
 */ \
\
/** \
 * @brief Converts an unsigned integer to its 'pul' representation. \
 * @param v The input unsigned integer. \
 * @param ofp The number of bits to use for mantissa in the output 'pul' value. \
 *            The remaining bits (lbits - ofp) will be used for the mantissa. \
 *            The range is 1 to (lbits - 1 - fls(lbits)). \
 * @return The 'pul' representation of the value. \
 */ \
u##lbits u##hbits##_to_pul##lbits##fp(u##hbits v, u8 ofp) { \
	if (v <= 1) return !v; /* Special encoding: v=0 -> 1, v=1 -> 0 */ \
	u8 clz = __intfp_clz(v, hbits); \
	/* Keep implicit leading 1 in mantissa; addition carries it into exponent */ \
	u##lbits m = (u##hbits)(v << clz) >> (hbits - 1 - ofp); \
	return ((u##lbits)(hbits - 2 - clz) << ofp) + m; \
} \
/** @brief Converts to 'pul' using the maximum possible precision for the mantissa. */ \
u##lbits u##hbits##_to_pul##lbits##fpmax(u##hbits v) { \
	return u##hbits##_to_pul##lbits##fp( \
		v, intfp_pul_fpmax(hbits, lbits)); \
} \
\
/** \
 * @brief Converts a 'pul' representation back to an unsigned integer. \
 * @param v The input 'pul' value. \
 * @param ifp The number of bits used for the exponent in the input 'pul' value. \
 *            The range is 1 to (hbits - 1 - fls(hbits)). \
 * @return The reconstructed unsigned integer. Returns max value on overflow. \
 */ \
u##hbits pul##lbits##fp_to_u##hbits(u##lbits v, u8 ifp) { \
	if (v == intfp_pul_0(lbits)) return 0; /* pul value of 1 represents 0 */ \
	u##lbits e = v >> ifp; /* Extract exponent */ \
	if (e >= hbits) return intfp_unsigned_max(hbits); /* Avoid overflow */ \
	u##hbits m = v & intfp_bitmask(ifp - 1, lbits); /* Extract mantissa */ \
	/* Reconstruct the normalized value by adding the implicit leading '1' */ \
	u##hbits norm = (u##hbits)1 << (hbits-1) | (m << (hbits-1 - ifp)); \
	/* De-normalize by shifting right based on the exponent */ \
	return norm >> (hbits-1 - e); \
} \
/** @brief Converts from 'pul' using the maximum possible precision for the mantissa. */ \
u##hbits pul##lbits##fpmax_to_u##hbits(u##hbits v) { \
	return pul##lbits##fp_to_u##hbits( \
		v, intfp_pul_fpmax(hbits, lbits)); \
} \
\
/* --- Signed Logarithmic ('log') Representation --- */ \
/** \
 * The 'log' format approximates a number in a signed logarithmic form. \
 * It is composed of a sign, exponent, and mantissa: | sign | exponent | mantissa | \
 * - Calculatable: Supports operations like multiplication/division via addition/subtraction. \
 * - Can represent values less than 1.0 (as negative log values). \
 * - Precision is slightly lower than 'pul' due to the sign bit. \
 * - Special encoding for zero: intfp_log_0(bits), which is the most negative value. \
 * \
 * @warning --- Approximation Method --- \
 * This 'log' representation is a high-speed approximation and does NOT represent \
 * the true mathematical base-2 logarithm. It calculates the exponent `e = floor(log2(v))` \
 * and uses the *linear* fractional part of the normalized value as the mantissa. \
 * \
 *   - True Logarithm: `log2(v) = e + log2(1 + m)` \
 *   - This Library's 'log' Value: `e + m` \
 * \
 * This linear approximation (`e + m`) avoids costly `log` function calls, making it \
 * extremely fast. However, it introduces a systematic error compared to the true \
 * logarithmic value. This error is smallest when the input value `v` is close to a \
 * power of two and largest in between. \
 * \
 * Consequently, adding a 'log' value from this library to a true logarithmic value \
 * (e.g., one calculated externally or by the `rescale` functions) will result in a \
 * predictable but non-zero error. \
 */ \
\
/** \
 * @brief Converts an unsigned fixed-point value to its 'log' representation. \
 * \
 * This function creates a fast, approximate logarithmic representation of a value. \
 * It combines the integer part of the base-2 logarithm (exponent) with a linear \
 * representation of the fractional part (mantissa). \
 * \
 * @param v The input unsigned fixed-point value. \
 * @param ifp The number of fractional bits in the input fixed-point value `v`. \
 * @param ofp The number of bits to use for mantissa in the output 'log' value. \
 * @return The approximate 'log' representation of the value. \
 */ \
s##lbits u##hbits##fp_to_log##lbits##fp(u##hbits v, u8 ifp, u8 ofp) { \
	if (v == 0) return intfp_log_0(lbits); \
	u8 clz = __intfp_clz(v, hbits); \
	/* Keep implicit leading 1 in mantissa; addition carries it into exponent. \
	 * ifp adjustment is folded into the exponent term (no extra instruction). */ \
	u##lbits m = (u##hbits)(v << clz) >> (hbits - 1 - ofp); \
	return (s##lbits)(((u##lbits)(hbits - 2 - clz - ifp) << ofp) + m); \
} \
/** @brief Converts to 'log' using max precision, from a fixed-point value. */ \
s##lbits u##hbits##fp_to_log##lbits##fpmax(u##hbits v, u8 ifp) { \
	return u##hbits##fp_to_log##lbits##fp( \
		v, ifp, intfp_log_fpmax(hbits, lbits)); \
} \
/** @brief Converts an unsigned integer (no fractional part) to 'log' representation. */ \
s##lbits u##hbits##_to_log##lbits##fp(u##hbits v, u8 ofp) { \
	return u##hbits##fp_to_log##lbits##fp(v, 0, ofp); \
} \
/** @brief Converts an unsigned integer to 'log' using max precision. */ \
s##lbits u##hbits##_to_log##lbits##fpmax(u##hbits v) { \
	return u##hbits##fp_to_log##lbits##fpmax(v, 0); \
} \
\
/* --- Corrected 'log' Representation (_corr suffix) --- */ \
/** \
 * The '_corr' variants improve upon 'log' by applying a pre-computed LUT \
 * correction to both encode and decode, reducing end-to-end multiplication \
 * error from ~11% to ~1.3%. The correction uses a 256-entry table lookup \
 * instead of multiplications for minimal latency (1024 bytes total). \
 * \
 * Corrected values share the same bit-level format as 'log' (they are just \
 * more accurate approximations of true log2), so corrected and uncorrected \
 * values can be freely added/subtracted. However, mixing corrected and \
 * uncorrected encode/decode will degrade the precision benefit. \
 */ \
\
/** \
 * @brief Converts an unsigned fixed-point value to corrected 'log' representation. \
 * \
 * Applies LUT-based correction: log2(1+m) ≈ m + enc_lut[top8(m)] \
 * Reduces max log-domain error from 0.0861 to ~0.008 (11x improvement). \
 * \
 * @param v The input unsigned fixed-point value. \
 * @param ifp The number of fractional bits in the input fixed-point value `v`. \
 * @param ofp The number of bits to use for mantissa in the output value. \
 * @return The corrected 'log' representation of the value. \
 */ \
s##lbits u##hbits##fp_to_log##lbits##fp_corr(u##hbits v, u8 ifp, u8 ofp) { \
	if (v == 0) return intfp_log_0(lbits); \
	u8 clz = __intfp_clz(v, hbits); \
	u##lbits m = (u##hbits)(v << clz) >> (hbits - 1 - ofp); \
	/* LUT correction: index by top 8 bits of fractional mantissa */ \
	u##lbits _mf = m & intfp_bitmask(ofp - 1, lbits); \
	u8 _idx = (ofp >= 8) ? \
		(u8)(_mf >> (ofp - 8)) : (u8)(_mf << (8 - ofp)); \
	m += (ofp <= 16) ? \
		(u##lbits)(__intfp_enc_corr_lut[_idx] >> (16 - ofp)) : \
		(u##lbits)((u##lbits)__intfp_enc_corr_lut[_idx] << (ofp - 16)); \
	return (s##lbits)(((u##lbits)(hbits - 2 - clz - ifp) << ofp) + m); \
} \
/** @brief Converts to corrected 'log' using max precision, from a fixed-point value. */ \
s##lbits u##hbits##fp_to_log##lbits##fpmax_corr(u##hbits v, u8 ifp) { \
	return u##hbits##fp_to_log##lbits##fp_corr( \
		v, ifp, intfp_log_fpmax(hbits, lbits)); \
} \
/** @brief Converts an unsigned integer to corrected 'log' representation. */ \
s##lbits u##hbits##_to_log##lbits##fp_corr(u##hbits v, u8 ofp) { \
	return u##hbits##fp_to_log##lbits##fp_corr(v, 0, ofp); \
} \
/** @brief Converts an unsigned integer to corrected 'log' using max precision. */ \
s##lbits u##hbits##_to_log##lbits##fpmax_corr(u##hbits v) { \
	return u##hbits##fp_to_log##lbits##fpmax_corr(v, 0); \
} \
\
/** \
 * @brief Converts a 'log' representation back to an unsigned fixed-point value. \
 * @param v The input 'log' value. \
 * @param ifp The number of bits used for the exponent in the input 'log' value. \
 * @param ofp The number of fractional bits in the output fixed-point value. \
 * @return The reconstructed unsigned fixed-point value. \
 */ \
u##hbits log##lbits##fp_to_u##hbits##fp(s##lbits v, u8 ifp, u8 ofp) { \
	if (v == intfp_log_0(lbits)) return 0; \
	bool negative = v < 0; \
	if (negative) v = -v; \
	/* The exponent itself is signed. A negative log value means the original value was < 1.0 */ \
	s##lbits e = v >> ifp; \
	if (negative) e = -e; \
	/* Adjust exponent for the output fixed-point format */ \
	s##lbits scaled_e = e + ofp; \
	if (scaled_e < 0) return 0; /* Underflow */ \
	if (scaled_e >= hbits) return intfp_unsigned_max(hbits); /* Overflow */ \
	u##hbits m = v & intfp_bitmask(ifp - 1, lbits); \
	u##hbits norm = (u##hbits)1 << (hbits-1) | (m << (hbits-1 - ifp)); \
	return norm >> (hbits-1 - scaled_e); \
} \
/** @brief Converts from 'log' (max precision) to a fixed-point value. */ \
u##hbits log##lbits##fpmax_to_u##hbits##fp(s##lbits v, u8 ofp) { \
	return log##lbits##fp_to_u##hbits##fp( \
		v, intfp_log_fpmax(hbits, lbits), ofp); \
} \
/** @brief Converts a 'log' value to an integer (no fractional part). */ \
u##hbits log##lbits##fp_to_u##hbits(s##lbits v, u8 ifp) { \
	return log##lbits##fp_to_u##hbits##fp(v, ifp, 0); \
} \
/** @brief Converts from 'log' (max precision) to an unsigned integer. */ \
u##hbits log##lbits##fpmax_to_u##hbits(s##lbits v) { \
	return log##lbits##fpmax_to_u##hbits##fp(v, 0); \
} \
\
/** \
 * @brief Converts a corrected 'log' representation back to an unsigned fixed-point value. \
 * \
 * Applies LUT-based correction: 2^m ≈ (1+m) - dec_lut[top8(m)] \
 * Corrects the decode-side error where (1+m) overestimates 2^m. \
 * \
 * @param v The input corrected 'log' value. \
 * @param ifp The number of bits used for the exponent in the input value. \
 * @param ofp The number of fractional bits in the output fixed-point value. \
 * @return The reconstructed unsigned fixed-point value. \
 */ \
u##hbits log##lbits##fp_to_u##hbits##fp_corr(s##lbits v, u8 ifp, u8 ofp) { \
	if (v == intfp_log_0(lbits)) return 0; \
	bool negative = v < 0; \
	if (negative) v = -v; \
	s##lbits e = v >> ifp; \
	if (negative) e = -e; \
	s##lbits scaled_e = e + ofp; \
	if (scaled_e < 0) return 0; \
	if (scaled_e >= hbits) return intfp_unsigned_max(hbits); \
	u##hbits m = v & intfp_bitmask(ifp - 1, lbits); \
	u##hbits norm = (u##hbits)1 << (hbits-1) | (m << (hbits-1 - ifp)); \
	/* LUT correction: 2^m ≈ (1+m) - dec_lut[top8(m)] */ \
	u##hbits _mh = m << (hbits - 1 - ifp); \
	u8 _idx = ((hbits-1) >= 8) ? \
		(u8)(_mh >> ((hbits-1) - 8)) : (u8)((u32)_mh << (8 - (hbits-1))); \
	norm -= ((hbits-1) <= 16) ? \
		(u##hbits)(__intfp_dec_corr_lut[_idx] >> (16 - (hbits-1))) : \
		(u##hbits)((u##hbits)__intfp_dec_corr_lut[_idx] << ((hbits-1) - 16)); \
	return norm >> (hbits-1 - scaled_e); \
} \
/** @brief Converts from corrected 'log' (max precision) to a fixed-point value. */ \
u##hbits log##lbits##fpmax_to_u##hbits##fp_corr(s##lbits v, u8 ofp) { \
	return log##lbits##fp_to_u##hbits##fp_corr( \
		v, intfp_log_fpmax(hbits, lbits), ofp); \
} \
/** @brief Converts a corrected 'log' value to an integer (no fractional part). */ \
u##hbits log##lbits##fp_to_u##hbits##_corr(s##lbits v, u8 ifp) { \
	return log##lbits##fp_to_u##hbits##fp_corr(v, ifp, 0); \
} \
/** @brief Converts from corrected 'log' (max precision) to an unsigned integer. */ \
u##hbits log##lbits##fpmax_to_u##hbits##_corr(s##lbits v) { \
	return log##lbits##fpmax_to_u##hbits##fp_corr(v, 0); \
}

/* Generate conversion functions for various bit-width combinations */
INTFP_DECL_HBITS_LBITS( 8, 8)
INTFP_DECL_HBITS_LBITS(16, 8)
INTFP_DECL_HBITS_LBITS(32, 8)
INTFP_DECL_HBITS_LBITS(64, 8)
INTFP_DECL_HBITS_LBITS(16,16)
INTFP_DECL_HBITS_LBITS(32,16)
INTFP_DECL_HBITS_LBITS(64,16)
INTFP_DECL_HBITS_LBITS(32,32)
INTFP_DECL_HBITS_LBITS(64,32)
INTFP_DECL_HBITS_LBITS(64,64)


/**
 * @brief Generates functions for converting between different 'pul' and 'log' types.
 * These functions allow changing the bit-width (e.g., log8 to log16) or the
 * exponent/mantissa allocation within the same bit-width.
 * @param ibits The bit-width of the input 'pul'/'log' type.
 * @param obits The bit-width of the output 'pul'/'log' type.
 */
#define INTFP_DECL_IBITS_OBITS(ibits, obits) \
/* --- In-type conversions (bit-width and exponent/mantissa ratio changes) --- */ \
\
/** @brief Converts a 'pul' value to another 'pul' type, adjusting for exponent bits. */ \
u##obits pul##ibits##fp_to_pul##obits##fp(u##ibits v, u8 ifp, u8 ofp) { \
	if (v == intfp_pul_0(ibits)) return intfp_pul_0(obits); \
	/* Conversion is a simple shift if the exponent bit allocation changes. */ \
	return (ifp == ofp) ? v : ((ifp < ofp) ? \
		(v << (ofp - ifp)): (v >> (ifp - ofp))); \
} \
/** @brief Converts 'pul' to 'pul' using max precision settings for both. */ \
u##obits pul##ibits##fpmax_to_pul##obits##fpmax(u##ibits v) { \
	return pul##ibits##fp_to_pul##obits##fp( \
		v, ibits - intfp_fls32(ibits-1), obits - intfp_fls32(ibits-1)); \
} \
/** @brief Converts a 'log' value to another 'log' type, adjusting for exponent bits. */ \
s##obits log##ibits##fp_to_log##obits##fp(s##ibits v, u8 ifp, u8 ofp) { \
	if (v == intfp_log_0(ibits)) return intfp_log_0(obits); \
	return (ifp == ofp) ? v : (s##obits)((ifp < ofp) ? \
		((u##ibits)v << (ofp - ifp)): ((u##ibits)v >> (ifp - ofp))); \
} \
/** @brief Converts 'log' to 'log' using max precision settings for both. */ \
s##obits log##ibits##fpmax_to_log##obits##fpmax(s##ibits v) { \
	return log##ibits##fp_to_log##obits##fp( \
		v, ibits-1 - intfp_fls32(ibits-1), obits-1 - intfp_fls32(ibits-1)); \
} \
\
/* --- Inter-type conversions ('pul' <-> 'log') --- */ \
\
/** @brief Converts a 'pul' value to a 'log' value. */ \
s##obits pul##ibits##fp_to_log##obits##fp(u##ibits v, u8 ifp, u8 ofp) { \
	if (v == intfp_pul_0(ibits)) return intfp_log_0(obits); \
	/* Since 'pul' is always positive, this is just a bit-width/ratio change. */ \
	return (ifp == ofp) ? v : ((ifp < ofp) ? \
		(v << (ofp - ifp)): (v >> (ifp - ofp))); \
} \
/** @brief Converts 'pul' (max precision) to 'log' (max precision). */ \
s##obits pul##ibits##fpmax_to_log##obits##fpmax(u##ibits v) { \
	return pul##ibits##fp_to_log##obits##fp( \
		v, ibits - intfp_fls32(ibits-1), obits-1 - intfp_fls32(ibits-1)); \
} \
/** @brief Converts a 'log' value to a 'pul' value. */ \
u##obits log##ibits##fp_to_pul##obits##fp(s##ibits v, u8 ifp, u8 ofp) { \
	/* 'pul' cannot represent negative 'log' values (i.e., values < 1.0) */ \
	if (v < 0) return intfp_pul_0(obits); \
	return (ifp == ofp) ? (u##obits)v : (u##obits)((ifp < ofp) ? \
		((u##ibits)v << (ofp - ifp)): ((u##ibits)v >> (ifp - ofp))); \
} \
/** @brief Converts 'log' (max precision) to 'pul' (max precision). */ \
u##obits log##ibits##fpmax_to_pul##obits##fpmax(s##ibits v) { \
	return log##ibits##fp_to_pul##obits##fp( \
		v, ibits-1 - intfp_fls32(ibits-1), obits - intfp_fls32(ibits-1)); \
}

/* Generate type-conversion functions for various bit-width combinations */
INTFP_DECL_IBITS_OBITS( 8, 8)
INTFP_DECL_IBITS_OBITS( 8,16)
INTFP_DECL_IBITS_OBITS( 8,32)
INTFP_DECL_IBITS_OBITS( 8,64)
INTFP_DECL_IBITS_OBITS(16, 8)
INTFP_DECL_IBITS_OBITS(16,16)
INTFP_DECL_IBITS_OBITS(16,32)
INTFP_DECL_IBITS_OBITS(16,64)
INTFP_DECL_IBITS_OBITS(32, 8)
INTFP_DECL_IBITS_OBITS(32,16)
INTFP_DECL_IBITS_OBITS(32,32)
INTFP_DECL_IBITS_OBITS(32,64)
INTFP_DECL_IBITS_OBITS(64, 8)
INTFP_DECL_IBITS_OBITS(64,16)
INTFP_DECL_IBITS_OBITS(64,32)
INTFP_DECL_IBITS_OBITS(64,64)

/**
 * @brief Generates Exponentially Weighted Moving Average (EWMA) functions.
 * EWMA is used to create a smoothed average of a series of numbers.
 * @param bits The bit-width (8, 16, 32, 64) for the signed integer types.
 */
#define INTFP_DECL_BITS(bits) \
/** \
 * @brief Calculates the EWMA using integer division. \
 * @param new The new value to incorporate into the average. \
 * @param old The previous average value. \
 * @param bottom_limit A floor value; any input below this is clamped to it. \
 * @param damper The damping factor (divisor). A higher value means slower changes. \
 * @return The updated average. \
 */ \
s##bits ewma_s##bits##fp_div(s##bits new, s##bits old, \
		s##bits bottom_limit, u##bits damper) { \
	u##bits abs_diff, adj_diff; \
	if (damper <= 1) return new; \
	if (old < bottom_limit) old = bottom_limit; \
	if (new < bottom_limit) new = bottom_limit; \
	if (new == old) return old; \
	abs_diff = (new > old) ? (new - old) : (old - new); \
	/* Ceiling division to ensure the average moves even for small diffs */ \
	adj_diff = (abs_diff / damper) + ((abs_diff % damper) != 0); \
	return (new > old) ? (old + adj_diff) : (old - adj_diff); \
} \
/** \
 * @brief Calculates the EWMA using a bitwise right shift (faster but less precise). \
 * This is efficient when the damper is a power of 2. \
 * @param new The new value to incorporate into the average. \
 * @param old The previous average value. \
 * @param bottom_limit A floor value; any input below this is clamped to it. \
 * @param damper The damping factor (shift amount). A higher value means slower changes. \
 * @return The updated average. \
 */ \
s##bits ewma_s##bits##fp_shr(s##bits new, s##bits old, \
		s##bits bottom_limit, u8 damper) { \
	u##bits abs_diff, adj_diff; \
	if (damper <= 1) return new; \
	if (old < bottom_limit) old = bottom_limit; \
	if (new < bottom_limit) new = bottom_limit; \
	if (new == old) return old; \
	abs_diff = (new > old) ? (new - old) : (old - new); \
	adj_diff = (abs_diff >> damper); \
	return (new > old) ? (old + adj_diff) : (old - adj_diff); \
}
/* Generate EWMA functions for 8, 16, 32, and 64-bit signed integers */
INTFP_DECL_BITS(8)
INTFP_DECL_BITS(16)
INTFP_DECL_BITS(32)
INTFP_DECL_BITS(64)

/**
 * @enum u32fp_radix_type
 * @brief Defines different logarithmic bases for radix conversion.
 */
enum u32fp_radix_type {
	U32FP_RADIX_TYPE_DB_POWER, /**< Base for decibel-milliwatts (dBm) or similar power ratios. */
	U32FP_RADIX_TYPE_1_25,     /**< Base for 1.25x scaling factor. */
	U32FP_RADIX_TYPE_COUNT     /**< The number of available radix types. */
};

/**
 * @struct u32fp_radix
 * @brief Holds fixed-point constants for converting between log2 and another log base.
 */
struct u32fp_radix {
	u32 to;        /**< Fixed-point constant for log2 -> target_log conversion. */
	u32 from;      /**< Fixed-point constant for target_log -> log2 conversion. */
	u8  to_shr;   /**< Number of fractional bits in the 'to' constant. */
	u8  from_shr; /**< Number of fractional bits in the 'from' constant. */
};

/**
 * @brief Table of pre-calculated conversion constants for different logarithmic bases.
 */
const struct u32fp_radix u32fp_radix_tbl[U32FP_RADIX_TYPE_COUNT] = {
	[U32FP_RADIX_TYPE_DB_POWER] = {
		.to   = 0xC0A8C129, .to_shr   = 30, /* Constant for converting to a dB-like scale */
		.from = 0x550A9686, .from_shr = 32, /* Constant for converting from a dB-like scale */
	},
	[U32FP_RADIX_TYPE_1_25] = {
		.to   = 0xC6CD5A3B, .to_shr   = 30, /* Constant for converting to a log_1.25 scale */
		.from = 0x5269E11A, .from_shr = 32, /* Constant for converting from a log_1.25 scale */
	},
};

/**
 * @brief Generates functions to rescale 'log' values between base-2 and another radix.
 * These functions are useful for converting internal log representations to human-readable
 * formats like decibels.
 * @param bits The bit-width of the 'log' type (8, 16, 32).
 */
#define INTFP_DECL_BITS_UP_TO_32(bits) \
/** \
 * @brief Rescales a base-2 'log' value to a target radix (e.g., dB scale). \
 * @param v The input 'log' value (base-2). \
 * @param type The target radix type from u32fp_radix_type. \
 * @return The rescaled 'log' value in the new base. \
 */ \
s##bits rescale_log##bits##fp_to_radix(s##bits v, enum u32fp_radix_type type) { \
	const struct u32fp_radix *radix = &u32fp_radix_tbl[type]; \
	if (v == 0 || v == intfp_log_0(bits)) return v; \
	bool negative = v < 0; \
	if (negative) v = -v; \
	/* The multiplication is a fixed-point operation. v is treated as Q32.0. */ \
	s##bits temp = ((u64)v * radix->to) >> radix->to_shr; \
	if (negative) temp = -temp; \
	return (s##bits)temp; \
} \
/** \
 * @brief Rescales a 'log' value from a target radix back to base-2. \
 * @param v The input 'log' value in the target radix. \
 * @param type The radix type of the input value. \
 * @return The rescaled 'log' value in base-2. \
 */ \
s##bits rescale_log##bits##fp_from_radix(s##bits v, enum u32fp_radix_type type) { \
	const struct u32fp_radix *radix = &u32fp_radix_tbl[type]; \
	if (v == 0 || v == intfp_log_0(bits)) return v; \
	bool negative = v < 0; \
	if (negative) v = -v; \
	s##bits temp = ((u64)v * radix->from) >> radix->from_shr; \
	if (negative) temp = -temp; \
	return (s##bits)temp; \
}
/* Generate radix conversion functions for 8, 16, and 32-bit log types */
INTFP_DECL_BITS_UP_TO_32(8)
INTFP_DECL_BITS_UP_TO_32(16)
INTFP_DECL_BITS_UP_TO_32(32)

#endif /* _INTFP_H */
