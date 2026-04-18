// SPDX-License-Identifier: GPL-2.0
/*
 * Cambyses — Context-Aware Migration Balancer Yielding Scored Entity Selection
 *
 * Scored migration selection for CFS load balancer.
 * Score = (now - exec_start) × weight, compressed to pul16 for SIMD argmax.
 *
 * Uses svec (Swap Vector) for cache-friendly contiguous iteration with
 * multi-stage prefetch, replacing linked list traversal.
 */

/*
 * This file is #included from fair.c (not compiled separately)
 * to access static functions: can_migrate_task(), detach_task(),
 * task_h_load(), task_util_est(), task_fits_cpu(), etc.
 */

/**************************************************************
 * Version Information:
 */

#define CAMBYSES_PROGNAME "Cambyses Migration Selector"
#define CAMBYSES_AUTHOR   "Masahito Suzuki"

#define CAMBYSES_VERSION  "0.5.3"

/* Runtime toggle — NOP-patched when disabled */
DEFINE_STATIC_KEY_TRUE(sched_cambyses);

/* Debug trace — NOP when disabled, enable via sysctl kernel.sched_cambyses_debug */
DEFINE_STATIC_KEY_FALSE(cambyses_debug);

/*
 * Newidle fallback level — controls how aggressively an idle CPU
 * relaxes migration gates when no candidate passes the normal filter.
 * The normal pass (budget + cache-hot) always runs; this sysctl
 * controls additional fallback passes:
 *
 *   0: no fallback (normal pass only)
 *   1: drop budget check (allow overshoot, still respect cache-hot)
 *   2: drop budget + cache-hot (any movable task)
 */
static unsigned int sysctl_cambyses_newidle_fallback = 1;

/*
 * Weight emphasis coefficient for task scoring.
 *
 * score = pul16(delta) + pul16(weight) * (1 + coeff / 1024)
 *       = pul16(delta) + pul16(weight) + (pul16(weight) * coeff) >> 10
 *
 *   coeff = 0:    weight contributes equally with delta (log-space equivalent
 *                 of the original delta * weight multiplication)
 *   coeff = 308:  ~1.3x weight emphasis (default; 308 ≈ log10(2) * 1024)
 *   coeff = 1024: 2.0x weight emphasis (maximum)
 *
 * Higher values make the scorer prefer heavier tasks over long-starved
 * light tasks, improving heavy-task migration rates.
 */
/*
 * Multi-source drain — after pulling from the busiest CPU, continue
 * draining additional above-average CPUs in the same group.
 * Disabled by default; enable via sysctl kernel.sched_cambyses_multisource_drain
 */
DEFINE_STATIC_KEY_FALSE(cambyses_multisource_drain);

static unsigned int sysctl_cambyses_weight_coeff = 308;
static int cambyses_weight_coeff_max = 1024;

#ifdef CONFIG_SCHED_CAMBYSES_SIMD
#ifdef CONFIG_X86_64
DEFINE_STATIC_KEY_FALSE(cambyses_has_avx2);
DEFINE_STATIC_KEY_FALSE(cambyses_has_ssse3);
DEFINE_STATIC_KEY_FALSE(cambyses_has_sse41);
#endif
#ifdef CONFIG_ARM64
DEFINE_STATIC_KEY_FALSE(cambyses_has_neon);
#endif
#endif

/*
 * pul16 — Packed Unsigned Log, 16-bit.
 * Monotonic compression of u64 → u16 for SIMD-friendly score representation.
 * Derived from intfp library (Masahito Suzuki, GPL-2.0).
 *
 * Format: | 6-bit exponent | 10-bit mantissa |
 * Range:  0 .. 2^62 (covers all practical delta*weight values)
 * Property: strictly monotone for v >= 2 → argmax(pul16) = argmax(u64)
 *
 * Cost: CLZ + 2 shifts + 1 add ≈ 4-5 cycles/task.
 */
#define PUL16_FP  10

static inline u16 u64_to_pul16(u64 v)
{
	u8 clz;
	u16 m;

	if (v <= 1)
		return (u16)!v;  /* 0→1, 1→0 */

	clz = __builtin_clzll(v);
	m = (u16)((v << clz) >> (64 - 1 - PUL16_FP));
	return ((u16)(62 - clz) << PUL16_FP) + m;
}

/*
 * score_task_raw — compute u64 migration suitability score.
 *
 * score = (now - exec_start) × weight
 *
 * Compensates for CFS statistical priority inversion: lighter tasks
 * accumulate larger deltas (scheduled less frequently), so without
 * weight multiplication they would be preferentially migrated.
 * Multiplying by weight produces a WSJF-like urgency metric that
 * favors heavier (more important) tasks.
 */
static inline u64 score_task_raw(struct task_struct *p, struct rq *src_rq)
{
	u64 delta = rq_clock_task(src_rq) - p->se.exec_start;
	unsigned long weight = max_t(unsigned long, task_h_load(p), 1);

	return delta * weight;
}

