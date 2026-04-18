/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Cambyses — Context-Aware Migration Balancer Yielding Scored Entity Selection
 *
 * Scored migration selection for CFS load balancer Pull path.
 * Score = (now - exec_start) × weight, compressed to pul16 for SIMD argmax.
 */
#ifndef _KERNEL_SCHED_CAMBYSES_H
#define _KERNEL_SCHED_CAMBYSES_H

#include <linux/types.h>
#include <linux/jump_label.h>

#ifdef CONFIG_SCHED_CAMBYSES

/*
 * Candidate entry for scored migration selection.
 * Stored on stack during detach_tasks_cambyses().
 * Scores are kept in a separate u16 array for SIMD argmax access.
 */
struct cambyses_candidate {
	struct task_struct	*p;
};

/* Static key for zero-cost runtime disable (NOP patching) */
extern struct static_key_true sched_cambyses;

/* Debug trace static key */
extern struct static_key_false cambyses_debug;

/* Multi-source drain static key */
extern struct static_key_false cambyses_multisource_drain;

/*
 * SIMD support — separate TUs to prevent auto-vectorization contamination.
 * Each file is compiled with its own ISA flags.
 *
 * Two SIMD operations:
 *   1. u64 → pul16 batch conversion (Phase 1.5)
 *   2. u16 argmax (Phase 2)
 */
#ifdef CONFIG_SCHED_CAMBYSES_SIMD

/*
 * Scores array size for SIMD argmax — rounded up from
 * SCHED_NR_MIGRATE_BREAK to the next SIMD-friendly boundary.
 */
#ifdef SCHED_NR_MIGRATE_BREAK
#if SCHED_NR_MIGRATE_BREAK <= 8
#define CAMBYSES_SIMD_SCORES_SIZE	8
#elif SCHED_NR_MIGRATE_BREAK <= 16
#define CAMBYSES_SIMD_SCORES_SIZE	16
#elif SCHED_NR_MIGRATE_BREAK <= 32
#define CAMBYSES_SIMD_SCORES_SIZE	32
#else
#error "SCHED_NR_MIGRATE_BREAK > 32 not supported by Cambyses SIMD argmax"
#endif
#endif /* SCHED_NR_MIGRATE_BREAK */

/* Static keys for ISA dispatch — enabled at boot based on CPUID/HWCAP */
#ifdef CONFIG_X86_64
extern struct static_key_false cambyses_has_avx2;
extern struct static_key_false cambyses_has_ssse3;
extern struct static_key_false cambyses_has_sse41;
#endif
#ifdef CONFIG_ARM64
extern struct static_key_false cambyses_has_neon;
#endif

/*
 * SIMD argmax on u16 pul16 scores.
 * Returns index of maximum element, or -1 if all zero (tombstoned).
 *
 * x86 uses PHMINPOSUW (SSE4.1): XOR 0xFFFF converts u16-max to u16-min,
 * PHMINPOSUW returns both the minimum value and its lane index.
 */
#ifdef CONFIG_X86_64
int cambyses_simd_argmax_avx2_8(const u16 *scores);
int cambyses_simd_argmax_avx2_16(const u16 *scores);
int cambyses_simd_argmax_avx2_32(const u16 *scores);
int cambyses_simd_argmax_ssse3_8(const u16 *scores);
int cambyses_simd_argmax_ssse3_16(const u16 *scores);
int cambyses_simd_argmax_ssse3_32(const u16 *scores);
#endif

#ifdef CONFIG_ARM64
int cambyses_simd_argmax_neon_8(const u16 *scores);
int cambyses_simd_argmax_neon_16(const u16 *scores);
int cambyses_simd_argmax_neon_32(const u16 *scores);
#endif

/*
 * SIMD u64 → pul16 batch conversion.
 * Uses IEEE 754 double trick: u64→double cast, then extract exponent+mantissa.
 */
#ifdef CONFIG_X86_64
void cambyses_simd_u64_to_pul16_avx2(const u64 *raw, u16 *out, int nr);
void cambyses_simd_u64_to_pul16_ssse3(const u64 *raw, u16 *out, int nr);
#endif

#ifdef CONFIG_ARM64
void cambyses_simd_u64_to_pul16_neon(const u64 *raw, u16 *out, int nr);
#endif

/*
 * Compile-time dispatch — maps generic name to the size-correct variant.
 * Only available when SCHED_NR_MIGRATE_BREAK is known (i.e. in cambyses.c
 * via fair.c / sched.h).
 */
#ifdef CAMBYSES_SIMD_SCORES_SIZE
#define _CAMBYSES_PASTE(fn, sz)		fn##sz
#define _CAMBYSES_DISPATCH(fn, sz)	_CAMBYSES_PASTE(fn, sz)
#define cambyses_simd_argmax_avx2(s)	_CAMBYSES_DISPATCH(cambyses_simd_argmax_avx2_, CAMBYSES_SIMD_SCORES_SIZE)(s)
#define cambyses_simd_argmax_ssse3(s)	_CAMBYSES_DISPATCH(cambyses_simd_argmax_ssse3_, CAMBYSES_SIMD_SCORES_SIZE)(s)
#define cambyses_simd_argmax_neon(s)	_CAMBYSES_DISPATCH(cambyses_simd_argmax_neon_, CAMBYSES_SIMD_SCORES_SIZE)(s)
#endif /* CAMBYSES_SIMD_SCORES_SIZE */

/*
 * x86 SIMD vector type definitions — only available in TUs compiled
 * with the corresponding ISA flags.
 *
 * GCC vector extensions + __builtin_ia32_*.
 * <immintrin.h> is a userspace header and cannot be used in kernel.
 */
#ifdef __SSSE3__
typedef short v8hi  __attribute__((__vector_size__(16)));
typedef char  v16qi __attribute__((__vector_size__(16)));
typedef short v8hi_u  __attribute__((__vector_size__(16), __aligned__(2)));
#endif

#ifdef __AVX2__
typedef short v16hi __attribute__((__vector_size__(32)));
typedef char  v32qi __attribute__((__vector_size__(32)));
typedef short v16hi_u __attribute__((__vector_size__(32), __aligned__(2)));
#endif

#endif /* CONFIG_SCHED_CAMBYSES_SIMD */

#endif /* CONFIG_SCHED_CAMBYSES */
#endif /* _KERNEL_SCHED_CAMBYSES_H */
