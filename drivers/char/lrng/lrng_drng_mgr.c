// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * LRNG DRNG management
 *
 * Copyright (C) 2022, Stephan Mueller <smueller@chronox.de>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/lrng.h>
#include <linux/fips.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/wait.h>

#include "lrng_drng_atomic.h"
#include "lrng_drng_chacha20.h"
#include "lrng_drng_drbg.h"
#include "lrng_drng_kcapi.h"
#include "lrng_drng_mgr.h"
#include "lrng_es_aux.h"
#include "lrng_es_mgr.h"
#include "lrng_interface_random_kernel.h"
#include "lrng_numa.h"
#include "lrng_sha.h"

/*
 * Maximum number of seconds between DRNG reseed intervals of the DRNG. Note,
 * this is enforced with the next request of random numbers from the
 * DRNG. Setting this value to zero implies a reseeding attempt before every
 * generated random number.
 */
int lrng_drng_reseed_max_time = 600;

/*
 * Is LRNG for general-purpose use (i.e. is at least the lrng_drng_init
 * fully allocated)?
 */
static atomic_t lrng_avail = ATOMIC_INIT(0);

/* Guard protecting all crypto callback update operation of all DRNGs. */
DEFINE_MUTEX(lrng_crypto_cb_update);

/*
 * Default hash callback that provides the crypto primitive right from the
 * kernel start. It must not perform any memory allocation operation, but
 * simply perform the hash calculation.
 */
const struct lrng_hash_cb *lrng_default_hash_cb = &lrng_sha_hash_cb;

/*
 * Default DRNG callback that provides the crypto primitive which is
 * allocated either during late kernel boot stage. So, it is permissible for
 * the callback to perform memory allocation operations.
 */
const struct lrng_drng_cb *lrng_default_drng_cb =
#if defined(CONFIG_LRNG_DFLT_DRNG_CHACHA20)
	&lrng_cc20_drng_cb;
#elif defined(CONFIG_LRNG_DFLT_DRNG_DRBG)
	&lrng_drbg_cb;
#elif defined(CONFIG_LRNG_DFLT_DRNG_KCAPI)
	&lrng_kcapi_drng_cb;
#else
#error "Unknown default DRNG selected"
#endif

/* DRNG for non-atomic use cases */
static struct lrng_drng lrng_drng_init = {
	LRNG_DRNG_STATE_INIT(lrng_drng_init, NULL, NULL, NULL,
			     &lrng_sha_hash_cb),
	.lock = __MUTEX_INITIALIZER(lrng_drng_init.lock),
};

/* Prediction-resistance DRNG: only deliver as much data as received entropy */
static struct lrng_drng lrng_drng_pr = {
	LRNG_DRNG_STATE_INIT(lrng_drng_pr, NULL, NULL, NULL,
			     &lrng_sha_hash_cb),
	.lock = __MUTEX_INITIALIZER(lrng_drng_pr.lock),
};

static u32 max_wo_reseed = LRNG_DRNG_MAX_WITHOUT_RESEED;
#ifdef CONFIG_LRNG_RUNTIME_MAX_WO_RESEED_CONFIG
module_param(max_wo_reseed, uint, 0444);
MODULE_PARM_DESC(max_wo_reseed,
		 "Maximum number of DRNG generate operation without full reseed\n");
#endif

static bool force_seeding = true;
#ifdef CONFIG_LRNG_RUNTIME_FORCE_SEEDING_DISABLE
module_param(force_seeding, bool, 0444);
MODULE_PARM_DESC(force_seeding,
		 "Allow disabling of the forced seeding when insufficient entropy is available\n");
#endif

/* Wait queue to wait until the LRNG is initialized - can freely be used */
DECLARE_WAIT_QUEUE_HEAD(lrng_init_wait);

/********************************** Helper ************************************/

bool lrng_get_available(void)
{
	return likely(atomic_read(&lrng_avail));
}

struct lrng_drng *lrng_drng_init_instance(void)
{
	return &lrng_drng_init;
}

struct lrng_drng *lrng_drng_pr_instance(void)
{
	return &lrng_drng_pr;
}