/*
 * score_task — compute u16 migration score with tunable weight emphasis.
 *
 * score = pul16(delta) + pul16(weight) * (1 + coeff / 1024)
 *
 * Both delta and weight are compressed to pul16 (log2-approximate u16),
 * making their contributions comparable in magnitude.  The coefficient
 * amplifies weight's contribution so heavier tasks score higher.
 *
 * Returns u16 (clamped).  Directly usable in scores[] for SIMD argmax.
 */
static inline u16 score_task(struct task_struct *p, struct rq *src_rq)
{
	u64 delta = rq_clock_task(src_rq) - p->se.exec_start;
	unsigned long weight = max_t(unsigned long, task_h_load(p), 1);
	u16 d = u64_to_pul16(delta);
	u16 w = u64_to_pul16(weight);
	unsigned int coeff = sysctl_cambyses_weight_coeff;
	u32 score = (u32)d + w + ((u32)w * coeff >> 10);

	return score > 0xFFFF ? 0xFFFF : (u16)score;
}

/*
 * prefetch_migration_task — prefetch task_struct cache lines to L1
 * that can_migrate_task() + score_task_raw() will access.
 *
 * With svec, future element addresses are known in advance (index+1),
 * enabling multi-stage prefetch depth (vs 1-ahead with linked lists).
 */
static inline void prefetch_migration_task(struct task_struct *p)
{
	/* exec_start — needed for scoring */
	prefetch(&p->se.exec_start);
	/* se.avg: for task_h_load() */
	prefetch(&p->se.avg.util_avg);
	/* cpus_ptr — always needed by can_migrate_task */
	prefetch(&p->cpus_ptr);
	/* on_cpu + flags — needed by can_migrate_task (task_on_cpu, kthread_is_per_cpu) */
	prefetch(&p->on_cpu);
}

/*
 * prefetch_migration_task_l3 — light prefetch to L3 only.
 *
 * Two-level prefetch pipeline:
 *   Level 2 (far-ahead):  DRAM → L3  via prefetchT2 (this function)
 *   Level 1 (near-ahead): L3 → L1    via prefetchT0 (prefetch_migration_task)
 *
 * By the time Level 1 fires, data is already in L3 (~10-15 cyc hit)
 * instead of DRAM (~400 cyc miss).  Only one cache line is prefetched
 * to minimize MSHR pressure on in-order cores.
 */
static inline void prefetch_migration_task_l3(struct task_struct *p)
{
	/* locality hint 1 = L3 level (prefetcht2 on x86) */
	__builtin_prefetch(&p->se.exec_start, 0, 1);
}

/*
 * check_imbalance_cambyses — coarse filter for Phase 1 candidate inclusion
 *
 * Replaces Vanilla's shr_bound(load, nr_balance_failed) > imbalance with a
 * simple load > imbalance comparison.  Vanilla's shr_bound progressively
 * relaxes the check as nr_balance_failed grows, but nr_balance_failed is
 * reset to 0 whenever any task is successfully migrated — so light-task
 * migrations perpetually reset the counter, preventing heavy tasks from
 * ever passing the check.
 *
 * The direct comparison avoids this starvation: a heavy task is excluded
 * only when its load genuinely exceeds the remaining budget, not when an
 * exponential back-off counter happens to be too low.
 */
static bool check_imbalance_cambyses(struct task_struct *p,
				     struct lb_env *env)
{
	switch (env->migration_type) {
	case migrate_load: {
		unsigned long load = max_t(unsigned long, task_h_load(p), 1);

		if (sched_feat(LB_MIN) &&
		    load < 16 && !env->sd->nr_balance_failed)
			return false;
		if (load > env->imbalance)
			return false;
		return true;
	}
	case migrate_util: {
		unsigned long util = task_util_est(p);

		if (util > env->imbalance)
			return false;
		return true;
	}
	case migrate_task:
		return true;
	case migrate_misfit:
		return !task_fits_cpu(p, env->src_cpu);
	}
	return false;
}

/*
 * consume_imbalance_cambyses — deduct task cost from imbalance budget
 *
 * Called in Phase 2 for each detached task.
 * Matches Vanilla behavior: allows imbalance to go negative on the last task.
 */
static void consume_imbalance_cambyses(struct task_struct *p,
				       struct lb_env *env)
{
	switch (env->migration_type) {
	case migrate_load:
		env->imbalance -= max_t(unsigned long, task_h_load(p), 1);
		break;
	case migrate_util:
		env->imbalance -= task_util_est(p);
		break;
	case migrate_task:
		env->imbalance--;
		break;
	case migrate_misfit:
		env->imbalance = 0;
		break;
	}
}

#ifdef CONFIG_SCHED_CAMBYSES_SIMD

