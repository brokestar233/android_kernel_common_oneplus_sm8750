// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * LRNG Entropy sources management
 *
 * Copyright (C) 2022 - 2023, Stephan Mueller <smueller@chronox.de>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/random.h>
#include <linux/utsname.h>
#include <linux/workqueue.h>
#include <asm/archrandom.h>

#ifdef CONFIG_VDSO_GETRANDOM
#include <vdso/getrandom.h>
#include <vdso/datapage.h>
#include <vdso/vsyscall.h>
#endif

#include "lrng_drng_atomic.h"
#include "lrng_drng_mgr.h"
#include "lrng_es_aux.h"
#include "lrng_es_cpu.h"
#include "lrng_es_irq.h"
#include "lrng_es_jent.h"
#include "lrng_es_krng.h"
#include "lrng_es_mgr.h"
#include "lrng_es_sched.h"
#include "lrng_interface_dev_common.h"
#include "lrng_interface_random_kernel.h"

struct lrng_state {
	bool can_invalidate;		/* Can invalidate batched entropy? */
	bool perform_seedwork;		/* Can seed work be performed? */
	bool lrng_operational;		/* Is DRNG operational? */
	bool lrng_fully_seeded;		/* Is DRNG fully seeded? */
	bool lrng_min_seeded;		/* Is DRNG minimally seeded? */
	bool all_online_numa_node_seeded;/* All NUMA DRNGs seeded? */

	/*
	 * To ensure that external entropy providers cannot dominate the
	 * internal noise sources but yet cannot be dominated by internal
	 * noise sources, the following booleans are intended to allow
	 * external to provide seed once when a DRNG reseed occurs. This
	 * triggering of external noise source is performed even when the
	 * entropy pool has sufficient entropy.
	 */

	atomic_t boot_entropy_thresh;	/* Reseed threshold */
	struct mutex reseed_in_progress;	/* Flag for on executing reseed */
	struct work_struct lrng_seed_work;	/* (re)seed work queue */
};

static struct lrng_state lrng_state = {
	false, false, false, false, false, false,
	.boot_entropy_thresh	= ATOMIC_INIT(LRNG_INIT_ENTROPY_BITS),
	.reseed_in_progress	=
		__MUTEX_INITIALIZER(lrng_state.reseed_in_progress),
};

/*
 * If the entropy count falls under this number of bits, then we
 * should wake up processes which are selecting or polling on write
 * access to /dev/random.
 */
u32 lrng_write_wakeup_bits = (LRNG_WRITE_WAKEUP_ENTROPY << 3);

/*
 * The entries must be in the same order as defined by enum lrng_internal_es and
 * enum lrng_external_es
 */
struct lrng_es_cb *lrng_es[] = {
#ifdef CONFIG_LRNG_IRQ
	&lrng_es_irq,
#endif
#ifdef CONFIG_LRNG_SCHED
	&lrng_es_sched,
#endif
#ifdef CONFIG_LRNG_JENT
	&lrng_es_jent,
#endif
#ifdef CONFIG_LRNG_CPU
	&lrng_es_cpu,
#endif
#ifdef CONFIG_LRNG_KERNEL_RNG
	&lrng_es_krng,
#endif
	&lrng_es_aux
};

static bool ntg1 = false;
#ifdef CONFIG_LRNG_AIS2031_NTG1_SEEDING_STRATEGY
module_param(ntg1, bool, 0444);
MODULE_PARM_DESC(ntg1, "Enable AIS20/31 NTG.1 compliant seeding strategy\n");
#endif

/* Only panic the kernel on permanent health failure if this variable is true */
static bool lrng_panic_on_permanent_health_failure = false;
module_param(lrng_panic_on_permanent_health_failure, bool, 0444);
MODULE_PARM_DESC(lrng_panic_on_permanent_health_failure, "Panic on reaching permanent health failure - only required if LRNG is part of a FIPS 140-3 module\n");

/********************************** Helper ***********************************/

bool lrng_enforce_panic_on_permanent_health_failure(void)
{
	return lrng_panic_on_permanent_health_failure;
}

bool lrng_ntg1_2024_compliant(void)
{
	return ntg1;
}

void lrng_debug_report_seedlevel(const char *name)
{
#ifdef CONFIG_WARN_ALL_UNSEEDED_RANDOM
	static void *previous = NULL;
	void *caller = (void *) _RET_IP_;
	struct lrng_drng *atomic = lrng_get_atomic();

	if (READ_ONCE(previous) == caller)
		return;

	if (atomic && !atomic->fully_seeded)
		pr_notice("%pS %s called without reaching minimally seeded level (available entropy %u)\n",
			  caller, name, lrng_avail_entropy());

	WRITE_ONCE(previous, caller);
#endif
}