struct lrng_drng *lrng_drng_node_instance(void)
{
	struct lrng_drng **lrng_drng = lrng_drng_instances();
	int node = numa_node_id();

	if (lrng_drng && lrng_drng[node])
		return lrng_drng[node];

	return lrng_drng_init_instance();
}

void lrng_drng_reset(struct lrng_drng *drng)
{
	/* Ensure reseed during next call */
	atomic_set(&drng->requests, 1);
	atomic_set(&drng->requests_since_fully_seeded, 0);
	drng->last_seeded = jiffies;
	drng->fully_seeded = false;
	/* Do not set force, as this flag is used for the emergency reseeding */
	drng->force_reseed = false;
	pr_debug("reset DRNG\n");
}

/* Initialize the DRNG, except the mutex lock */
int lrng_drng_alloc_common(struct lrng_drng *drng,
			   const struct lrng_drng_cb *drng_cb)
{
	if (!drng || !drng_cb)
		return -EINVAL;
	if (!IS_ERR_OR_NULL(drng->drng))
		return 0;

	drng->drng_cb = drng_cb;
	drng->drng = drng_cb->drng_alloc(LRNG_DRNG_SECURITY_STRENGTH_BYTES);
	if (IS_ERR(drng->drng))
		return -PTR_ERR(drng->drng);

	lrng_drng_reset(drng);
	return 0;
}

/* Initialize the default DRNG during boot and perform its seeding */
int lrng_drng_initalize(void)
{
	int ret;

	if (lrng_get_available())
		return 0;

	/* Catch programming error */
	WARN_ON(lrng_drng_init.hash_cb != lrng_default_hash_cb);

	mutex_lock(&lrng_drng_init.lock);
	if (lrng_get_available()) {
		mutex_unlock(&lrng_drng_init.lock);
		return 0;
	}

	/* Initialize the PR DRNG inside init lock as it guards lrng_avail. */
	mutex_lock(&lrng_drng_pr.lock);
	ret = lrng_drng_alloc_common(&lrng_drng_pr, lrng_default_drng_cb);
	mutex_unlock(&lrng_drng_pr.lock);

	if (!ret) {
		ret = lrng_drng_alloc_common(&lrng_drng_init,
					     lrng_default_drng_cb);
		if (!ret)
			atomic_set(&lrng_avail, 1);
	}
	mutex_unlock(&lrng_drng_init.lock);
	if (ret)
		return ret;

	pr_debug("LRNG for general use is available\n");

	/* Seed the DRNG with any entropy available */
	if (lrng_pool_trylock()) {
		pr_info("Initial DRNG initialized triggering first seeding\n");
		lrng_drng_seed_work(NULL);
	} else {
		pr_info("Initial DRNG initialized without seeding\n");
	}

	return 0;
}

static int __init lrng_drng_make_available(void)
{
	return lrng_drng_initalize();
}
late_initcall(lrng_drng_make_available);

bool lrng_sp80090c_compliant(void)
{
	/* SP800-90C compliant oversampling is only requested in FIPS mode */
	return fips_enabled;
}

/************************* Random Number Generation ***************************/

/* Inject a data buffer into the DRNG - caller must hold its lock */
void lrng_drng_inject(struct lrng_drng *drng, const u8 *inbuf, u32 inbuflen,
		      bool fully_seeded, const char *drng_type)
{
	BUILD_BUG_ON(LRNG_DRNG_RESEED_THRESH > INT_MAX);
	pr_debug("seeding %s DRNG with %u bytes\n", drng_type, inbuflen);
	if (drng->drng_cb->drng_seed(drng->drng, inbuf, inbuflen) < 0) {
		pr_warn("seeding of %s DRNG failed\n", drng_type);
		drng->force_reseed = true;
	} else {
		int gc = LRNG_DRNG_RESEED_THRESH - atomic_read(&drng->requests);

		pr_debug("%s DRNG stats since last seeding: %lu secs; generate calls: %d\n",
			 drng_type,
			 (time_after(jiffies, drng->last_seeded) ?
			  (jiffies - drng->last_seeded) : 0) / HZ, gc);

		/* Count the numbers of generate ops since last fully seeded */
		if (fully_seeded)
			atomic_set(&drng->requests_since_fully_seeded, 0);
		else
			atomic_add(gc, &drng->requests_since_fully_seeded);

		drng->last_seeded = jiffies;
		atomic_set(&drng->requests, LRNG_DRNG_RESEED_THRESH);
		drng->force_reseed = false;

		if (!drng->fully_seeded) {
			drng->fully_seeded = fully_seeded;
			if (drng->fully_seeded)
				pr_debug("%s DRNG fully seeded\n", drng_type);
		}
	}
}

