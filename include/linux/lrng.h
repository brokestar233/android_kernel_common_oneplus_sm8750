/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright (C) 2022, Stephan Mueller <smueller@chronox.de>
 */

#ifndef _LRNG_H
#define _LRNG_H

#include <crypto/hash.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/types.h>

/*
 * struct lrng_drng_cb - cryptographic callback functions defining a DRNG
 * @drng_name		Name of DRNG
 * @drng_alloc:		Allocate DRNG -- the provided integer should be used for
 *			sanity checks.
 *			return: allocated data structure or PTR_ERR on error
 * @drng_dealloc:	Deallocate DRNG
 * @drng_seed:		Seed the DRNG with data of arbitrary length drng: is
 *			pointer to data structure allocated with drng_alloc
 *			return: >= 0 on success, < 0 on error
 * @drng_generate:	Generate random numbers from the DRNG with arbitrary
 *			length
 */
struct lrng_drng_cb {
	const char *(*drng_name)(void);
	void *(*drng_alloc)(u32 sec_strength);
	void (*drng_dealloc)(void *drng);
	int (*drng_seed)(void *drng, const u8 *inbuf, u32 inbuflen);
	int (*drng_generate)(void *drng, u8 *outbuf, u32 outbuflen);
};

/*
 * struct lrng_hash_cb - cryptographic callback functions defining a hash
 * @hash_name		Name of Hash used for reading entropy pool arbitrary
 *			length
 * @hash_alloc:		Allocate the hash for reading the entropy pool
 *			return: allocated data structure (NULL is success too)
 *				or ERR_PTR on error
 * @hash_dealloc:	Deallocate Hash
 * @hash_digestsize:	Return the digestsize for the used hash to read out
 *			entropy pool
 *			hash: is pointer to data structure allocated with
 *			      hash_alloc
 *			return: size of digest of hash in bytes
 * @hash_init:		Initialize hash
 *			hash: is pointer to data structure allocated with
 *			      hash_alloc
 *			return: 0 on success, < 0 on error
 * @hash_update:	Update hash operation
 *			hash: is pointer to data structure allocated with
 *			      hash_alloc
 *			return: 0 on success, < 0 on error
 * @hash_final		Final hash operation
 *			hash: is pointer to data structure allocated with
 *			      hash_alloc
 *			return: 0 on success, < 0 on error
 * @hash_desc_zero	Zeroization of hash state buffer
 *
 * Assumptions:
 *
 * 1. Hash operation will not sleep
 * 2. The hash' volatile state information is provided with *shash by caller.
 */
struct lrng_hash_cb {
	const char *(*hash_name)(void);
	void *(*hash_alloc)(void);
	void (*hash_dealloc)(void *hash);
	u32 (*hash_digestsize)(void *hash);
	int (*hash_init)(struct shash_desc *shash, void *hash);
	int (*hash_update)(struct shash_desc *shash, const u8 *inbuf,
			   u32 inbuflen);
	int (*hash_final)(struct shash_desc *shash, u8 *digest);
	void (*hash_desc_zero)(struct shash_desc *shash);
};

/* Register cryptographic backend */
#ifdef CONFIG_LRNG_SWITCH
int lrng_set_drng_cb(const struct lrng_drng_cb *cb);
int lrng_set_hash_cb(const struct lrng_hash_cb *cb);
#else	/* CONFIG_LRNG_SWITCH */
static inline int
lrng_set_drng_cb(const struct lrng_drng_cb *cb) { return -EOPNOTSUPP; }
static inline int
lrng_set_hash_cb(const struct lrng_hash_cb *cb) { return -EOPNOTSUPP; }
#endif	/* CONFIG_LRNG_SWITCH */

/* Callback to feed events to the scheduler entropy source */
#ifdef CONFIG_LRNG_SCHED
extern void add_sched_randomness(const struct task_struct *p, int cpu);
#else
static inline void
add_sched_randomness(const struct task_struct *p, int cpu) { }
#endif

/*
 * lrng_get_random_bytes() - Provider of cryptographic strong random numbers
 * for kernel-internal usage.
 *
 * This function is appropriate for in-kernel use cases operating in atomic
 * contexts. It will always use the ChaCha20 DRNG and it may be the case that
 * it is not fully seeded when being used.
 *
 * @buf: buffer to store the random bytes
 * @nbytes: size of the buffer
 */
#ifdef CONFIG_LRNG_DRNG_ATOMIC
void lrng_get_random_bytes(void *buf, int nbytes);
#endif

/*
 * lrng_get_random_bytes_full() - Provider of cryptographic strong
 * random numbers for kernel-internal usage from a fully initialized LRNG.
 *
 * This function will always return random numbers from a fully seeded and
 * fully initialized LRNG.
 *
 * This function is appropriate only for non-atomic use cases as this
 * function may sleep. It provides access to the full functionality of LRNG
 * including the switchable DRNG support, that may support other DRNGs such
 * as the SP800-90A DRBG.
 *
 * @buf: buffer to store the random bytes
 * @nbytes: size of the buffer
 */
#ifdef CONFIG_LRNG
void lrng_get_random_bytes_full(void *buf, int nbytes);
#endif