/*
 * Reading of the LRNG pool is only allowed by one caller. The reading is
 * only performed to (re)seed DRNGs. Thus, if this "lock" is already taken,
 * the reseeding operation is in progress. The caller is not intended to wait
 * but continue with its other operation.
 */
int lrng_pool_trylock(void)
{
	return mutex_trylock(&lrng_state.reseed_in_progress);
}

void lrng_pool_lock(void)
{
	mutex_lock(&lrng_state.reseed_in_progress);
}

void lrng_pool_unlock(void)
{
	mutex_unlock(&lrng_state.reseed_in_progress);
}

/* Set new entropy threshold for reseeding during boot */
void lrng_set_entropy_thresh(u32 new_entropy_bits)
{
	atomic_set(&lrng_state.boot_entropy_thresh, new_entropy_bits);
}

/*
 * Reset LRNG state - the entropy counters are reset, but the data that may
 * or may not have entropy remains in the pools as this data will not hurt.
 */
void lrng_reset_state(void)
{
	u32 i;

	for_each_lrng_es(i) {
		if (lrng_es[i]->reset)
			lrng_es[i]->reset();
	}
	lrng_state.lrng_operational = false;
	lrng_state.lrng_fully_seeded = false;
	lrng_state.lrng_min_seeded = false;
	lrng_state.all_online_numa_node_seeded = false;

#ifdef CONFIG_VDSO_GETRANDOM
	WRITE_ONCE(__arch_get_k_vdso_rng_data()->is_ready, false);
#endif

	pr_debug("reset LRNG\n");
}

/* Set flag that all DRNGs are fully seeded */
void lrng_pool_all_numa_nodes_seeded(bool set)
{
	lrng_state.all_online_numa_node_seeded = set;
	if (set)
		wake_up_all(&lrng_init_wait);
}

bool lrng_pool_all_numa_nodes_seeded_get(void)
{
	return lrng_state.all_online_numa_node_seeded;
}

/* Return boolean whether LRNG reached minimally seed level */
bool lrng_state_min_seeded(void)
{
	return lrng_state.lrng_min_seeded;
}

/* Return boolean whether LRNG reached fully seed level */
bool lrng_state_fully_seeded(void)
{
	return lrng_state.lrng_fully_seeded;
}

/* Return boolean whether LRNG is considered fully operational */
bool lrng_state_operational(void)
{
	return lrng_state.lrng_operational;
}

static void lrng_init_wakeup(void)
{
#ifdef CONFIG_VDSO_GETRANDOM
	/*
	 * The LRNG does not enable the user space ChaCha20
	 * DRNG in the VDSO.
	 */
	/* WRITE_ONCE(__arch_get_k_vdso_rng_data()->is_ready, true); */
#endif

	wake_up_all(&lrng_init_wait);
	lrng_init_wakeup_dev();
	lrng_kick_random_ready();
}

static u32 lrng_avail_entropy_thresh(void)
{
	u32 ent_thresh = lrng_security_strength();

	/*
	 * Apply oversampling during initialization according to SP800-90C as
	 * we request a larger buffer from the ES.
	 */
	if (lrng_sp80090c_compliant() &&
	    !lrng_state.all_online_numa_node_seeded)
		ent_thresh += LRNG_SEED_BUFFER_INIT_ADD_BITS;

	return ent_thresh;
}

bool lrng_fully_seeded(bool fully_seeded, u32 collected_entropy,
		       struct entropy_buf *eb)
{
	/* AIS20/31 NTG.1: two entropy sources with each delivering 240 bits */
	if (!fully_seeded && ntg1) {
		u32 i, result = 0, ent_thresh = lrng_avail_entropy_thresh();

		for_each_lrng_es(i) {
			result += (eb ? eb->e_bits[i] :
					lrng_es[i]->curr_entropy(ent_thresh)) >=
				  LRNG_AIS2031_NPTRNG_MIN_ENTROPY;
		}

		return (result >= 2);
	}

	return (collected_entropy >= lrng_get_seed_entropy_osr(fully_seeded));
}

u32 lrng_entropy_rate_eb(struct entropy_buf *eb)
{
	u32 i, collected_entropy = 0;

	for_each_lrng_es(i)
		collected_entropy += eb->e_bits[i];

	return collected_entropy;
}

