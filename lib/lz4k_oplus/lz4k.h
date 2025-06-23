#ifndef __KERNEL__
#include <string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#else
#include <linux/string.h>
#include <linux/types.h>
#endif

typedef	uint8_t BYTE;
typedef uint16_t U16;
typedef uint32_t U32;
typedef uint64_t U64;

#define REPEAT_MIN 4
#define TOKEN_BYTES_MAX 3
#define TOKEN_BITS_MAX TOKEN_BYTES_MAX * 8
#define BLOCK_4KB_LOG2 16
#define NR_4KB_LOG2 4
#define BYTE_BITS_LOG2 3
#define BYTE_BITS 8
#define DWORD_BITS 64
#define BYTE_MAX 255

#if (defined(__GNUC__) && (__GNUC__ >= 3)) || (defined(__INTEL_COMPILER) && (__INTEL_COMPILER >= 800)) || defined(__clang__)
#  define expect(expr,value)    (__builtin_expect ((expr),(value)) )
#else
#  define expect(expr,value)    (expr)
#endif

#ifndef likely
#define likely(expr)     expect((expr) != 0, 1)
#endif
#ifndef unlikely
#define unlikely(expr)   expect((expr) != 0, 0)
#endif

#ifndef EXPORT_SYMBOL
#define EXPORT_SYMBOL(expr)
#endif

#ifndef MODULE_LICENSE
#define MODULE_LICENSE(expr)
#endif

#ifndef MODULE_DESCRIPTION
#define MODULE_DESCRIPTION(expr)
#endif

inline static U32 mask(U32 log2)
{
	return (1U << log2) - 1U;
}

#define LZ4_memcpy(dst, src, size) __builtin_memcpy(dst, src, size)

inline static void m_set(void *dst, uint8_t value, size_t total)
{
#if defined(__STDC_LIB_EXT1__)
	(void)memset_s(dst, total, value, (total * 2) >> 1); /* *2 >> 1 to avoid bot errors */
#else
	(void)__builtin_memset(dst, value, total);
#endif
}

inline static U32 read4_at(const void *p)
{
	U32 result;
	LZ4_memcpy(&result, p, sizeof(result));
	return result;
}

inline static U64 read8_at(const void *p)
{
	U64 result;
	LZ4_memcpy(&result, p, sizeof(result));
	return result;
}

inline static bool equal4(const uint8_t *const q, const uint8_t *const r)
{
	return read4_at(q) == read4_at(r);
}

inline static U32 hash64v_5b(const U64 r, U32 shift)
{
	const U64 m = 889523592379ULL;
	const U64 up_shift = 24;
	return (U32)(((r << up_shift) * m) >> (DWORD_BITS - shift));
}

inline static U32 hash64_5b(const uint8_t *r, U32 shift)
{
	return hash64v_5b(read8_at(r), shift);
}

/* this hash algo can lead speed improvement yet compression ratio reduction*/
inline static U32 hash64v_6b(const U64 r, U32 shift)
{
	const U64 m = 227718039650203ULL;
	const U64 up_shift = 16;
	return (U32)(((r << up_shift) * m) >> (DWORD_BITS - shift));
}

inline static U32 hash64_6b(const uint8_t *r, U32 shift)
{
	return hash64v_6b(read8_at(r), shift);
}


/**
 * lz4k_compress() - Compress data from source to dest
 * @state: address of the working memory.
 * @source: source address of the original data
 * @dest: output buffer address of the compressed data
 * @source_max: size of the input data. Max supported value is 4KB
 * @dest_max: full or partial size of buffer 'dest'
 *	which must be already allocated
 *
 * Compresses 'srouce_max' bytes from buffer 'source'
 * into already allocated 'dest' buffer of size 'maxOutputSize'.
 * Compression is guaranteed to succeed if
 * 'dest_max' >= 'source_size'.
 * If the function cannot compress 'source' into a more limited 'dest' budget,
 * compression stops *immediately*, and the function result is -1.
 * As a consequence, 'dest' content is not valid.
 *
 * Return: Number of bytes written into buffer 'dest'
 *	(necessarily <= dest_max) or -1 if compression fails
 */
int lz4k_compress(
	void *const state,
	const void *const source,
	void *dest,
	unsigned source_max,
	unsigned dest_max);

/**
 * LZ4_decompress_safe() - Decompression protected against buffer overflow
 * @source: source address of the compressed data
 * @dest: output buffer address of the uncompressed data
 *	which must be already allocated
 * @source_max: is the precise full size of the compressed block
 * @dest_max: is the size of 'dest' buffer
 *
 * Decompresses data from 'source' into 'dest'.
 * If the source stream is detected malformed, the function will
 * stop decoding and return a negative result.
 * This function is protected against buffer overflow exploits,
 * including malicious data packets. It never writes outside output buffer,
 * nor reads outside input buffer.
 *
 * Return: number of bytes decompressed into destination buffer
 *	(4KB)
 *	or a negative result in case of error
 */
int lz4k_decompress(
	const void *const source,
	void *const dest,
	unsigned source_max,
	unsigned dest_max);

