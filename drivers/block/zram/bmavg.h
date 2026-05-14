/*
 * bmavg.h - Binary-Merge Average Filter for Linux Kernel (Fixed)
 */
#ifndef BMAVG_H
#define BMAVG_H

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/bitops.h>
#include <linux/spinlock.h> /* Added for concurrency safety */

/* Map types to Kernel shorthand */
typedef u8  bmavg_u8_t;
typedef u16 bmavg_u16_t;
typedef u32 bmavg_u32_t;
typedef u64 bmavg_u64_t;

/* 
 * Macro to construct type names 
 */
#define BMAVG_CONCAT_RAW(a, b) a##b
#define BMAVG_CONCAT(a, b) BMAVG_CONCAT_RAW(a, b)
#define bmavg_uint(N) BMAVG_CONCAT(BMAVG_CONCAT(bmavg_u, N), _t)

/* 
 * Struct definition with Spinlock 
 */
#define DEFINE_BMAVG_DECL(BITLEN, HIST_COUNT) \
extern const int bmavg_hist_count_u##BITLEN; \
struct bmavg_u##BITLEN { \
    spinlock_t lock; \
    bmavg_uint(BITLEN) hist[HIST_COUNT]; \
    bmavg_uint(BITLEN) count; \
    int limit_bitlen; \
}; \
void bmavg_write_u##BITLEN(struct bmavg_u##BITLEN *bmavg, bmavg_uint(BITLEN) v); \
bmavg_uint(BITLEN) bmavg_read_u##BITLEN(struct bmavg_u##BITLEN *bmavg); \
void bmavg_init_u##BITLEN(struct bmavg_u##BITLEN *bmavg); \
void bmavg_reset_u##BITLEN(struct bmavg_u##BITLEN *bmavg); \
void bmavg_set_limit_u##BITLEN(struct bmavg_u##BITLEN *bmavg, int limit);

/* Define declarations for 8, 16, 32 bits */
DEFINE_BMAVG_DECL(8, 5)
DEFINE_BMAVG_DECL(16, 12)
DEFINE_BMAVG_DECL(32, 28)

#endif // BMAVG_H
