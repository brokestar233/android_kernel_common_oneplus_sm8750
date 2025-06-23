#include "lz4k.h"

#define NR_COPY_LOG2 4
#define NR_COPY_MIN (1 << NR_COPY_LOG2)
#define HT_LOG2 12
#define STEP_LOG2 5


inline static const BYTE *hashed(
	const BYTE *const base,
	U16 *const dict,
	U32 h,
	const BYTE *r)
{
	const BYTE *q = base + dict[h];
	dict[h] = (U16)(r - base);
	return q;
}

inline static U32 size_bytes_count(U32 u)
{
	return ((u + BYTE_MAX) >> BYTE_BITS) + 1; /* (u + BYTE_MAX - 1) / BYTE_MAX; */
}

/* minimum compressd size for non-compressible data */
inline static U32 compressd_bytes_min(
	U32 nr_log2,
	U32 source_max)
{
	return source_max < mask(nr_log2) ?
		TOKEN_BYTES_MAX + source_max :
		TOKEN_BYTES_MAX + size_bytes_count(source_max - mask(nr_log2)) + source_max;
}

inline static void copy_x_while_total(
	uint8_t *dst,
	const uint8_t *src,
	size_t total,
	const size_t copy_min)
{
	LZ4_memcpy(dst, src, copy_min);
	for (; total > copy_min; total -= copy_min)
		LZ4_memcpy(dst += copy_min, src += copy_min, copy_min);
}

inline static void  update_token(
	U32 match_length,
	U32 *token,
	const U32 nr_log2,
	const U32 off_log2)
{
	const U32 r_mask = mask(TOKEN_BITS_MAX - (off_log2 + nr_log2));
	*token |= likely(match_length - REPEAT_MIN < r_mask) ?
		 ((match_length - REPEAT_MIN) << off_log2) : (r_mask << off_log2);
}

inline static BYTE *dest_size_bytes(BYTE *dest_at, U32 u)
{
	for (; u >= BYTE_MAX; *dest_at++ = (BYTE)BYTE_MAX, u -= BYTE_MAX);
	*dest_at++ = (BYTE)u;
	return dest_at;
}

inline static BYTE *dest_token_then_bytes_left(
	BYTE *dest_at,
	U32 token,
	U32 bytes_left)
{
	LZ4_memcpy(dest_at, &token, TOKEN_BYTES_MAX);
	return dest_size_bytes(dest_at + TOKEN_BYTES_MAX, bytes_left);
}

static int dest_tail(
	BYTE *dest_at,
	BYTE *const dest_end,
	const BYTE *const dest,
	const BYTE *const nr0,
	const BYTE *const source_end,
	const U32 nr_log2,
	const U32 off_log2)
{
	const U32 nr_mask = mask(nr_log2);
	const U32 r_log2 = TOKEN_BITS_MAX - (off_log2 + nr_log2);
	const U32 nr_bytes_literal = (U32)(source_end - nr0);
	/* check if there is enough space for uncompressed data */
	if (compressd_bytes_min(nr_log2, nr_bytes_literal) > (U32)(dest_end - dest_at))
		return -1;
	if (nr_bytes_literal < nr_mask) {
		/* caller guarantees at least one nr-byte */
		U32 token = (nr_bytes_literal << (off_log2 + r_log2));
		LZ4_memcpy(dest_at, &token, TOKEN_BYTES_MAX);
		dest_at += TOKEN_BYTES_MAX;
	} else { /* nr_bytes_literal>=nr_mask */
		U32 bytes_left = nr_bytes_literal - nr_mask;
		U32 token = (nr_mask << (off_log2 + r_log2));
		dest_at = dest_token_then_bytes_left(dest_at, token, bytes_left);
	} /* if (nr_bytes_literal<nr_mask) */
	LZ4_memcpy(dest_at, nr0, nr_bytes_literal);
	return (int)(dest_at + nr_bytes_literal - dest);
}

inline static int dest_tail2(
	BYTE *dest_at,
	BYTE *const dest_end,
	const BYTE *const dest,
	const BYTE *const r,
	const BYTE *const source_end,
	const U32 nr_log2,
	const U32 off_log2)
{
	return r == source_end ? (int)(dest_at - dest) :
		dest_tail(dest_at, dest_end, dest, r, source_end,
			 nr_log2, off_log2);
}


static BYTE *dest_non_repeat(
	BYTE *dest_at,
	BYTE *const dest_end,
	U32 token,
	const BYTE *const nr0,
	const BYTE *const r,
	const U32 nr_log2,
	const U32 off_log2)
{
	const U32 lit_length = (U32)(r - nr0);
	const U32 nr_mask = mask(nr_log2),
		r_log2 = TOKEN_BITS_MAX - (off_log2 + nr_log2);
	if (likely(lit_length < nr_mask)) {
		token |= (lit_length << (off_log2 + r_log2));
		LZ4_memcpy(dest_at, &token, TOKEN_BYTES_MAX);
		dest_at += TOKEN_BYTES_MAX;
	} else { /* lit_length >= nr_mask */
		U32 bytes_left = lit_length - nr_mask;
		token |= (nr_mask << (off_log2 + r_log2));
		dest_at = dest_token_then_bytes_left(dest_at, token, bytes_left);
	} /* if (lit_length<nr_mask) */
	copy_x_while_total(dest_at, nr0, lit_length, NR_COPY_MIN);
	dest_at += lit_length;
	return dest_at;
}