/*
 * cambyses_simd_begin/end — FPU/FPSIMD context management for
 * IRQ-disabled scheduler context.
 */

#ifdef CONFIG_X86_64
#include <asm/fpu/api.h>

static __always_inline void cambyses_simd_begin(void)
{
	kernel_fpu_begin();
}

static __always_inline void cambyses_simd_end(void)
{
	kernel_fpu_end();
}
#endif /* CONFIG_X86_64 */

#ifdef CONFIG_ARM64
#include <asm/fpsimd.h>

static __always_inline void cambyses_simd_begin(void)
{
	fpsimd_save_and_flush_cpu_state();
}

static __always_inline void cambyses_simd_end(void)
{
	/* Nothing — lazy restore via fpsimd_restore_current_state() */
}
#endif /* CONFIG_ARM64 */

/*
 * cambyses_simd_argmax — ISA-dispatched SIMD argmax on u16 pul16 scores.
 * Returns index of maximum, or -1 if all zero.
 */
static __always_inline int cambyses_simd_argmax(const u16 *scores)
{
#ifdef CONFIG_X86_64
	if (static_branch_likely(&cambyses_has_avx2))
		return cambyses_simd_argmax_avx2(scores);
	if (static_branch_likely(&cambyses_has_ssse3))
		return cambyses_simd_argmax_ssse3(scores);
#endif
#ifdef CONFIG_ARM64
	if (static_branch_likely(&cambyses_has_neon))
		return cambyses_simd_argmax_neon(scores);
#endif
	return -1;  /* no SIMD available */
}

/*
 * cambyses_simd_u64_to_pul16 — ISA-dispatched SIMD batch conversion.
 */
static __always_inline void cambyses_simd_u64_to_pul16(const u64 *raw,
							u16 *out, int nr)
{
#ifdef CONFIG_X86_64
	if (static_branch_likely(&cambyses_has_avx2))
		cambyses_simd_u64_to_pul16_avx2(raw, out, nr);
	else if (static_branch_likely(&cambyses_has_ssse3))
		cambyses_simd_u64_to_pul16_ssse3(raw, out, nr);
	else
		for (int i = 0; i < nr; i++)
			out[i] = u64_to_pul16(raw[i]);
	return;
#endif
#ifdef CONFIG_ARM64
	if (static_branch_likely(&cambyses_has_neon)) {
		cambyses_simd_u64_to_pul16_neon(raw, out, nr);
		return;
	}
#endif
	for (int i = 0; i < nr; i++)
		out[i] = u64_to_pul16(raw[i]);
}

static __always_inline bool cambyses_simd_usable(void)
{
#ifdef CONFIG_X86_64
	return irq_fpu_usable();
#elif defined(CONFIG_ARM64)
	return true;
#else
	return false;
#endif
}

#endif /* CONFIG_SCHED_CAMBYSES_SIMD */

/*
 * argmax_scores_u16 — find index of highest u16 score via 4-way parallel
 * reduction.  GPR-only — no FPU context needed.
 *
 * Returns index of maximum, or -1 if all scores are 0 (tombstoned).
 */
static __always_inline int argmax_scores_u16(const u16 *scores, int nr)
{
	u16 bv0 = 0, bv1 = 0, bv2 = 0, bv3 = 0;
	int bi0 = -1, bi1 = -1, bi2 = -1, bi3 = -1;
	int j;

	for (j = 0; j + 3 < nr; j += 4) {
		if (scores[j]     > bv0) { bv0 = scores[j];     bi0 = j;     }
		if (scores[j + 1] > bv1) { bv1 = scores[j + 1]; bi1 = j + 1; }
		if (scores[j + 2] > bv2) { bv2 = scores[j + 2]; bi2 = j + 2; }
		if (scores[j + 3] > bv3) { bv3 = scores[j + 3]; bi3 = j + 3; }
	}
	for (; j < nr; j++) {
		if (scores[j] > bv0) { bv0 = scores[j]; bi0 = j; }
	}
	if (bv1 > bv0) { bv0 = bv1; bi0 = bi1; }
	if (bv3 > bv2) { bv2 = bv3; bi2 = bi3; }
	if (bv2 > bv0) { bi0 = bi2; }
	return bi0;
}

#define CAMBYSES_PREFETCH_DEPTH    8
#define CAMBYSES_PREFETCH_L3_DEPTH 16

/* Minimum candidates to enter SIMD path */
#ifdef CONFIG_SCHED_CAMBYSES_SIMD
#define CAMBYSES_SIMD_THRESHOLD		min_t(int, 8, SCHED_NR_MIGRATE_BREAK)
#endif

/*
 * CAMBYSES_PRESCORE_LIMIT — max tasks to pre-score from shadow arrays.
 *
 * Phase 0 scans up to this many svec entries using only shadow arrays
 * (no task_struct access), then selects top-K for full evaluation.
 */
#define CAMBYSES_PRESCORE_LIMIT	64

