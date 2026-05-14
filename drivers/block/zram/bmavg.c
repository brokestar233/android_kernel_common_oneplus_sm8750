/*
 *  Binary-Merge Average Filter (bmavg)
 *  Linux Kernel Adaptation (Fixed & Safe Version)
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/math64.h> /* For div_u64 */
#include <linux/spinlock.h>
#include "bmavg.h"

/* Double width types for accumulation */
#define BMAVG_DBL_8   16
#define BMAVG_DBL_16  32
#define BMAVG_DBL_32  64

#define bmavg_dbl_uint(N) bmavg_uint(BMAVG_CONCAT(BMAVG_DBL_, N))

/*
 * Division helpers
 */
#define bmavg_do_div_8(num, den)  ((num) / (den))
#define bmavg_do_div_16(num, den) ((num) / (den))
/* u64 / u32 must use div_u64 in kernel to support 32-bit archs */
#define bmavg_do_div_32(num, den) div_u64(num, den)

/* 
 * Safe bit manipulation helpers
 * Returns 2^p. If p >= width, returns 0.
 * This ensures (bmavg_pow2(v, 32) - 1) becomes 0 - 1 = UINT_MAX, which is correct mask.
 */
#define bmavg_pow2(val, p) \
    ((p) >= sizeof(val) * 8 ? 0 : ((__typeof__(val))1 << (p)))

#define bmavg_bit(v, p)  (((v) >> (p)) & 1)

/* Wrapper for fls */
#define bmavg_fls_8(v)  fls(v)
#define bmavg_fls_16(v) fls(v)
#define bmavg_fls_32(v) fls(v)

/*
 * Implementation Macro
 */
