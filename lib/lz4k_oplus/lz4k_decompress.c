#include "lz4k.h"
#define MASK_3B 0X00FFFFFF

static const BYTE *get_size(
	U32 *size,
	const BYTE *source_at,
	const BYTE *const source_end)
{
	U32 u;
	do {
		if (unlikely(source_at >= source_end))
			return NULL;
		*size += (u = *(const BYTE*)source_at);
		++source_at;
	} while (BYTE_MAX == u);
	return source_at;
}

inline static void while_lt_copy_x(
	BYTE *dst,
	const BYTE *src,
	const BYTE *dst_end,
	const size_t copy_min)
{
	for (; dst < dst_end; dst += copy_min, src += copy_min)
		LZ4_memcpy(dst, src, copy_min);
}

inline static void copy_x_while_lt(
	BYTE *dst,
	const BYTE *src,
	const BYTE *dst_end,
	const size_t copy_min)
{
	while (dst + copy_min < dst_end){
		LZ4_memcpy(dst += copy_min, src += copy_min, copy_min);
	}
}

inline static void copy_2x(
	BYTE *dst,
	const BYTE *src,
	const size_t copy_min)
{
	LZ4_memcpy(dst, src, copy_min);
	LZ4_memcpy(dst + copy_min, src + copy_min, copy_min);
}

inline static void copy_2x_as_x2_while_lt(
	BYTE *dst,
	const BYTE *src,
	const BYTE *dst_end,
	const size_t copy_min)
{
	copy_2x(dst, src, copy_min);
	while (dst + (copy_min << 1) < dst_end)
		copy_2x(dst += (copy_min << 1), src += (copy_min << 1), copy_min);
}

inline static void while_lt_copy_2x_as_x2(
	BYTE *dst,
	const BYTE *src,
	const BYTE *dst_end,
	const size_t copy_min)
{
	for (; dst < dst_end; dst += (copy_min << 1), src += (copy_min << 1))
		copy_2x(dst, src, copy_min);
}

static int end_of_block(
	const U32 lit_length,
	const U32 match_length,
	const BYTE *const source_at,
	const BYTE *const source_end,
	const BYTE *const dest,
	const BYTE *const dest_at)
{
	/* check if this is the last block */
	if (unlikely((!lit_length) || (source_at != source_end)
		|| (match_length != REPEAT_MIN)))
			return -1;
	return (int)(dest_at - dest);
}

enum {
	NR_COPY_MIN = 32,
	R_COPY_MIN = 16,
	R_COPY_SAFE = R_COPY_MIN - 1,
	R_COPY_SAFE_2X = (R_COPY_MIN << 1) - 1
};

static bool literal_decompress(
	const BYTE **source_at,
	BYTE **dest_at,
	U32 lit_length,
	const BYTE *const source_end,
	const BYTE *const dest_end)
{
	const BYTE *const source_copy_end = *source_at + lit_length;
	BYTE *const dest_copy_end = *dest_at + lit_length;
	/* literals to be copied are small */
	if (likely(lit_length <= NR_COPY_MIN)) {
		if (likely(*source_at <= source_end - NR_COPY_MIN))
			LZ4_memcpy(*dest_at, *source_at, NR_COPY_MIN);
		else if (source_copy_end <= source_end)
			LZ4_memcpy(*dest_at, *source_at, lit_length);
		else
			return false;

	} else { /* more literals need to be copied */
		/* check if there are enough space for copying without out of bounds access */
		if (likely(source_copy_end <= source_end - NR_COPY_MIN &&
			   dest_copy_end <= dest_end - NR_COPY_MIN)) {
			LZ4_memcpy(*dest_at, *source_at, NR_COPY_MIN);
			copy_x_while_lt(*dest_at,
					*source_at,
					dest_copy_end, NR_COPY_MIN);
			/* if (*dest_at + NR_COPY_MIN < dest_copy_end){
				LZ4_memcpy(*dest_at += NR_COPY_MIN, *source_at += NR_COPY_MIN, NR_COPY_MIN);
			} */
		} else if (source_copy_end <= source_end && dest_copy_end <= dest_end) {
			LZ4_memcpy(*dest_at, *source_at, lit_length);
		} else { /* source_copy_end > source_end || dest_copy_end > dest_end */
			return false;
		}
	} /* if (lit_length <= NR_COPY_MIN) */
	*source_at = source_copy_end;
	*dest_at = dest_copy_end;
	return true;
}