/* Mark one DRNG as not fully seeded */
void lrng_unset_fully_seeded(struct lrng_drng *drng)
{
	drng->fully_seeded = false;
	lrng_pool_all_numa_nodes_seeded(false);

	/*
	 * The init DRNG instance must always be fully seeded as this instance
	 * is the fall-back if any of the per-NUMA node DRNG instances is
	 * insufficiently seeded. Thus, we mark the entire LRNG as
	 * non-operational if the initial DRNG becomes not fully seeded.
	 */
	if (drng == lrng_drng_init_instance() && lrng_state_operational()) {
		pr_debug("LRNG set to non-operational\n");
		lrng_state.lrng_operational = false;
		lrng_state.lrng_fully_seeded = false;

#ifdef CONFIG_VDSO_GETRANDOM
		WRITE_ONCE(__arch_get_k_vdso_rng_data()->is_ready, false);
#endif

		/* If sufficient entropy is available, reseed now. */
		lrng_es_add_entropy();
	}
}

/* Policy to enable LRNG operational mode */
static void lrng_set_operational(void)
{
	/*
	 * LRNG is operational if the initial DRNG is fully seeded. This state
	 * can only occur if either the external entropy sources provided
	 * sufficient entropy, or the SP800-90B startup test completed for
	 * the internal ES to supply also entropy data.
	 */
	if (lrng_state.lrng_fully_seeded) {
		lrng_state.lrng_operational = true;
		lrng_init_wakeup();
		pr_info("LRNG fully operational\n");
	}
}

/* Available entropy in the entire LRNG considering all entropy sources */
u32 lrng_avail_entropy(void)
{
	u32 i, ent = 0, ent_thresh = lrng_avail_entropy_thresh();

	BUILD_BUG_ON(ARRAY_SIZE(lrng_es) != lrng_ext_es_last);
	for_each_lrng_es(i)
		ent += lrng_es[i]->curr_entropy(ent_thresh);
	return ent;
}

u32 lrng_avail_entropy_aux(void)
{
	u32 ent_thresh = lrng_avail_entropy_thresh();

	return lrng_es[lrng_ext_es_aux]->curr_entropy(ent_thresh);
}

/*
 * lrng_init_ops() - Set seed stages of LRNG
 *
 * Set the slow noise source reseed trigger threshold. The initial threshold
 * is set to the minimum data size that can be read from the pool: a word. Upon
 * reaching this value, the next seed threshold of 128 bits is set followed
 * by 256 bits.
 *
 * @eb: buffer containing the size of entropy currently injected into DRNG - if
 *	NULL, the function obtains the available entropy from the ES.
 */
void lrng_init_ops(struct entropy_buf *eb)
{
	struct lrng_state *state = &lrng_state;
	u32 i, requested_bits, seed_bits = 0;

	if (state->lrng_operational)
		return;

	requested_bits = ntg1 ?
		/* Approximation so that two ES should deliver 240 bits each */
		(lrng_avail_entropy() + LRNG_AIS2031_NPTRNG_MIN_ENTROPY) :
		/* Apply SP800-90C oversampling if applicable */
		lrng_get_seed_entropy_osr(state->all_online_numa_node_seeded);

	if (eb) {
		seed_bits = lrng_entropy_rate_eb(eb);
	} else {
		u32 ent_thresh = lrng_avail_entropy_thresh();

		for_each_lrng_es(i)
			seed_bits += lrng_es[i]->curr_entropy(ent_thresh);
	}

	/* DRNG is seeded with full security strength */
	if (state->lrng_fully_seeded) {
		lrng_set_operational();
		lrng_set_entropy_thresh(requested_bits);
	} else if (lrng_fully_seeded(state->all_online_numa_node_seeded,
				     seed_bits, eb)) {
		if (state->can_invalidate)
			invalidate_batched_entropy();

		state->lrng_fully_seeded = true;
		lrng_set_operational();
		state->lrng_min_seeded = true;
		pr_info("LRNG fully seeded with %u bits of entropy\n",
			seed_bits);
		lrng_set_entropy_thresh(requested_bits);
	} else if (!state->lrng_min_seeded) {

		/* DRNG is seeded with at least 128 bits of entropy */
		if (seed_bits >= LRNG_MIN_SEED_ENTROPY_BITS) {
			if (state->can_invalidate)
				invalidate_batched_entropy();

			state->lrng_min_seeded = true;
			pr_info("LRNG minimally seeded with %u bits of entropy\n",
				seed_bits);
			lrng_set_entropy_thresh(requested_bits);
			lrng_init_wakeup();

		/* DRNG is seeded with at least LRNG_INIT_ENTROPY_BITS bits */
		} else if (seed_bits >= LRNG_INIT_ENTROPY_BITS) {
			pr_info("LRNG initial entropy level %u bits of entropy\n",
				seed_bits);
			lrng_set_entropy_thresh(LRNG_MIN_SEED_ENTROPY_BITS);
		}
	}
}

