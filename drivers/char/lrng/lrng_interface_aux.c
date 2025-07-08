// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * LRNG auxiliary interfaces
 *
 * Copyright (C) 2022 Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2017 Jason A. Donenfeld <Jason@zx2c4.com>. All
 * Rights Reserved.
 * Copyright (C) 2016 Jason Cooper <jason@lakedaemon.net>
 */

#include <linux/lrng.h>
#include <linux/mm.h>
#include <linux/random.h>

#include "lrng_es_mgr.h"
#include "lrng_interface_random_kernel.h"

/*
 * Fill a buffer with random numbers and tokenize it to provide random numbers
 * to callers in fixed chunks. This approach is provided to be consistent with
 * the Linux kernel interface requirements. Yet, this approach violate the
 * backtracking resistance of the random number generator. Thus, the provided
 * random numbers are not considered to be as strong as those requested directly
 * from the LRNG.
 */
struct batched_entropy {
	union {
		u64 entropy_u64[LRNG_DRNG_BLOCKSIZE / sizeof(u64)];
		u32 entropy_u32[LRNG_DRNG_BLOCKSIZE / sizeof(u32)];
		u16 entropy_u16[LRNG_DRNG_BLOCKSIZE / sizeof(u16)];
		u8 entropy_u8[LRNG_DRNG_BLOCKSIZE / sizeof(u8)];
	};
	unsigned int position;
	spinlock_t batch_lock;
};

/*
 * Get a random word for internal kernel use only. The quality of the random
 * number is as good as /dev/urandom, but there is no backtrack protection,
 * with the goal of being quite fast and not depleting entropy.
 */
static DEFINE_PER_CPU(struct batched_entropy, batched_entropy_u64) = {
	.batch_lock	= __SPIN_LOCK_UNLOCKED(batched_entropy_u64.lock),
};

u64 get_random_u64(void)
{
	u64 ret;
	unsigned long flags;
	struct batched_entropy *batch;

	lrng_debug_report_seedlevel("get_random_u64");

	batch = raw_cpu_ptr(&batched_entropy_u64);
	spin_lock_irqsave(&batch->batch_lock, flags);
	if (batch->position % ARRAY_SIZE(batch->entropy_u64) == 0) {
		lrng_get_random_bytes(batch->entropy_u64, LRNG_DRNG_BLOCKSIZE);
		batch->position = 0;
	}
	ret = batch->entropy_u64[batch->position++];
	spin_unlock_irqrestore(&batch->batch_lock, flags);
	return ret;
}
EXPORT_SYMBOL(get_random_u64);

static DEFINE_PER_CPU(struct batched_entropy, batched_entropy_u32) = {
	.batch_lock	= __SPIN_LOCK_UNLOCKED(batched_entropy_u32.lock),
};

u32 get_random_u32(void)
{
	u32 ret;
	unsigned long flags;
	struct batched_entropy *batch;

	lrng_debug_report_seedlevel("get_random_u32");

	batch = raw_cpu_ptr(&batched_entropy_u32);
	spin_lock_irqsave(&batch->batch_lock, flags);
	if (batch->position % ARRAY_SIZE(batch->entropy_u32) == 0) {
		lrng_get_random_bytes(batch->entropy_u32, LRNG_DRNG_BLOCKSIZE);
		batch->position = 0;
	}
	ret = batch->entropy_u32[batch->position++];
	spin_unlock_irqrestore(&batch->batch_lock, flags);
	return ret;
}
EXPORT_SYMBOL(get_random_u32);

static DEFINE_PER_CPU(struct batched_entropy, batched_entropy_u16) = {
	.batch_lock	= __SPIN_LOCK_UNLOCKED(batched_entropy_u16.lock),
};