/*
 * pre_score_shadow — compute approximate score from shadow arrays.
 *
 * score ≈ (now - shadow_exec_start) × shadow_weight
 * No task_struct access — pure sequential array reads.
 */
static inline u64 pre_score_shadow(u64 now, u64 shadow_exec_start,
				   unsigned long shadow_weight)
{
	u64 delta = now - shadow_exec_start;

	return delta * max_t(unsigned long, shadow_weight, 1);
}

/*
 * select_topk_indices — partial selection of top-K indices by pre-score.
 *
 * Uses a simple insertion-sort into a small fixed-size buffer.
 * K = SCHED_NR_MIGRATE_BREAK (typically 8).  O(N×K) ≈ O(N).
 */
static int select_topk_indices(const u64 *pre_scores, int nr,
			       int *out_indices, int k)
{
	u64 topk_scores[SCHED_NR_MIGRATE_BREAK];
	int topk_nr = 0;
	int i, j;

	for (i = 0; i < nr; i++) {
		u64 s = pre_scores[i];

		if (topk_nr < k) {
			/* Still filling — insert sorted */
			j = topk_nr++;
			while (j > 0 && topk_scores[j - 1] < s) {
				topk_scores[j] = topk_scores[j - 1];
				out_indices[j] = out_indices[j - 1];
				j--;
			}
			topk_scores[j] = s;
			out_indices[j] = i;
		} else if (s > topk_scores[topk_nr - 1]) {
			/* Replace the smallest in top-K */
			j = topk_nr - 1;
			while (j > 0 && topk_scores[j - 1] < s) {
				topk_scores[j] = topk_scores[j - 1];
				out_indices[j] = out_indices[j - 1];
				j--;
			}
			topk_scores[j] = s;
			out_indices[j] = i;
		}
	}
	return topk_nr;
}

/*
 * detach_tasks_cambyses — scored migration selection for Pull path
 *
 * Called from detach_tasks() when sched_cambyses is active.
 * Operates under rq_lock_irqsave (inherited from caller).
 *
 * Phase 0:   Pre-score via shadow arrays (no task_struct access)
 * Phase 0.5: Select top-K candidates by pre-score
 * Phase 1:   can_migrate_task + check_imbalance on top-K only + exact score
 * Phase 1.5: u64 → pul16 batch conversion (SIMD or scalar)
 * Phase 2:   Repeated argmax extraction (SIMD or scalar) + detach
 */