static void dest_repeat_overlap(
	U32 offset,
	BYTE *dest_at,
	const BYTE *dest_from,
	const BYTE *const dest_copy_end)
{
	enum {
		COPY_MIN = R_COPY_MIN >> 1,
		OFFSET_LIMIT = COPY_MIN >> 1
	};
	LZ4_memcpy(dest_at, dest_from, COPY_MIN);
/* (1 < offset < R_COPY_MIN/2) && dest_copy_end + R_COPY_SAFE_2X  <= dest_end */
	dest_at += offset;
	if (offset <= OFFSET_LIMIT)
		offset <<= 1;
	do {
		LZ4_memcpy(dest_at, dest_from, COPY_MIN);
		dest_at += offset;
		if (offset <= OFFSET_LIMIT)
			offset <<= 1;
	} while (dest_at - dest_from < R_COPY_MIN);
	while_lt_copy_2x_as_x2(dest_at, dest_from, dest_copy_end, R_COPY_MIN);
}

static bool dest_repeat_slow(
	U32 match_length,
	U32 offset,
	BYTE *dest_at,
	const BYTE *dest_from,
	const BYTE *const dest_copy_end,
	const BYTE *const dest_end)
{
	if (offset > 1 && dest_copy_end <= dest_end - R_COPY_SAFE_2X) {
		dest_repeat_overlap(offset, dest_at, dest_from, dest_copy_end);
	} else {
		if (unlikely(dest_copy_end > dest_end))
			return false;
		if (offset == 1) {
			m_set(dest_at, *dest_from, match_length);
		} else {
			do
				*dest_at++ = *dest_from++;
			while (dest_at < dest_copy_end);
		}
	}
	return true;
}

static int decompress(
	const BYTE *source_at,
	BYTE *const dest,
	const BYTE *const source_end,
	const BYTE *const dest_end,
	const U32 lit_log2,
	const U32 off_log2)
{
	const U32 match_log2 = TOKEN_BITS_MAX - (off_log2 + lit_log2);
	const BYTE *const source_end_minus_x = source_end - TOKEN_BYTES_MAX;
	BYTE *dest_at = dest;
	while (likely(source_at <= source_end_minus_x)) {
		const U32 token = (*(U32 *)(source_at)) & MASK_3B;
		const U32 offset = token & mask(off_log2);
		U32 lit_length = token >> (off_log2 + match_log2),
			      match_length = ((token >> off_log2) & mask(match_log2)) +
					    REPEAT_MIN;
		const BYTE *dest_from = 0;
		BYTE *dest_copy_end = 0;
		const BYTE *dest_safe_end = 0;
		source_at += TOKEN_BYTES_MAX;
		/* get literal length and decompress */
		if (unlikely(lit_length == mask(lit_log2))) {
			source_at = get_size(&lit_length, source_at, source_end);
		}
		if (!literal_decompress(&source_at, &dest_at, lit_length, source_end, dest_end))
			return -1;
		/* get match length and decompress */
		if (unlikely(match_length == mask(match_log2) + REPEAT_MIN)) {
			source_at = get_size(&match_length, source_at, source_end);
		}
		dest_from = dest_at - offset;
		if (unlikely(dest_from < dest))
			return -1;
		dest_copy_end = dest_at + match_length;
		dest_safe_end = dest_end - R_COPY_SAFE_2X;
		/* need offset >= R_COPY_MIN, since every time copy R_COPY_MIN Bytes */
		if (likely(offset >= R_COPY_MIN && dest_copy_end <= dest_safe_end)) {
			copy_2x_as_x2_while_lt(dest_at, dest_from, dest_copy_end,
					       R_COPY_MIN);
		} else if (likely(offset >= (R_COPY_MIN >> 1) &&
				  dest_copy_end <= dest_safe_end)) {
			LZ4_memcpy(dest_at, dest_from, R_COPY_MIN);
			dest_at += offset;
			while_lt_copy_x(dest_at, dest_from, dest_copy_end, R_COPY_MIN);
		} else if (likely(offset > 0)) {
			if (!dest_repeat_slow(match_length, offset, dest_at, dest_from,
			     dest_copy_end, dest_end))
				return -1;
		} else { /* offset == 0: EOB, last literal */
			return end_of_block(lit_length, match_length, source_at,
					    source_end, dest, dest_at);
		}
		dest_at = dest_copy_end;
	}
	return source_at == source_end ? (int)(dest_at - dest) : -1;
}

int lz4k_decompress(
	const void *source,
	void *const dest,
	unsigned source_max,
	unsigned dest_max)
{
	/* preventing compiler optimizations */
	const BYTE *volatile source_end = (const BYTE*)source + source_max;
	const BYTE *volatile dest_end = (BYTE*)dest + dest_max;

	return decompress((const BYTE*)source, (BYTE*)dest, source_end, dest_end,
			NR_4KB_LOG2, BLOCK_4KB_LOG2);
}
EXPORT_SYMBOL(lz4k_decompress);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("LZ4K decompressr");