/*
 * Perform the seeding of the DRNG with data from entropy source.
 * The function returns the entropy injected into the DRNG in bits.
 */
static u32 lrng_drng_seed_es_nolock(struct lrng_drng *drng, bool init_ops,
				    const char *drng_type)
{
	struct entropy_buf seedbuf __aligned(LRNG_KCAPI_ALIGN),
			   collected_seedbuf;
	u32 collected_entropy = 0;
	unsigned int i, num_es_delivered = 0;
	bool forced = drng->force_reseed;

	for_each_lrng_es(i)
		collected_seedbuf.e_bits[i] = 0;

	do {
		/* Count the number of ES which delivered entropy */
		num_es_delivered = 0;

		if (collected_entropy)
			pr_debug("Force fully seeding level for %s DRNG by repeatedly pull entropy from available entropy sources\n",
				 drng_type);

		lrng_fill_seed_buffer(&seedbuf,
			lrng_get_seed_entropy_osr(drng->fully_seeded),
				      forced && !drng->fully_seeded);

		collected_entropy += lrng_entropy_rate_eb(&seedbuf);

		/* Sum iterations up. */
		for_each_lrng_es(i) {
			collected_seedbuf.e_bits[i] += seedbuf.e_bits[i];
			num_es_delivered += !!seedbuf.e_bits[i];
		}

		lrng_drng_inject(drng, (u8 *)&seedbuf, sizeof(seedbuf),
				 lrng_fully_seeded(drng->fully_seeded,
						   collected_entropy,
						   &collected_seedbuf),
				 drng_type);

		/*
		 * Set the seeding state of the LRNG
		 *
		 * Do not call lrng_init_ops(seedbuf) here as the atomic DRNG
		 * does not serve common users.
		 */
		if (init_ops)
			lrng_init_ops(&collected_seedbuf);

	/*
	 * Emergency reseeding: If we reached the min seed threshold now
	 * multiple times but never reached fully seeded level and we collect
	 * entropy, keep doing it until we reached fully seeded level for
	 * at least one DRNG. This operation is not continued if the
	 * ES do not deliver entropy such that we cannot reach the fully seeded
	 * level.
	 *
	 * The emergency reseeding implies that the consecutively injected
	 * entropy can be added up. This is applicable due to the fact that
	 * the entire operation is atomic which means that the DRNG is not
	 * producing data while this is ongoing.
	 */
	} while (force_seeding && forced && !drng->fully_seeded &&
		 num_es_delivered >= (lrng_ntg1_2024_compliant() ? 2 : 1));

	memzero_explicit(&seedbuf, sizeof(seedbuf));

	return collected_entropy;
}

static void lrng_drng_seed_es(struct lrng_drng *drng)
{
	mutex_lock(&drng->lock);
	lrng_drng_seed_es_nolock(drng, true, "regular");
	mutex_unlock(&drng->lock);
}

static void lrng_drng_seed(struct lrng_drng *drng)
{
	BUILD_BUG_ON(LRNG_MIN_SEED_ENTROPY_BITS >
		     LRNG_DRNG_SECURITY_STRENGTH_BITS);

	/* (Re-)Seed DRNG */
	lrng_drng_seed_es(drng);
	/* (Re-)Seed atomic DRNG from regular DRNG */
	lrng_drng_atomic_seed_drng(drng);
}

static void lrng_drng_seed_work_one(struct lrng_drng *drng, u32 node)
{
	pr_debug("reseed triggered by system events for DRNG on NUMA node %d\n",
		 node);
	lrng_drng_seed(drng);
	if (drng->fully_seeded) {
		/* Prevent reseed storm */
		drng->last_seeded += node * 100 * HZ;
	}
}