static int detach_tasks_cambyses(struct lb_env *env)
{
	struct rq *src_rq = env->src_rq;
	struct sched_cambyses_rq_data *cambyses = rq_cambyses(src_rq);
	struct svec_head *sv;
	int nr;
	struct cambyses_candidate cands[SCHED_NR_MIGRATE_BREAK];
#ifdef CONFIG_SCHED_CAMBYSES_SIMD
	u16 scores[CAMBYSES_SIMD_SCORES_SIZE] = {
		[0 ... CAMBYSES_SIMD_SCORES_SIZE - 1] = 0
	};
#else
	u16 scores[SCHED_NR_MIGRATE_BREAK];
#endif
	int nr_cands = 0, detached = 0, i;
	int scan_nr;

	if (!cambyses) {
		env->flags &= ~LBF_ALL_PINNED;
		return 0;
	}

	sv = &cambyses->tasks;
	nr = svec_count(sv);

	if (src_rq->nr_running <= 1) {
		env->flags &= ~LBF_ALL_PINNED;
		return 0;
	}

	scan_nr = min(nr, CAMBYSES_PRESCORE_LIMIT);

	/*
	 * SMT siblings share L1/L2 cache, so cache-hot migration
	 * penalty is negligible.  Skip the hot check entirely.
	 */
	bool smt = env->sd->flags & SD_SHARE_CPUCAPACITY;

	if (static_branch_unlikely(&cambyses_debug))
		trace_printk("cambyses: balance_scan src_cpu=%d dst_cpu=%d nr=%d scan_nr=%d "
			     "mig_type=%d imbal=%ld idle=%d\n",
			     env->src_cpu, env->dst_cpu, nr, scan_nr,
			     env->migration_type, env->imbalance, env->idle);

	/*
	 * Phase 0: Pre-score via shadow arrays.
	 *
	 * Sequential reads of shadow_exec_start[] and shadow_weight[]
	 * (contiguous u64/ulong arrays in struct rq) — no pointer chasing,
	 * no task_struct cache line touches.  ~1 cache line per 8 entries.
	 */
	if (scan_nr > SCHED_NR_MIGRATE_BREAK) {
		u64 pre_scores[CAMBYSES_PRESCORE_LIMIT];
		int topk_indices[SCHED_NR_MIGRATE_BREAK];
		int topk_nr;
		u64 now = rq_clock_task(src_rq);

		for (i = 0; i < scan_nr; i++)
			pre_scores[i] = pre_score_shadow(
				now,
				cambyses->shadow_exec_start[i],
				cambyses->shadow_weight[i]);

		/* Phase 0.5: Select top-K indices */
		topk_nr = select_topk_indices(pre_scores, scan_nr,
					      topk_indices,
					      SCHED_NR_MIGRATE_BREAK);

		/*
		 * Phase 1: Evaluate top-K candidates only.
		 * Prefetch the selected tasks (not the entire svec).
		 */
		for (i = 0; i < min(topk_nr, CAMBYSES_PREFETCH_DEPTH); i++) {
			struct task_struct *t =
				svec_entry_idx(sv, topk_indices[i],
					       struct task_struct,
					       se.cambyses_node);
			prefetch_migration_task(t);
		}

		for (i = 0; i < topk_nr; i++) {
			int si = topk_indices[i];
			struct task_struct *p =
				svec_entry_idx(sv, si,
					       struct task_struct,
					       se.cambyses_node);

			/* Prefetch next candidate (if any) */
			if (i + CAMBYSES_PREFETCH_DEPTH < topk_nr)
				prefetch_migration_task(
					svec_entry_idx(sv,
						topk_indices[i + CAMBYSES_PREFETCH_DEPTH],
						struct task_struct,
						se.cambyses_node));

			/*
			 * CAMBYSES_FILTER_SCORE — shared filter + score + collect.
			 * Used in both pre-score and direct-scan paths.
			 *
			 * Wrapped in if(1){} so that break/continue target
			 * the enclosing for-loop (not a do-while wrapper).
			 */
			#define CAMBYSES_FILTER_SCORE(p) \
			if (1) { \
			env->loop++; \
			if (env->loop > env->loop_max) \
				break; \
			if (env->loop > env->loop_break) { \
				env->loop_break += SCHED_NR_MIGRATE_BREAK; \
				env->flags |= LBF_NEED_BREAK; \
				break; \
			} \
			\
			if (smt) { \
				if (!cpumask_test_cpu(env->dst_cpu, \
								(p)->cpus_ptr)) \
					continue; \
				if (task_on_cpu(env->src_rq, (p))) \
					continue; \
			} else { \
				if (!can_migrate_task((p), env)) { \
					if (task_hot((p), env)) \
						schedstat_inc( \
						(p)->stats.nr_failed_migrations_hot); \
					continue; \
				} \
			} \
			if (!check_imbalance_cambyses((p), env)) \
				continue; \
			\
			cands[nr_cands].p = (p); \
			scores[nr_cands] = score_task((p), src_rq); \
			nr_cands++; \
			\
			if (nr_cands >= SCHED_NR_MIGRATE_BREAK) \
				break; \
			}
			CAMBYSES_FILTER_SCORE(p);
		}
	} else {
		/*
		 * Small svec (≤ SCHED_NR_MIGRATE_BREAK): direct scan.
		 * Pre-scoring overhead not worth it — evaluate all directly.
		 */

		/* Two-level prefetch priming */
		for (i = 0; i < min(scan_nr, CAMBYSES_PREFETCH_L3_DEPTH); i++) {
			struct task_struct *t =
				svec_entry_idx(sv, i, struct task_struct,
					       se.cambyses_node);
			if (i < CAMBYSES_PREFETCH_DEPTH)
				prefetch_migration_task(t);
			else
				prefetch_migration_task_l3(t);
		}

		/* Direct scan + filter + score */
		for (i = 0; i < scan_nr; i++) {
			struct task_struct *p =
				svec_entry_idx(sv, i, struct task_struct,
					       se.cambyses_node);

			if (i + CAMBYSES_PREFETCH_DEPTH < scan_nr)
				prefetch_migration_task(
					svec_entry_idx(sv,
						i + CAMBYSES_PREFETCH_DEPTH,
						struct task_struct,
						se.cambyses_node));

			if (i + CAMBYSES_PREFETCH_L3_DEPTH < scan_nr)
				prefetch_migration_task_l3(
					svec_entry_idx(sv,
						i + CAMBYSES_PREFETCH_L3_DEPTH,
						struct task_struct,
						se.cambyses_node));

			CAMBYSES_FILTER_SCORE(p);
			#undef CAMBYSES_FILTER_SCORE
		}
	}

	/*
	 * Newidle fallback: if the normal filter found no candidates,
	 * progressively relax gates so the idle CPU gets *something*.
	 *
	 *   Pass 1: drop check_imbalance (allow budget overshoot)
	 *   Pass 2: also skip cache-hot tasks (any movable task)
	 *
	 * Pass number matches sysctl_cambyses_newidle_fallback directly.
	 * For SMT domains, cache-hot is already skipped in the normal
	 * pass, so Pass 1 is redundant — jump straight to Pass 2.
	 *
	 * The svec is already built; only the filter
	 * loop is re-run, so the cost is minimal.
	 */
	if (!nr_cands && env->idle == CPU_NEWLY_IDLE &&
	    sysctl_cambyses_newidle_fallback > 0) {
		int pass;
		int max_pass = sysctl_cambyses_newidle_fallback;

		/*
		 * SMT: hot check is already skipped in the normal pass,
		 * so Pass 1 (drop budget, keep hot) is redundant — jump
		 * straight to Pass 2 (drop budget + hot).
		 */
		if (smt) {
			pass = 2;
			max_pass = 2;
		} else {
			pass = 1;
		}

		for (; pass <= max_pass && !nr_cands; pass++) {
			for (i = 0; i < scan_nr; i++) {
				struct task_struct *p =
					svec_entry_idx(sv, i,
						       struct task_struct,
						       se.cambyses_node);

				if (pass < 2) {
					if (!can_migrate_task(p, env))
						continue;
				} else {
					/*
					 * Skip only hard constraints
					 * (affinity, on_cpu); allow hot.
					 */
					if (!cpumask_test_cpu(env->dst_cpu,
							      p->cpus_ptr))
						continue;
					if (task_on_cpu(env->src_rq, p))
						continue;
					if (env->flags & LBF_DST_PINNED &&
					    !cpumask_test_cpu(env->dst_cpu,
							      env->dst_grpmask))
						continue;
				}
				/* No check_imbalance — accept overshoot */
				cands[nr_cands].p = p;
				scores[nr_cands] =
					score_task(p, src_rq);
				nr_cands++;

				if (nr_cands >= SCHED_NR_MIGRATE_BREAK)
					break;
			}
		}
	}

	if (!nr_cands)
		return 0;

	/* Debug: dump candidate info before scoring */
	if (static_branch_unlikely(&cambyses_debug)) {
		trace_printk("cambyses: svec_nr=%d scan_nr=%d nr_cands=%d path=%s idle=%d mig_type=%d src_cpu=%d dst_cpu=%d\n",
			     nr, scan_nr, nr_cands,
			     scan_nr > SCHED_NR_MIGRATE_BREAK ? "prescore" : "direct",
			     env->idle, env->migration_type,
			     env->src_cpu, env->dst_cpu);
		for (i = 0; i < nr_cands; i++) {
			struct task_struct *__p = cands[i].p;
			u64 __delta = rq_clock_task(src_rq) - __p->se.exec_start;

			trace_printk("  cand[%d]: pid=%d comm=%s nice=%d weight=%lu "
				     "load_avg=%lu delta=%llu score=%u\n",
				     i, __p->pid, __p->comm,
				     task_nice(__p),
				     __p->se.load.weight,
				     __p->se.avg.load_avg,
				     __delta, scores[i]);
		}
	}

	/*
	 * Phase 2: Repeated argmax + detach.
	 *
	 * pul16 score 0 is used as tombstone: pul16(0)=1, pul16(1)=0,
	 * and real scores (delta*weight) are always >= 2 in practice.
	 */
#ifdef CONFIG_SCHED_CAMBYSES_SIMD
	if (nr_cands >= CAMBYSES_SIMD_THRESHOLD && cambyses_simd_usable()) {
		int selected[SCHED_NR_MIGRATE_BREAK];
		int nr_selected = 0;

		cambyses_simd_begin();
		while (env->imbalance > 0) {
			int best = cambyses_simd_argmax(scores);

			if (best < 0 || scores[best] == 0)
				break;

			scores[best] = 0;  /* tombstone */
			selected[nr_selected++] = best;

			consume_imbalance_cambyses(cands[best].p, env);

#ifdef CONFIG_PREEMPTION
			if (env->idle == CPU_NEWLY_IDLE)
				break;
#endif
		}
		cambyses_simd_end();

		/* Detach selected candidates (outside FPU context) */
		for (i = 0; i < nr_selected; i++) {
			struct task_struct *p = cands[selected[i]].p;

			if (static_branch_unlikely(&cambyses_debug))
				trace_printk("cambyses: SIMD selected[%d]=%d pid=%d comm=%s "
					     "nice=%d score=%u src_cpu=%d\n",
					     i, selected[i], p->pid, p->comm,
					     task_nice(p), scores[selected[i]],
					     env->src_cpu);

			if (env->idle && env->src_rq->nr_running <= 1)
				break;

			detach_task(p, env);
			list_add(&p->se.group_node, &env->tasks);
			detached++;
		}
	} else
#endif /* CONFIG_SCHED_CAMBYSES_SIMD */
	{
		while (env->imbalance > 0) {
			int best = argmax_scores_u16(scores, nr_cands);
			struct task_struct *p;

			if (best < 0 || scores[best] == 0)
				break;

			if (env->idle && env->src_rq->nr_running <= 1)
				break;

			p = cands[best].p;

			if (static_branch_unlikely(&cambyses_debug))
				trace_printk("cambyses: scalar selected=%d pid=%d comm=%s "
					     "nice=%d score=%u src_cpu=%d\n",
					     best, p->pid, p->comm,
					     task_nice(p), scores[best],
					     env->src_cpu);

			scores[best] = 0;  /* tombstone */

			consume_imbalance_cambyses(p, env);
			detach_task(p, env);
			list_add(&p->se.group_node, &env->tasks);
			detached++;

#ifdef CONFIG_PREEMPTION
			if (env->idle == CPU_NEWLY_IDLE)
				break;
#endif
		}
	}

	schedstat_add(env->sd->lb_gained[env->idle], detached);
	return detached;
}