void __init lrng_rand_initialize_early(void)
{
	struct seed {
		unsigned long data[((LRNG_MAX_DIGESTSIZE +
				     sizeof(unsigned long) - 1) /
				    sizeof(unsigned long))];
		struct new_utsname utsname;
	} seed __aligned(LRNG_KCAPI_ALIGN);
	size_t longs = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(seed.data); i += longs) {
		longs = arch_get_random_seed_longs(seed.data + i,
						   ARRAY_SIZE(seed.data) - i);
		if (longs)
			continue;
		longs = arch_get_random_longs(seed.data + i,
					      ARRAY_SIZE(seed.data) - i);
		if (longs)
			continue;
		longs = 1;
	}
	memcpy(&seed.utsname, init_utsname(), sizeof(*(init_utsname())));

	lrng_pool_insert_aux((u8 *)&seed, sizeof(seed), 0);
	memzero_explicit(&seed, sizeof(seed));

	lrng_force_fully_seeded();
}

void __init lrng_rand_initialize(void)
{
	unsigned long entropy = random_get_entropy();
	ktime_t time = ktime_get_real();

	lrng_pool_insert_aux((u8 *)&entropy, sizeof(entropy), 0);
	lrng_pool_insert_aux((u8 *)&time, sizeof(time), 0);

	/* Initialize the seed work queue */
	INIT_WORK(&lrng_state.lrng_seed_work, lrng_drng_seed_work);
	lrng_state.perform_seedwork = true;

	invalidate_batched_entropy();

	lrng_state.can_invalidate = true;
}

#ifndef CONFIG_LRNG_RANDOM_IF
static int __init lrng_rand_initialize_call(void)
{
	lrng_rand_initialize_early();
	lrng_rand_initialize();
	return 0;
}

early_initcall(lrng_rand_initialize_call);
#endif

/* Interface requesting a reseed of the DRNG */
void lrng_es_add_entropy(void)
{
	/*
	 * Once all DRNGs are fully seeded, the system-triggered arrival of
	 * entropy will not cause any reseeding any more.
	 */
	if (likely(lrng_state.all_online_numa_node_seeded))
		return;

	/* Only trigger the DRNG reseed if we have collected entropy. */
	if (lrng_avail_entropy() <
	    atomic_read_u32(&lrng_state.boot_entropy_thresh))
		return;

	/* Ensure that the seeding only occurs once at any given time. */
	if (!lrng_pool_trylock())
		return;

	/* Seed the DRNG with any available noise. */
	if (lrng_state.perform_seedwork)
		schedule_work(&lrng_state.lrng_seed_work);
	else
		lrng_drng_seed_work(NULL);
}

/* Fill the seed buffer with data from the noise sources */
void lrng_fill_seed_buffer(struct entropy_buf *eb, u32 requested_bits,
			   bool force)
{
	struct lrng_state *state = &lrng_state;
	u32 i, req_ent = lrng_sp80090c_compliant() ?
			  lrng_security_strength() : LRNG_MIN_SEED_ENTROPY_BITS;

	/* Guarantee that requested bits is a multiple of bytes */
	BUILD_BUG_ON(LRNG_DRNG_SECURITY_STRENGTH_BITS % 8);

	/* always reseed the DRNG with the current time stamp */
	eb->now = random_get_entropy();

	/*
	 * Require at least 128 bits of entropy for any reseed. If the LRNG is
	 * operated SP800-90C compliant we want to comply with SP800-90A section
	 * 9.2 mandating that DRNG is reseeded with the security strength.
	 */
	if (!force &&
	    state->lrng_fully_seeded && (lrng_avail_entropy() < req_ent)) {
		for_each_lrng_es(i)
			eb->e_bits[i] = 0;

		goto wakeup;
	}

	/* Concatenate the output of the entropy sources. */
	for_each_lrng_es(i) {
		lrng_es[i]->get_ent(eb, requested_bits,
				    state->lrng_fully_seeded);
	}

	/* allow external entropy provider to provide seed */
	lrng_state_exseed_allow_all();

wakeup:
	lrng_writer_wakeup();
}