/*
 * DRNG reseed trigger: Kernel thread handler triggered by the schedule_work()
 */
static void __lrng_drng_seed_work(bool force)
{
	struct lrng_drng **lrng_drng;
	u32 node;

	/*
	 * If the DRNG is not yet initialized, let us try to seed the atomic
	 * DRNG.
	 */
	if (!lrng_get_available()) {
		struct lrng_drng *atomic;
		unsigned long flags;

		if (wq_has_sleeper(&lrng_init_wait)) {
			lrng_init_ops(NULL);
			return;
		}
		atomic = lrng_get_atomic();
		if (!atomic || atomic->fully_seeded)
			return;

		atomic->force_reseed |= force;
		spin_lock_irqsave(&atomic->spin_lock, flags);
		lrng_drng_seed_es_nolock(atomic, false, "atomic");
		spin_unlock_irqrestore(&atomic->spin_lock, flags);

		return;
	}

	lrng_drng = lrng_drng_instances();
	if (lrng_drng) {
		for_each_online_node(node) {
			struct lrng_drng *drng = lrng_drng[node];

			if (drng && !drng->fully_seeded) {
				drng->force_reseed |= force;
				lrng_drng_seed_work_one(drng, node);
				return;
			}
		}
	} else {
		if (!lrng_drng_init.fully_seeded) {
			lrng_drng_init.force_reseed |= force;
			lrng_drng_seed_work_one(&lrng_drng_init, 0);
			return;
		}
	}

	if (!lrng_drng_pr.fully_seeded) {
		lrng_drng_pr.force_reseed |= force;
		lrng_drng_seed_work_one(&lrng_drng_pr, 0);
		return;
	}

	lrng_pool_all_numa_nodes_seeded(true);
}

void lrng_drng_seed_work(struct work_struct *dummy)
{
	__lrng_drng_seed_work(false);

	/* Allow the seeding operation to be called again */
	lrng_pool_unlock();
}

/* Force all DRNGs to reseed before next generation */
void lrng_drng_force_reseed(void)
{
	struct lrng_drng **lrng_drng = lrng_drng_instances();
	u32 node;

	/*
	 * If the initial DRNG is over the reseed threshold, allow a forced
	 * reseed only for the initial DRNG as this is the fallback for all. It
	 * must be kept seeded before all others to keep the LRNG operational.
	 */
	if (!lrng_drng ||
	    (atomic_read_u32(&lrng_drng_init.requests_since_fully_seeded) >
	     LRNG_DRNG_RESEED_THRESH)) {
		lrng_drng_init.force_reseed = lrng_drng_init.fully_seeded;
		pr_debug("force reseed of initial DRNG\n");
		return;
	}
	for_each_online_node(node) {
		struct lrng_drng *drng = lrng_drng[node];

		if (!drng)
			continue;

		drng->force_reseed = drng->fully_seeded;
		pr_debug("force reseed of DRNG on node %u\n", node);
	}
	lrng_drng_atomic_force_reseed();
}
EXPORT_SYMBOL(lrng_drng_force_reseed);

static bool lrng_drng_must_reseed(struct lrng_drng *drng)
{
	return (atomic_dec_and_test(&drng->requests) ||
		drng->force_reseed ||
		time_after(jiffies,
			   drng->last_seeded + lrng_drng_reseed_max_time * HZ));
}

/*
 * lrng_drng_get() - Get random data out of the DRNG which is reseeded
 * frequently.
 *
 * @drng: DRNG instance
 * @outbuf: buffer for storing random data
 * @outbuflen: length of outbuf
 *
 * Return:
 * * < 0 in error case (DRNG generation or update failed)
 * * >=0 returning the returned number of bytes
 */