u16 get_random_u16(void)
{
	u16 ret;
	unsigned long flags;
	struct batched_entropy *batch;

	lrng_debug_report_seedlevel("get_random_u16");

	batch = raw_cpu_ptr(&batched_entropy_u16);
	spin_lock_irqsave(&batch->batch_lock, flags);
	if (batch->position % ARRAY_SIZE(batch->entropy_u16) == 0) {
		lrng_get_random_bytes(batch->entropy_u16, LRNG_DRNG_BLOCKSIZE);
		batch->position = 0;
	}
	ret = batch->entropy_u16[batch->position++];
	spin_unlock_irqrestore(&batch->batch_lock, flags);
	return ret;
}
EXPORT_SYMBOL(get_random_u16);

static DEFINE_PER_CPU(struct batched_entropy, batched_entropy_u8) = {
	.batch_lock	= __SPIN_LOCK_UNLOCKED(batched_entropy_u8.lock),
};

u8 get_random_u8(void)
{
	u8 ret;
	unsigned long flags;
	struct batched_entropy *batch;

	lrng_debug_report_seedlevel("get_random_u8");

	batch = raw_cpu_ptr(&batched_entropy_u8);
	spin_lock_irqsave(&batch->batch_lock, flags);
	if (batch->position % ARRAY_SIZE(batch->entropy_u8) == 0) {
		lrng_get_random_bytes(batch->entropy_u8, LRNG_DRNG_BLOCKSIZE);
		batch->position = 0;
	}
	ret = batch->entropy_u8[batch->position++];
	spin_unlock_irqrestore(&batch->batch_lock, flags);
	return ret;
}
EXPORT_SYMBOL(get_random_u8);

/* Taken directly from random.c */
u32 __get_random_u32_below(u32 ceil)
{
	u64 mult = (u64)ceil * get_random_u32();

	if (unlikely((u32)mult < ceil)) {
		u32 bound = -ceil % ceil;
		while (unlikely((u32)mult < bound))
			mult = (u64)ceil * get_random_u32();
	}
	return mult >> 32;
}
EXPORT_SYMBOL(__get_random_u32_below);

#ifdef CONFIG_SMP
/*
 * This function is called when the CPU is coming up, with entry
 * CPUHP_RANDOM_PREPARE, which comes before CPUHP_WORKQUEUE_PREP.
 */
int random_prepare_cpu(unsigned int cpu)
{
	/*
	 * When the cpu comes back online, immediately invalidate all batches,
	 * so that we serve fresh randomness.
	 */
	per_cpu_ptr(&batched_entropy_u8, cpu)->position = 0;
	per_cpu_ptr(&batched_entropy_u16, cpu)->position = 0;
	per_cpu_ptr(&batched_entropy_u32, cpu)->position = 0;
	per_cpu_ptr(&batched_entropy_u64, cpu)->position = 0;
	return 0;
}

int random_online_cpu(unsigned int cpu)
{
	return 0;
}
#endif

/*
 * It's important to invalidate all potential batched entropy that might
 * be stored before the crng is initialized, which we can do lazily by
 * simply resetting the counter to zero so that it's re-extracted on the
 * next usage.
 */
void invalidate_batched_entropy(void)
{
	int cpu;
	unsigned long flags;

	for_each_possible_cpu(cpu) {
		struct batched_entropy *batched_entropy;

		batched_entropy = per_cpu_ptr(&batched_entropy_u8, cpu);
		spin_lock_irqsave(&batched_entropy->batch_lock, flags);
		batched_entropy->position = 0;
		spin_unlock_irqrestore(&batched_entropy->batch_lock, flags);

		batched_entropy = per_cpu_ptr(&batched_entropy_u16, cpu);
		spin_lock_irqsave(&batched_entropy->batch_lock, flags);
		batched_entropy->position = 0;
		spin_unlock_irqrestore(&batched_entropy->batch_lock, flags);

		batched_entropy = per_cpu_ptr(&batched_entropy_u32, cpu);
		spin_lock_irqsave(&batched_entropy->batch_lock, flags);
		batched_entropy->position = 0;
		spin_unlock_irqrestore(&batched_entropy->batch_lock, flags);

		batched_entropy = per_cpu_ptr(&batched_entropy_u64, cpu);
		spin_lock(&batched_entropy->batch_lock);
		batched_entropy->position = 0;
		spin_unlock_irqrestore(&batched_entropy->batch_lock, flags);
	}
}