/*
 * detach_one_task_cambyses — scored selection for Push path (active balancing)
 *
 * Scans all migratable tasks via svec and selects the one with the
 * highest raw score.  Push moves exactly 1 task, so pul16 compression
 * is unnecessary — direct u64 comparison suffices.
 */
static struct task_struct *detach_one_task_cambyses(struct lb_env *env)
{
	struct rq *src_rq = env->src_rq;
	struct sched_cambyses_rq_data *cambyses = rq_cambyses(src_rq);
	struct svec_head *sv;
	int nr;
	struct task_struct *best = NULL;
	u64 best_score = 0;
	int i;

	if (!cambyses)
		return NULL;

	sv = &cambyses->tasks;
	nr = svec_count(sv);

	lockdep_assert_rq_held(src_rq);

	if (static_branch_unlikely(&cambyses_debug))
		trace_printk("cambyses_one: balance_scan src_cpu=%d dst_cpu=%d nr=%d idle=%d\n",
			     env->src_cpu, env->dst_cpu, nr, env->idle);

	for (i = 0; i < min(nr, CAMBYSES_PREFETCH_L3_DEPTH); i++) {
		struct task_struct *t =
			svec_entry_idx(sv, i, struct task_struct,
				       se.cambyses_node);
		if (i < CAMBYSES_PREFETCH_DEPTH)
			prefetch_migration_task(t);
		else
			prefetch_migration_task_l3(t);
	}