#define DEFINE_BMAVG(BITLEN, HIST_COUNT) \
const int bmavg_hist_count_u##BITLEN = HIST_COUNT; \
\
void bmavg_write_u##BITLEN(struct bmavg_u##BITLEN *bmavg, bmavg_uint(BITLEN) v) { \
    bmavg_uint(BITLEN) curr; \
    bmavg_uint(BITLEN) fmask_val, hmask_val; \
    bmavg_uint(BITLEN) next, should_halve, carried, carry_bmp; \
    int full, pos = 0; \
    unsigned long flags; \
    \
    spin_lock_irqsave(&bmavg->lock, flags); \
    \
    curr = bmavg->count; \
    /* Calculate masks safely to avoid UB on shift overflow */ \
    fmask_val = bmavg_pow2(curr, bmavg->limit_bitlen); \
    hmask_val = bmavg_pow2(curr, bmavg->limit_bitlen - 1); \
    \
    /* fmask_val - 1 generates a mask of all 1s if bitlen == width */ \
    full = (curr == (bmavg_uint(BITLEN))(fmask_val - 1)); \
    \
    if (unlikely(full)) \
        curr = hmask_val - 1; \
    \
    next = curr + 1; \
    should_halve = !(next % hmask_val); \
    carried = v; \
    carry_bmp = (next ^ curr) >> 1; \
    \
    for (; bmavg_bit(carry_bmp, pos); pos++) { \
        /* Ensure we don't access out of bounds if carry logic goes wrong */ \
        if (pos >= HIST_COUNT) break; \
        \
        { \
            bmavg_uint(BITLEN) other = bmavg->hist[pos]; \
            bmavg_uint(BITLEN) mid, rem; \
            bmavg->hist[pos] = 0; \
            mid = (carried & other) + ((carried ^ other) >> 1); \
            rem = (carried ^ other) & 1; \
            carried = mid + (mid & rem); \
        } \
    } \
    \
    if (unlikely(full) && pos < HIST_COUNT) { \
        bmavg_uint(BITLEN) other = bmavg->hist[pos]; \
        bmavg_uint(BITLEN) mid = (carried & other) + ((carried ^ other) >> 1); \
        bmavg_uint(BITLEN) rem = (carried ^ other) & 1; \
        carried = mid + (mid & rem); \
    } \
    \
    if (likely(pos < HIST_COUNT)) \
        bmavg->hist[pos] = carried; \
    \
    bmavg->count++; \
    if (unlikely(should_halve)) \
        bmavg->count = hmask_val; \
    \
    spin_unlock_irqrestore(&bmavg->lock, flags); \
} \
\
bmavg_uint(BITLEN) bmavg_read_u##BITLEN(struct bmavg_u##BITLEN *bmavg) { \
    bmavg_dbl_uint(BITLEN) total = 0; \
    bmavg_uint(BITLEN) cnt_snapshot; \
    int pos; \
    unsigned long flags; \
    \
    /* Lock to ensure consistent snapshot of hist array and count */ \
    spin_lock_irqsave(&bmavg->lock, flags); \
    cnt_snapshot = bmavg->count; \
    \
    if (unlikely(!cnt_snapshot)) { \
        spin_unlock_irqrestore(&bmavg->lock, flags); \
        return 0; \
    } \
    \
    pos = bmavg_fls_##BITLEN(cnt_snapshot) - 1; \
    if (pos > bmavg->limit_bitlen) \
        pos = bmavg->limit_bitlen; \
    \
    /* Clamp pos to HIST_COUNT to be safe */ \
    if (pos >= HIST_COUNT) pos = HIST_COUNT - 1; \
    \
    for (; pos >= 0; pos--) { \
        if (bmavg_bit(cnt_snapshot, pos)) { \
            /* Cast to double width BEFORE shifting to avoid overflow */ \
            total += (bmavg_dbl_uint(BITLEN))bmavg->hist[pos] << pos; \
        } \
    } \
    spin_unlock_irqrestore(&bmavg->lock, flags); \
    \
    return (bmavg_uint(BITLEN))(bmavg_do_div_##BITLEN(total, cnt_snapshot)); \
} \
\
void bmavg_init_u##BITLEN(struct bmavg_u##BITLEN *bmavg) { \
    int pos; \
    spin_lock_init(&bmavg->lock); \
    for (pos = 0; pos < HIST_COUNT; pos++) \
        bmavg->hist[pos] = 0; \
    bmavg->limit_bitlen = HIST_COUNT; \
    bmavg->count = 0; \
} \
\
void bmavg_reset_u##BITLEN(struct bmavg_u##BITLEN *bmavg) { \
    int pos; \
    unsigned long flags; \
    \
    spin_lock_irqsave(&bmavg->lock, flags); \
    \
    for (pos = 0; pos < HIST_COUNT; pos++) \
        bmavg->hist[pos] = 0; \
    \
    bmavg->count = 0; \
    /* 注意：reset 函数不重置 limit_bitlen，保留用户的配置 */ \
    \
    spin_unlock_irqrestore(&bmavg->lock, flags); \
} \
\
void bmavg_set_limit_u##BITLEN(struct bmavg_u##BITLEN *bmavg, int limit) { \
    int pos; \
    unsigned long flags; \
    int max_bits = sizeof(bmavg_uint(BITLEN)) * 8; \
    \
    /* Bounds checking */ \
    if (limit > HIST_COUNT) limit = HIST_COUNT; \
    if (limit > max_bits) limit = max_bits; \
    if (limit < 1) limit = 1; \
    \
    spin_lock_irqsave(&bmavg->lock, flags); \
    \
    for (pos = limit; pos < HIST_COUNT; pos++) \
        bmavg->hist[pos] = 0; \
    \
    bmavg->limit_bitlen = limit; \
    \
    /* Mask count safely: if limit is max_bits, mask should be -1 */ \
    bmavg->count &= (bmavg_uint(BITLEN))(bmavg_pow2(bmavg->count, limit) - 1); \
    \
    spin_unlock_irqrestore(&bmavg->lock, flags); \
}

/* Instantiate the variants */
DEFINE_BMAVG( 8,  5)
DEFINE_BMAVG(16, 12)
DEFINE_BMAVG(32, 28)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Masahito Suzuki");
MODULE_DESCRIPTION("Binary-Merge Average Filter");
