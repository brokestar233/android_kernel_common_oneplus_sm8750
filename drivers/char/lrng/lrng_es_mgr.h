/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright (C) 2022, Stephan Mueller <smueller@chronox.de>
 */

#ifndef _LRNG_ES_MGR_H
#define _LRNG_ES_MGR_H

#include "lrng_es_mgr_cb.h"

/*************************** General LRNG parameter ***************************/

#define LRNG_DRNG_BLOCKSIZE 64		/* Maximum of DRNG block sizes */

/* Helper to concatenate a macro with an integer type */
#define LRNG_PASTER(x, y) x ## y
#define LRNG_UINT32_C(x) LRNG_PASTER(x, U)

/************************* Entropy sources management *************************/

extern struct lrng_es_cb *lrng_es[];

#define for_each_lrng_es(ctr)		\
	for ((ctr) = 0; (ctr) < lrng_ext_es_last; (ctr)++)

bool lrng_enforce_panic_on_permanent_health_failure(void);
bool lrng_ntg1_2024_compliant(void);
bool lrng_pool_all_numa_nodes_seeded_get(void);
bool lrng_state_min_seeded(void);
void lrng_debug_report_seedlevel(const char *name);
void lrng_rand_initialize_early(void);
void lrng_rand_initialize(void);
bool lrng_state_operational(void);

extern u32 lrng_write_wakeup_bits;
void lrng_set_entropy_thresh(u32 new);
u32 lrng_avail_entropy(void);
u32 lrng_avail_entropy_aux(void);
void lrng_reset_state(void);

bool lrng_state_fully_seeded(void);

int lrng_pool_trylock(void);
void lrng_pool_lock(void);
void lrng_pool_unlock(void);
void lrng_pool_all_numa_nodes_seeded(bool set);

bool lrng_fully_seeded(bool fully_seeded, u32 collected_entropy,
		       struct entropy_buf *eb);
u32 lrng_entropy_rate_eb(struct entropy_buf *eb);
void lrng_unset_fully_seeded(struct lrng_drng *drng);
void lrng_fill_seed_buffer(struct entropy_buf *eb, u32 requested_bits,
			   bool force);
void lrng_init_ops(struct entropy_buf *eb);

#endif /* _LRNG_ES_MGR_H */