	for (i = 0; i < nr; i++) {
		struct task_struct *p =
			svec_entry_idx(sv, i, struct task_struct,
				       se.cambyses_node);
		u64 score;

		if (i + CAMBYSES_PREFETCH_DEPTH < nr)
			prefetch_migration_task(
				svec_entry_idx(sv,
					       i + CAMBYSES_PREFETCH_DEPTH,
					       struct task_struct,
					       se.cambyses_node));

		if (i + CAMBYSES_PREFETCH_L3_DEPTH < nr)
			prefetch_migration_task_l3(
				svec_entry_idx(sv,
					       i + CAMBYSES_PREFETCH_L3_DEPTH,
					       struct task_struct,
					       se.cambyses_node));

		if (!can_migrate_task(p, env)) {
			if (static_branch_unlikely(&cambyses_debug))
				trace_printk("cambyses_one: SKIP pid=%d comm=%s nice=%d "
					     "weight=%lu delta=%llu idle=%d src_cpu=%d\n",
					     p->pid, p->comm, task_nice(p),
					     p->se.load.weight,
					     rq_clock_task(src_rq) - p->se.exec_start,
					     env->idle, env->src_cpu);
			continue;
		}

		score = score_task_raw(p, src_rq);
		if (score > best_score) {
			best_score = score;
			best = p;
		}
	}

	if (!best) {
		if (static_branch_unlikely(&cambyses_debug))
			trace_printk("cambyses_one: NO_PICK svec_nr=%d src_cpu=%d dst_cpu=%d idle=%d\n",
				     nr, env->src_cpu, env->dst_cpu, env->idle);
		return NULL;
	}

	if (static_branch_unlikely(&cambyses_debug))
		trace_printk("cambyses_one: PICK pid=%d comm=%s nice=%d "
			     "weight=%lu delta=%llu score=%llu idle=%d src_cpu=%d dst_cpu=%d\n",
			     best->pid, best->comm, task_nice(best),
			     best->se.load.weight,
			     rq_clock_task(src_rq) - best->se.exec_start,
			     best_score, env->idle, env->src_cpu, env->dst_cpu);

	detach_task(best, env);
	schedstat_inc(env->sd->lb_gained[env->idle]);
	return best;
}

#ifdef CONFIG_SCHED_CAMBYSES_SIMD
#include <asm/cpufeature.h>