int lrng_drng_get(struct lrng_drng *drng, u8 *outbuf, u32 outbuflen)
{
	u32 processed = 0;
	bool pr = (drng == &lrng_drng_pr) ? true : false;

	if (!outbuf || !outbuflen)
		return 0;

	if (!lrng_get_available())
		return -EOPNOTSUPP;

	outbuflen = min_t(size_t, outbuflen, INT_MAX);

	/* If DRNG operated without proper reseed for too long, block LRNG */
	BUILD_BUG_ON(LRNG_DRNG_MAX_WITHOUT_RESEED < LRNG_DRNG_RESEED_THRESH);
	if (atomic_read_u32(&drng->requests_since_fully_seeded) > max_wo_reseed)
		lrng_unset_fully_seeded(drng);

	while (outbuflen) {
		u32 todo = min_t(u32, outbuflen, LRNG_DRNG_MAX_REQSIZE);
		int ret;

		/* In normal operation, check whether to reseed */
		if (!pr && lrng_drng_must_reseed(drng)) {
			if (!lrng_pool_trylock()) {
				drng->force_reseed = true;
			} else {
				lrng_drng_seed(drng);
				lrng_pool_unlock();
			}
		}

		mutex_lock(&drng->lock);

		if (pr) {
			/* If async reseed did not deliver entropy, try now */
			if (!drng->fully_seeded) {
				u32 coll_ent_bits;

				/* If we cannot get the pool lock, try again. */
				if (!lrng_pool_trylock()) {
					mutex_unlock(&drng->lock);
					continue;
				}

				coll_ent_bits = lrng_drng_seed_es_nolock(
							drng, true, "regular");

				lrng_pool_unlock();

				/* If no new entropy was received, stop now. */
				if (!coll_ent_bits) {
					mutex_unlock(&drng->lock);
					goto out;
				}

				/* Produce no more data than received entropy */
				todo = min_t(u32, todo, coll_ent_bits >> 3);
			}

			/* Do not produce more than DRNG security strength */
			todo = min_t(u32, todo, lrng_security_strength() >> 3);
		}
		ret = drng->drng_cb->drng_generate(drng->drng,
						   outbuf + processed, todo);

		mutex_unlock(&drng->lock);
		if (ret <= 0) {
			pr_warn("getting random data from DRNG failed (%d)\n",
				ret);
			return -EFAULT;
		}
		processed += ret;
		outbuflen -= ret;

		if (pr) {
			/* Force the async reseed for PR DRNG */
			lrng_unset_fully_seeded(drng);
			if (outbuflen)
				cond_resched();
		}
	}

out:
	return processed;
}

int lrng_drng_get_sleep(u8 *outbuf, u32 outbuflen, bool pr)
{
	struct lrng_drng **lrng_drng = lrng_drng_instances();
	struct lrng_drng *drng = &lrng_drng_init;
	int ret, node = numa_node_id();

	might_sleep();

	if (pr)
		drng = &lrng_drng_pr;
	else if (lrng_drng && lrng_drng[node] && lrng_drng[node]->fully_seeded)
		drng = lrng_drng[node];

	ret = lrng_drng_initalize();
	if (ret)
		return ret;

	return lrng_drng_get(drng, outbuf, outbuflen);
}

/* Reset LRNG such that all existing entropy is gone */
static void _lrng_reset(struct work_struct *work)
{
	struct lrng_drng **lrng_drng = lrng_drng_instances();

	if (!lrng_drng) {
		mutex_lock(&lrng_drng_init.lock);
		lrng_drng_reset(&lrng_drng_init);
		mutex_unlock(&lrng_drng_init.lock);
	} else {
		u32 node;

		for_each_online_node(node) {
			struct lrng_drng *drng = lrng_drng[node];

			if (!drng)
				continue;
			mutex_lock(&drng->lock);
			lrng_drng_reset(drng);
			mutex_unlock(&drng->lock);
		}
	}

	mutex_lock(&lrng_drng_pr.lock);
	lrng_drng_reset(&lrng_drng_pr);
	mutex_unlock(&lrng_drng_pr.lock);

	lrng_drng_atomic_reset();
	lrng_set_entropy_thresh(LRNG_INIT_ENTROPY_BITS);

	lrng_reset_state();
}

static DECLARE_WORK(lrng_reset_work, _lrng_reset);

void lrng_reset(void)
{
	schedule_work(&lrng_reset_work);
}

/******************* Generic LRNG kernel output interfaces ********************/

