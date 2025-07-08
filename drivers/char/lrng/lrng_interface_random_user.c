// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * LRNG Common user space interfaces compliant to random(4), random(7) and
 * getrandom(2) man pages.
 *
 * Copyright (C) 2022, Stephan Mueller <smueller@chronox.de>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/random.h>
#include <linux/syscalls.h>

#include "lrng_es_mgr.h"
#include "lrng_interface_dev_common.h"

static ssize_t lrng_drng_read(struct file *file, char __user *buf,
			      size_t nbytes, loff_t *ppos)
{
	if (!lrng_state_min_seeded())
		pr_notice_ratelimited("%s - use of insufficiently seeded DRNG (%zu bytes read)\n",
				      current->comm, nbytes);
	else if (!lrng_state_operational())
		pr_debug_ratelimited("%s - use of not fully seeded DRNG (%zu bytes read)\n",
				     current->comm, nbytes);

	return lrng_read_common(buf, nbytes, false);
}

const struct file_operations random_fops = {
	.read  = lrng_drng_read_block,
	.write = lrng_drng_write,
	.poll  = lrng_random_poll,
	.unlocked_ioctl = lrng_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.fasync = lrng_fasync,
	.llseek = noop_llseek,
};

const struct file_operations urandom_fops = {
	.read  = lrng_drng_read,
	.write = lrng_drng_write,
	.unlocked_ioctl = lrng_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.fasync = lrng_fasync,
	.llseek = noop_llseek,
};

/*
 * GRND_SEED
 *
 * This flag requests to provide the data directly from the entropy sources.
 *
 * The behavior of the call is exactly as outlined for the function
 * lrng_get_seed in lrng.h.
 */
#define GRND_SEED		0x0010

/*
 * GRND_FULLY_SEEDED
 *
 * This flag indicates whether the caller wants to reseed a DRNG that is already
 * fully seeded. See esdm_get_seed in lrng.h for details.
 */
#define GRND_FULLY_SEEDED	0x0020

SYSCALL_DEFINE3(getrandom, char __user *, buf, size_t, count,
		unsigned int, flags)
{
	if (flags & ~(GRND_NONBLOCK|GRND_RANDOM|GRND_INSECURE|
		      GRND_SEED|GRND_FULLY_SEEDED))
		return -EINVAL;

	/*
	 * Requesting insecure and blocking randomness at the same time makes
	 * no sense.
	 */
	if ((flags &
	     (GRND_INSECURE|GRND_RANDOM)) == (GRND_INSECURE|GRND_RANDOM))
		return -EINVAL;
	if ((flags &
	     (GRND_INSECURE|GRND_SEED)) == (GRND_INSECURE|GRND_SEED))
		return -EINVAL;
	if ((flags &
	     (GRND_RANDOM|GRND_SEED)) == (GRND_RANDOM|GRND_SEED))
		return -EINVAL;

	if (count > INT_MAX)
		count = INT_MAX;

	if (flags & GRND_INSECURE) {
		return lrng_drng_read(NULL, buf, count, NULL);
	} else if (flags & GRND_SEED) {
		unsigned int seed_flags = (flags & GRND_NONBLOCK) ?
					  LRNG_GET_SEED_NONBLOCK : 0;

		seed_flags |= (flags & GRND_FULLY_SEEDED) ?
			      LRNG_GET_SEED_FULLY_SEEDED : 0;
		return lrng_read_seed(buf, count, seed_flags);
	}

	return lrng_read_common_block(flags & GRND_NONBLOCK,
				      flags & GRND_RANDOM, buf, count);
}