static int __init cambyses_init(void)
{
	const char *simd_name;

#ifdef CONFIG_X86_64
	if (boot_cpu_has(X86_FEATURE_AVX2)) {
		static_branch_enable(&cambyses_has_avx2);
		simd_name = "AVX2";
	} else if (boot_cpu_has(X86_FEATURE_SSSE3)) {
		static_branch_enable(&cambyses_has_ssse3);
		if (boot_cpu_has(X86_FEATURE_XMM4_1)) {
			static_branch_enable(&cambyses_has_sse41);
			simd_name = "SSE4.1";
		} else {
			simd_name = "SSSE3";
		}
	} else {
		simd_name = "none";
	}
#endif

#ifdef CONFIG_ARM64
	if (system_supports_fpsimd()) {
		static_branch_enable(&cambyses_has_neon);
		simd_name = "NEON";
	} else {
		simd_name = "none";
	}
#endif

	pr_info("%s v%s by %s [SIMD: %s, svec capacity: 256]\n",
		CAMBYSES_PROGNAME, CAMBYSES_VERSION,
		CAMBYSES_AUTHOR, simd_name);

	return 0;
}
#else /* !CONFIG_SCHED_CAMBYSES_SIMD */
static int __init cambyses_init(void)
{
	pr_info("%s v%s by %s [svec capacity: 256]\n",
		CAMBYSES_PROGNAME, CAMBYSES_VERSION,
		CAMBYSES_AUTHOR);

	return 0;
}
#endif /* CONFIG_SCHED_CAMBYSES_SIMD */
late_initcall(cambyses_init);

/* ======== sysctl interface ======== */

#ifdef CONFIG_SYSCTL
static int sched_cambyses_handler(struct ctl_table *table,
				  int write, void *buffer,
				  size_t *lenp, loff_t *ppos)
{
	static u8 sched_cambyses_val;
	struct ctl_table tmp = {
		.data	= &sched_cambyses_val,
		.maxlen	= sizeof(u8),
		.mode	= table->mode,
		.extra1	= SYSCTL_ZERO,
		.extra2	= SYSCTL_ONE,
	};
	int ret;

	if (!write)
		sched_cambyses_val = static_key_enabled(&sched_cambyses);

	ret = proc_dou8vec_minmax(&tmp, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (sched_cambyses_val)
		static_branch_enable(&sched_cambyses);
	else
		static_branch_disable(&sched_cambyses);

	return 0;
}

static int cambyses_debug_handler(struct ctl_table *table,
				  int write, void *buffer,
				  size_t *lenp, loff_t *ppos)
{
	static u8 cambyses_debug_val;
	struct ctl_table tmp = {
		.data	= &cambyses_debug_val,
		.maxlen	= sizeof(u8),
		.mode	= table->mode,
		.extra1	= SYSCTL_ZERO,
		.extra2	= SYSCTL_ONE,
	};
	int ret;

	if (!write)
		cambyses_debug_val = static_key_enabled(&cambyses_debug);

	ret = proc_dou8vec_minmax(&tmp, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (cambyses_debug_val)
		static_branch_enable(&cambyses_debug);
	else
		static_branch_disable(&cambyses_debug);

	return 0;
}

static int cambyses_multisource_drain_handler(struct ctl_table *table,
					      int write, void *buffer,
					      size_t *lenp, loff_t *ppos)
{
	static u8 cambyses_multisource_drain_val;
	struct ctl_table tmp = {
		.data	= &cambyses_multisource_drain_val,
		.maxlen	= sizeof(u8),
		.mode	= table->mode,
		.extra1	= SYSCTL_ZERO,
		.extra2	= SYSCTL_ONE,
	};
	int ret;

	if (!write)
		cambyses_multisource_drain_val =
			static_key_enabled(&cambyses_multisource_drain);

	ret = proc_dou8vec_minmax(&tmp, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	if (cambyses_multisource_drain_val)
		static_branch_enable(&cambyses_multisource_drain);
	else
		static_branch_disable(&cambyses_multisource_drain);

	return 0;
}

static struct ctl_table sched_cambyses_sysctls[] = {
	{
		.procname	= "sched_cambyses",
		.data		= NULL,
		.maxlen		= sizeof(u8),
		.mode		= 0644,
		.proc_handler	= sched_cambyses_handler,
	},
	{
		.procname	= "sched_cambyses_debug",
		.data		= NULL,
		.maxlen		= sizeof(u8),
		.mode		= 0644,
		.proc_handler	= cambyses_debug_handler,
	},
	{
		.procname	= "sched_cambyses_newidle_fallback",
		.data		= &sysctl_cambyses_newidle_fallback,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_TWO,
	},
	{
		.procname	= "sched_cambyses_multisource_drain",
		.data		= NULL,
		.maxlen		= sizeof(u8),
		.mode		= 0644,
		.proc_handler	= cambyses_multisource_drain_handler,
	},
	{
		.procname	= "sched_cambyses_weight_coeff",
		.data		= &sysctl_cambyses_weight_coeff,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= &cambyses_weight_coeff_max,
	},
};

static int __init sched_cambyses_sysctl_init(void)
{
	register_sysctl_init("kernel", sched_cambyses_sysctls);
	return 0;
}
late_initcall(sched_cambyses_sysctl_init);
#endif /* CONFIG_SYSCTL */