void lrng_force_fully_seeded(void)
{
	if (lrng_pool_all_numa_nodes_seeded_get())
		return;

	lrng_pool_lock();
	__lrng_drng_seed_work(true);
	lrng_pool_unlock();
}

static int lrng_drng_sleep_while_not_all_nodes_seeded(unsigned int nonblock)
{
	lrng_force_fully_seeded();
	if (lrng_pool_all_numa_nodes_seeded_get())
		return 0;
	if (nonblock)
		return -EAGAIN;
	wait_event_interruptible(lrng_init_wait,
				 lrng_pool_all_numa_nodes_seeded_get());
	return 0;
}

int lrng_drng_sleep_while_nonoperational(int nonblock)
{
	lrng_force_fully_seeded();
	if (likely(lrng_state_operational()))
		return 0;
	if (nonblock)
		return -EAGAIN;
	return wait_event_interruptible(lrng_init_wait,
					lrng_state_operational());
}

int lrng_drng_sleep_while_non_min_seeded(void)
{
	lrng_force_fully_seeded();
	if (likely(lrng_state_min_seeded()))
		return 0;
	return wait_event_interruptible(lrng_init_wait,
					lrng_state_min_seeded());
}

ssize_t lrng_get_seed(u64 *buf, size_t nbytes, unsigned int flags)
{
	struct entropy_buf *eb = (struct entropy_buf *)(buf + 2);
	u64 buflen = sizeof(struct entropy_buf) + 2 * sizeof(u64);
	u64 collected_bits = 0;
	int ret;

	/* Ensure buffer is aligned as required */
	BUILD_BUG_ON(sizeof(buflen) > LRNG_KCAPI_ALIGN);
	if (nbytes < sizeof(buflen))
		return -EINVAL;

	/* Write buffer size into first word */
	buf[0] = buflen;
	if (nbytes < buflen)
		return -EMSGSIZE;

	ret = lrng_drng_sleep_while_not_all_nodes_seeded(
		flags & LRNG_GET_SEED_NONBLOCK);
	if (ret)
		return ret;

	/* Try to get the pool lock and sleep on it to get it. */
	lrng_pool_lock();

	/* If an LRNG DRNG becomes unseeded, give this DRNG precedence. */
	if (!lrng_pool_all_numa_nodes_seeded_get()) {
		lrng_pool_unlock();
		return 0;
	}

	/*
	 * Try to get seed data - a rarely used busyloop is cheaper than a wait
	 * queue that is constantly woken up by the hot code path of
	 * lrng_init_ops.
	 */
	for (;;) {
		lrng_fill_seed_buffer(eb,
			lrng_get_seed_entropy_osr(flags &
						  LRNG_GET_SEED_FULLY_SEEDED),
						  false);
		collected_bits = lrng_entropy_rate_eb(eb);

		/* Break the collection loop if we got entropy, ... */
		if (collected_bits ||
		    /* ... a DRNG becomes unseeded, give DRNG precedence, ... */
		    !lrng_pool_all_numa_nodes_seeded_get() ||
		    /* ... if the caller does not want a blocking behavior. */
		    (flags & LRNG_GET_SEED_NONBLOCK))
			break;

		schedule();
	}

	lrng_pool_unlock();

	/* Write collected entropy size into second word */
	buf[1] = collected_bits;

	return (ssize_t)buflen;
}

void lrng_get_random_bytes_full(void *buf, int nbytes)
{
	lrng_drng_sleep_while_nonoperational(0);
	lrng_drng_get_sleep((u8 *)buf, (u32)nbytes, false);
}
EXPORT_SYMBOL(lrng_get_random_bytes_full);

void lrng_get_random_bytes_min(void *buf, int nbytes)
{
	lrng_drng_sleep_while_non_min_seeded();
	lrng_drng_get_sleep((u8 *)buf, (u32)nbytes, false);
}
EXPORT_SYMBOL(lrng_get_random_bytes_min);

int lrng_get_random_bytes_pr(void *buf, int nbytes)
{
	lrng_drng_sleep_while_nonoperational(0);
	return lrng_drng_get_sleep((u8 *)buf, (u32)nbytes, true);
}
EXPORT_SYMBOL(lrng_get_random_bytes_pr);