inline static BYTE *dest_r_bytes_left(
	BYTE *dest_at,
	U32 match_length,
	const U32 nr_log2,
	const U32 off_log2)
{
	const U32 r_mask = mask(TOKEN_BITS_MAX - (off_log2 + nr_log2));
	return likely(match_length - REPEAT_MIN < r_mask) ?
		dest_at : dest_size_bytes(dest_at, match_length - REPEAT_MIN - r_mask);
}

static BYTE *dest_repeat(
	BYTE *dest_at,
	U32 token,
	U32 match_length,
	const U32 nr_log2,
	const U32 off_log2)
{
	const U32 r_mask = mask(TOKEN_BITS_MAX - (off_log2 + nr_log2));
	if (likely(match_length - REPEAT_MIN < r_mask)) {
		token |= ((match_length - REPEAT_MIN) << off_log2);
		LZ4_memcpy(dest_at, &token, TOKEN_BYTES_MAX);
		dest_at += TOKEN_BYTES_MAX;
	} else {
		U32 bytes_left = match_length - REPEAT_MIN - r_mask;
		token |= (r_mask << off_log2);
		dest_at = dest_token_then_bytes_left(dest_at, token, bytes_left);
	}
	return dest_at;
}

inline static BYTE *dest_tuple(
	BYTE *dest_at,
	BYTE *const dest_end,
	U32 token,
	const BYTE *const nr0,
	const BYTE *const r,
	U32 match_length,
	const U32 nr_log2,
	const U32 off_log2)
{
	update_token(match_length, &token, nr_log2, off_log2);
	dest_at = dest_non_repeat(dest_at, dest_end, token, nr0, r, nr_log2, off_log2);
	return dest_r_bytes_left(dest_at, match_length, nr_log2, off_log2);
}

static const BYTE *repeat_end(
	const BYTE *q,
	const BYTE *r,
	const BYTE *const source_end_safe,
	const BYTE *const source_end)
{
	q += REPEAT_MIN;
	r += REPEAT_MIN;
	/* caller guarantees r+12<=in_end */
	do {
		const U64 x = read8_at(q) ^ read8_at(r);
		if (x) {
			const U16 ctz = (U16)__builtin_ctzl(x);
			return r + (ctz >> BYTE_BITS_LOG2);
		}
		/* some bytes differ: count of trailing 0-bits/bytes */
		q += sizeof(U64);
		r += sizeof(U64);
	} while (likely(r <= source_end_safe)); /* once, at input block end */
	while (r < source_end) {
		if (*q != *r) return r;
		++q;
		++r;
	}
	return r;
}

inline static U32 hash(const BYTE *r)
{
	return hash64_5b(r, HT_LOG2);
}

static int compress_64k(
	U16 *const dict,
	const BYTE *const base,
	const BYTE *const source_end,
	BYTE *const dest,
	BYTE *const dest_end)
{
	enum {
		NR_LOG2 = NR_4KB_LOG2,
		OFF_LOG2 = BLOCK_4KB_LOG2
	};
	const BYTE *const source_end_safe = source_end - NR_COPY_MIN;
	const BYTE *r = base;
	const BYTE *nr0 = r++;
	BYTE *dest_at = dest;
	for (; ; nr0 = r) {
		const BYTE *q = 0;
		U32 step = 1 << STEP_LOG2;
		U32 token = 0;
		const BYTE *r_end = 0;
		U32 match_length = 0;
		while (true) {
			if (equal4(q = hashed(base, dict, hash(r), r), r))
				break;
			++r;
			if (equal4(q = hashed(base, dict, hash(r), r), r))
				break;
			if (unlikely((r += (++step >> STEP_LOG2)) > source_end_safe))
				return dest_tail(dest_at, dest_end, dest, nr0, source_end,
						NR_LOG2, OFF_LOG2);
		}
		/* first store the offset */
		token = (U32)(r - q);
		r_end = repeat_end(q, r, source_end_safe, source_end);
		match_length = (U32)(r_end - r);
		if (unlikely(nr0 == r))
			dest_at = dest_repeat(dest_at, token, match_length,
					    NR_LOG2, OFF_LOG2);
		else
			dest_at = dest_tuple(dest_at, dest_end, token, nr0, r, match_length,
					    NR_LOG2, OFF_LOG2);
		if (unlikely((r += match_length) > source_end_safe))
			return dest_tail2(dest_at, dest_end, dest, r, source_end,
					 NR_LOG2, OFF_LOG2);
		/* update r-1 every iters, no need to worry about overflows since r >= 1 */
		dict[hash(r - 1)] = (U16)(r - 1 - base);
	}
}

int lz4k_compress(
	void *const state,
	const void *const source,
	void *dest,
	unsigned source_max,
	unsigned dest_max)
{
	m_set(state, 0, 1U << (HT_LOG2+1));
	*((BYTE*)dest) = 0;
	return compress_64k((U16*)state, (const BYTE*)source,
			(const BYTE*)source + source_max, (BYTE*)dest, (BYTE*)dest + dest_max);
}
EXPORT_SYMBOL(lz4k_compress);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("LZ4K compressr");