/*
 * lrng_get_random_bytes_min() - Provider of cryptographic strong
 * random numbers for kernel-internal usage from at least a minimally seeded
 * LRNG, which is not necessarily fully initialized yet (e.g. SP800-90C
 * oversampling applied in FIPS mode is not applied yet).
 *
 * This function is appropriate only for non-atomic use cases as this
 * function may sleep. It provides access to the full functionality of LRNG
 * including the switchable DRNG support, that may support other DRNGs such
 * as the SP800-90A DRBG.
 *
 * @buf: buffer to store the random bytes
 * @nbytes: size of the buffer
 */
#ifdef CONFIG_LRNG
void lrng_get_random_bytes_min(void *buf, int nbytes);
#endif

/*
 * lrng_get_random_bytes_pr() - Provider of cryptographic strong
 * random numbers for kernel-internal usage from a fully initialized LRNG and
 * requiring a reseed from the entropy sources before.
 *
 * This function will always return random numbers from a fully seeded and
 * fully initialized LRNG.
 *
 * This function is appropriate only for non-atomic use cases as this
 * function may sleep. It provides access to the full functionality of LRNG
 * including the switchable DRNG support, that may support other DRNGs such
 * as the SP800-90A DRBG.
 *
 * This call only returns no more data than entropy was pulled from the
 * entropy sources. Thus, it is likely that this call returns less data
 * than requested by the caller. Also, the caller MUST be prepared that this
 * call returns 0 bytes, i.e. it did not generate data.
 *
 * @buf: buffer to store the random bytes
 * @nbytes: size of the buffer
 *
 * @return: positive number indicates amount of generated bytes, < 0 on error
 */
#ifdef CONFIG_LRNG
int lrng_get_random_bytes_pr(void *buf, int nbytes);
#endif

/*
 * lrng_get_seed() - Fill buffer with data from entropy sources
 *
 * This call allows accessing the entropy sources directly and fill the buffer
 * with data from all available entropy sources. This filled buffer is
 * identical to the temporary seed buffer used by the LRNG to seed its DRNGs.
 *
 * The call is to allows users to seed their DRNG directly from the entropy
 * sources in case the caller does not want to use the LRNG's DRNGs. This
 * buffer can be directly used to seed the caller's DRNG from.
 *
 * The call blocks as long as one LRNG DRNG is not yet fully seeded. If
 * LRNG_GET_SEED_NONBLOCK is specified, it does not block in this case, but
 * returns with an error.
 *
 * Considering SP800-90C, there is a differentiation between the seeding
 * requirements during instantiating a DRNG and at runtime of the DRNG. When
 * specifying LRNG_GET_SEED_FULLY_SEEDED the caller indicates the DRNG was
 * already fully seeded and the regular amount of entropy is requested.
 * Otherwise, the LRNG will obtain the entropy rate required for initial
 * seeding. The following minimum entropy rates will be obtained:
 *
 * * FIPS mode:
 *	* Initial seeding: 384 bits of entropy
 *	* Runtime seeding: 256 bits of entropy
 * * Non-FIPS mode:
 *	* 128 bits of entropy in any case
 *
 * Albeit these are minimum entropy rates, the LRNG tries to request the
 * given amount of entropy from each entropy source individually. If the
 * minimum amount of entropy cannot be obtained collectively by all entropy
 * sources, the LRNG will not fill the buffer.
 *
 * The return data in buf is structurally equivalent to the following
 * definition:
 *
 * struct {
 *	u64 seedlen;
 *	u64 entropy_rate;
 *	struct entropy_buf seed;
 * } __attribute((__packed__));
 *
 * As struct entropy_buf is not known outsize of the LRNG, the LRNG fills
 * seedlen first with the size of struct entropy_buf. If the caller-provided
 * buffer buf is smaller than u64, then -EINVAL is returned
 * and buf is not touched. If it is u64 or larger but smaller
 * than the size of the structure above, -EMSGSIZE is returned and seedlen
 * is filled with the size of the buffer. Finally, if buf is large
 * enough to hold all data, it is filled with the seed data and the seedlen
 * is set to sizeof(struct entropy_buf). The entropy rate is returned with
 * the variable entropy_rate and provides the value in bits.
 *
 * The seed buffer is the data that should be handed to the caller's DRNG as
 * seed data.
 *
 * @buf [out] Buffer to be filled with data from the entropy sources - note, the
 *	      buffer is marked as u64 to ensure it is aligned to 64 bits.
 * @nbytes [in] Size of the buffer allocated by the caller - this value
 *		provides size of @param buf in bytes.
 * @flags [in] Flags field to adjust the behavior
 *
 * @return -EINVAL or -EMSGSIZE indicating the buffer is too small, -EAGAIN when
 *	   the call would block, but NONBLOCK is specified, > 0 the size of
 *	   the filled buffer.
 */
#ifdef CONFIG_LRNG
enum lrng_get_seed_flags {
	LRNG_GET_SEED_NONBLOCK = 0x0001, /**< Do not block the call */
	LRNG_GET_SEED_FULLY_SEEDED = 0x0002, /**< DRNG is fully seeded */
};

ssize_t lrng_get_seed(u64 *buf, size_t nbytes, unsigned int flags);
#endif

#endif /* _LRNG_H */
