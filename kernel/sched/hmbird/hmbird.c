// SPDX-License-Identifier: GPL-2.0

/*
 * HMBIRD scheduler class
 *
 * Copyright (c) 2024 OPlus.
 * Copyright (c) 2024 Dao Huang <huangdao1@oppo.com>
 * Copyright (c) 2024 Yuxing Wang <wangyuxing@oppo.com>
 * Copyright (c) 2024 Taiyu Li <litaiyu@oppo.com>
 */

#include "hmbird/slim.h"
#include "hmbird/hmbird_sched.h"
#include <linux/sched/hmbird_proc_val.h>

atomic_t __hmbird_ops_enabled = ATOMIC_INIT(0);
atomic_t non_hmbird_task;
atomic_t hmbird_module_loaded = ATOMIC_INIT(0);
int cgroup_ids_table[NUMS_CGROUP_KINDS];

enum task_event {
	PUT_PREV_TASK   = 0,
	PICK_NEXT_TASK  = 1,
	TASK_WAKE       = 2,
	TASK_MIGRATE    = 3,
	TASK_UPDATE     = 4,
	IRQ_UPDATE      = 5,
};

enum hmbird_internal_consts {
	HMBIRD_WATCHDOG_MAX_TIMEOUT = 30 * HZ,
};

enum hmbird_ops_enable_state {
	HMBIRD_OPS_PREPPING,
	HMBIRD_OPS_ENABLING,
	HMBIRD_OPS_ENABLED,
	HMBIRD_OPS_DISABLING,
	HMBIRD_OPS_DISABLED,
};

static inline struct task_group *css_tg(struct cgroup_subsys_state *css)
{
	return css ? container_of(css, struct task_group, css) : NULL;
}

static inline void check_class_changed(struct rq *rq, struct task_struct *p,
							const struct sched_class *prev_class,
							int oldprio)
{
	if (prev_class != p->sched_class) {
		if (prev_class->switched_from)
			prev_class->switched_from(rq, p);

		p->sched_class->switched_to(rq, p);
	} else if (oldprio != p->prio || dl_task(p))
		p->sched_class->prio_changed(rq, p, oldprio);
}

/*
 * hmbird_entity->ops_state
 *
 * Used to track the task ownership between the HMBIRD core and the BPF scheduler.
 * State transitions look as follows:
 *
 * NONE -> QUEUEING -> QUEUED -> DISPATCHING
 *   ^              |                 |
 *   |              v                 v
 *   \-------------------------------/
 *
 * QUEUEING and DISPATCHING states can be waited upon. See wait_ops_state() call
 * sites for explanations on the conditions being waited upon and why they are
 * safe. Transitions out of them into NONE or QUEUED must store_release and the
 * waiters should load_acquire.
 *
 * Tracking hmbird_ops_state enables hmbird core to reliably determine whether
 * any given task can be dispatched by the BPF scheduler at all times and thus
 * relaxes the requirements on the BPF scheduler. This allows the BPF scheduler
 * to try to dispatch any task anytime regardless of its state as the HMBIRD core
 * can safely reject invalid dispatches.
 */
enum hmbird_ops_state {
	HMBIRD_OPSS_NONE,		/* owned by the HMBIRD core */
	HMBIRD_OPSS_QUEUEING,	/* in transit to the BPF scheduler */
	HMBIRD_OPSS_QUEUED,	/* owned by the BPF scheduler */
	HMBIRD_OPSS_DISPATCHING,	/* in transit back to the HMBIRD core */

	/*
	 * QSEQ brands each QUEUED instance so that, when dispatch races
	 * dequeue/requeue, the dispatcher can tell whether it still has a claim
	 * on the task being dispatched.
	 */
	HMBIRD_OPSS_QSEQ_SHIFT	= 2,
	HMBIRD_OPSS_STATE_MASK	= (1LLU << HMBIRD_OPSS_QSEQ_SHIFT) - 1,
	HMBIRD_OPSS_QSEQ_MASK	= ~HMBIRD_OPSS_STATE_MASK,
};

enum switch_stat {
	HMBIRD_DISABLED,
	HMBIRD_SWITCH_PREP,
	HMBIRD_RQ_SWITCH_BEGIN,
	HMBIRD_RQ_SWITCH_DONE,
	HMBIRD_ENABLED,
};
enum switch_stat curr_ss;

/*
 * During exit, a task may schedule after losing its PIDs. When disabling the
 * BPF scheduler, we need to be able to iterate tasks in every state to
 * guarantee system safety. Maintain a dedicated task list which contains every
 * task between its fork and eventual free.
 */
DEFINE_SPINLOCK(hmbird_tasks_lock);
static LIST_HEAD(hmbird_tasks);

/* ops enable/disable */
static struct kthread_worker *hmbird_ops_helper;
static DEFINE_MUTEX(hmbird_ops_enable_mutex);
DEFINE_STATIC_PERCPU_RWSEM(hmbird_fork_rwsem);
static atomic_t hmbird_ops_enable_state_var = ATOMIC_INIT(HMBIRD_OPS_DISABLED);
static atomic_t set_sched_clock_prepare;

static bool hmbird_warned_zero_slice;
static int skip_num[MAX_GLOBAL_DSQS];
static int big_distribute_mask_prev;
static int little_distribute_mask_prev;

DEFINE_STATIC_KEY_FALSE(hmbird_ops_cpu_preempt);

static atomic64_t hmbird_nr_rejected = ATOMIC64_INIT(0);

/*
 * The maximum amount of time in jiffies that a task may be runnable without
 * being scheduled on a CPU. If this timeout is exceeded, it will trigger
 * hmbird_ops_error().
 */
unsigned long hmbird_watchdog_timeout;

/*
 * The last time the delayed work was run. This delayed work relies on
 * ksoftirqd being able to run to service timer interrupts, so it's possible
 * that this work itself could get wedged. To account for this, we check that
 * it's not stalled in the timer tick, and trigger an error if it is.
 */
unsigned long hmbird_watchdog_timestamp = INITIAL_JIFFIES;

static struct delayed_work hmbird_watchdog_work;
static struct work_struct hmbird_err_exit_work;

/* idle tracking */
#ifdef CONFIG_SMP
#ifdef CONFIG_CPUMASK_OFFSTACK
#define CL_ALIGNED_IF_ONSTACK
#else
#define CL_ALIGNED_IF_ONSTACK __cacheline_aligned_in_smp
#endif

static struct {
	cpumask_var_t cpu;
	cpumask_var_t smt;
} idle_masks CL_ALIGNED_IF_ONSTACK;

static bool __cacheline_aligned_in_smp hmbird_has_idle_cpus;
#endif	/* CONFIG_SMP */

/* dispatch queues */
static struct hmbird_dispatch_q __cacheline_aligned_in_smp hmbird_dsq_global;

u32 HMBIRD_BPF_DSQS_DEADLINE[MAX_GLOBAL_DSQS] = {0, 1, 2, 4, 6, 8, 16, 32, 64, 128};
u32 pcp_dsq_deadline = 20;
static struct hmbird_dispatch_q __cacheline_aligned_in_smp gdsqs[MAX_GLOBAL_DSQS];
static DEFINE_PER_CPU(struct hmbird_dispatch_q, pcp_ldsq);

static u64 max_hmbird_dsq_internal_id;

/* a dsq idx, whether task push to little domain cpu or bit domain cpu*/
#define CLUSTER_SEPARATE_IDX	(8)
#define GDSQS_ID_BASE		(3)
#define NON_PERIOD_START	(5)
#define NON_PERIOD_END		(MAX_GLOBAL_DSQS)
#define CREATE_DSQ_LEVEL_WITHIN	(1)

struct hmbird_sched_info {
	spinlock_t lock;
	int curr_idx[2];
	int rtime[MAX_GLOBAL_DSQS];
};

struct pcp_sched_info {
	s64 pcp_seq;
	int rtime;
	bool pcp_round;
};

/*
 * pcp_info may rw by another cpu.
 * protected by rq->lock.
 */
atomic64_t pcp_dsq_round;
static DEFINE_PER_CPU(struct pcp_sched_info, pcp_info);

struct hmbird_sched_rq_stats {
	u64				window_start;
	u64				latest_clock;
	u32				prev_window_size;
	u64				task_exec_scale;
	u64				prev_runnable_sum;
	u64				curr_runnable_sum;
};
DEFINE_PER_CPU(struct hmbird_sched_rq_stats, hmbird_sched_rq_stats);

static int b_rescue_l, l_rescue_b;
static struct hmbird_sched_info sinfo;

static unsigned long pcp_dsq_quota __read_mostly = 3 * NSEC_PER_MSEC;
static unsigned long dsq_quota[MAX_GLOBAL_DSQS] = {
					0, 0, 0, 0, 0,
					32 * NSEC_PER_MSEC,
					20 * NSEC_PER_MSEC,
					14 * NSEC_PER_MSEC,
					8 * NSEC_PER_MSEC,
					6 * NSEC_PER_MSEC
};

struct cluster_ctx {
	/* cpu-dsq map must within [lower, upper) */
	int upper;
	int lower;
	int tidx;
};

enum stat_items {
	GLOBAL_STAT,
	CPU_ALLOW_FAIL,
	RT_CNT,
	KEY_TASK_CNT,
	SWITCH_IDX,
	TIMEOUT_CNT,

	TOTAL_DSP_CNT,
	MOVE_RQ_CNT,
	SELECT_CPU,

	DWORD_STAT_END = SELECT_CPU,

	GDSQ_CNT,
	ERR_IDX,
	PCP_TIMEOUT_CNT,
	PCP_LDSQ_CNT,
	PCP_ENQL_CNT,

	MAX_ITEMS,
};
static DEFINE_SPINLOCK(stats_lock);
static char *stats_str[MAX_ITEMS] = {
	"global stat", "cpu_allow_fail", "rt_cnt", "key_task_cnt",
	"switch_idx", "timeout_cnt", "total_dsp_cnt", "move_rq_cnt",
	"select_cpu", "gdsq_cnt", "err_idx", "pcp_timeout_cnt",
	"pcp_ldsq_cnt", "pcp_enql_cnt"
};


struct stats_struct {
	u64 global_stat[2];
	u64 cpu_allow_fail[2];
	u64 rt_cnt[2];
	u64 key_task_cnt[2];
	u64 switch_idx[2];
	u64 timeout_cnt[2];

	u64 total_dsp_cnt[2];
	u64 move_rq_cnt[2];
	u64 select_cpu[2];

	u64 gdsq_count[MAX_GLOBAL_DSQS][2];
	u64 err_idx[5];
	u64 pcp_timeout_cnt[NR_CPUS];
	u64 pcp_ldsq_count[NR_CPUS][2];
	u64 pcp_enql_cnt[NR_CPUS];
} stats_data;

static struct {
	cpumask_var_t ex_free;
	cpumask_var_t exclusive;
	cpumask_var_t partial;
	cpumask_var_t big;
	cpumask_var_t little;
} iso_masks __read_mostly;

static bool cpu_same_cluster_stat(struct task_struct *p, struct rq *rq1, struct rq *rq2)
{
	int c1, c2;
	struct cpumask mask = {.bits = {0}};
	struct cpumask tmp = {.bits = {0}};

	if (!slim_stats)
		return false;

	if (!rq1 || !rq2)
		return false;

	c1 = cpu_of(rq1);
	c2 = cpu_of(rq2);
	cpumask_set_cpu(c1, &mask);
	cpumask_set_cpu(c2, &mask);

	if (cpumask_and(&tmp, iso_masks.little, &mask))
		if (cpumask_equal(&tmp, &mask))
			return true;

	if (cpumask_and(&tmp, iso_masks.big, &mask))
		if (cpumask_equal(&tmp, &mask))
			return true;

	if (cpumask_and(&tmp, iso_masks.partial, &mask))
		if (cpumask_equal(&tmp, &mask))
			return true;

	return false;
}

static void slim_stats_record(enum stat_items item, int idx, int dsq_id, int cpu)
{
	unsigned long flags;
	u64 *pval;
	u64 *pbase = (u64 *)&stats_data;

	if (!slim_stats)
		return;

	switch (item) {
	case GLOBAL_STAT:
		fallthrough;
	case CPU_ALLOW_FAIL:
		fallthrough;
	case RT_CNT:
		fallthrough;
	case KEY_TASK_CNT:
		fallthrough;
	case SWITCH_IDX:
		fallthrough;
	case TIMEOUT_CNT:
		fallthrough;
	case TOTAL_DSP_CNT:
		fallthrough;
	case MOVE_RQ_CNT:
		fallthrough;
	case SELECT_CPU:
		pval = pbase + item * 2 + idx;
		break;
	case GDSQ_CNT:
		pval = &stats_data.gdsq_count[dsq_id][idx];
		break;
	case ERR_IDX:
		pval = &stats_data.err_idx[idx];
		break;
	case PCP_TIMEOUT_CNT:
		pval = &stats_data.pcp_timeout_cnt[cpu];
		break;
	case PCP_LDSQ_CNT:
		pval = &stats_data.pcp_ldsq_count[cpu][idx];
		break;
	case PCP_ENQL_CNT:
		pval = &stats_data.pcp_enql_cnt[cpu];
		break;
	default:
		return;
	}

	spin_lock_irqsave(&stats_lock, flags);
	*pval += 1;
	spin_unlock_irqrestore(&stats_lock, flags);
}

#define PRINT_INTV	(5 * HZ)
void stats_print(char *buf, int len)
{
	int idx = 0, j, ret;
	int item = 0;
	u64 *pval;
	u64 *pbase = (u64 *)&stats_data;

	for (item = 0; item < MAX_ITEMS; item++) {
		if (item <= DWORD_STAT_END) {
			pval = pbase + item * 2;
			ret = snprintf(&buf[idx], len - idx, "%s:%llu, %llu\n",
					stats_str[item], pval[0],  pval[1]);
			if (ret < 0 || ret >= len - idx)
				return;
			idx += ret;
		} else if (item == GDSQ_CNT) {
			for (j = 0; j < MAX_GLOBAL_DSQS; j++) {
				pval = (u64 *)&stats_data.gdsq_count[j];
				ret = snprintf(&buf[idx], len - idx, "%s[%d]:%llu, %llu\n",
						stats_str[item], j, pval[0], pval[1]);
				if (ret < 0 || ret >= len - idx)
					return;
				idx += ret;
			}
		} else if (item == ERR_IDX) {
			pval = (u64 *)&stats_data.err_idx;
			ret = snprintf(&buf[idx], len - idx, "%s:%llu, %llu, %llu, %llu, %llu\n",
						stats_str[item], pval[0],
						pval[1], pval[2], pval[3], pval[4]);
			if (ret < 0 || ret >= len - idx)
				return;
			idx += ret;
		} else if (item == PCP_TIMEOUT_CNT) {
			for (j = 0; j < nr_cpu_ids; j++) {
				pval = (u64 *)&stats_data.pcp_timeout_cnt[j];
				ret = snprintf(&buf[idx], len - idx, "%s[%d]:%llu\n",
							stats_str[item], j, *pval);
				if (ret < 0 || ret >= len - idx)
					return;
				idx += ret;
			}
		} else if (item == PCP_LDSQ_CNT) {
			for (j = 0; j < nr_cpu_ids; j++) {
				pval = (u64 *)&stats_data.pcp_ldsq_count[j];
				ret = snprintf(&buf[idx], len - idx, "%s[%d]:%llu,%llu\n",
						stats_str[item], j, pval[0], pval[1]);
				if (ret < 0 || ret >= len - idx)
					return;
				idx += ret;
			}
		} else if (item == PCP_ENQL_CNT) {
			for (j = 0; j < nr_cpu_ids; j++) {
				pval = (u64 *)&stats_data.pcp_enql_cnt[j];
				ret = snprintf(&buf[idx], len - idx, "%s[%d]:%llu\n",
						stats_str[item], j, *pval);
				if (ret < 0 || ret >= len - idx)
					return;
				idx += ret;
			}
		}
	}
	buf[idx] = '\0';
}

enum cpu_type {
	LITTLE,
	BIG,
	PARTIAL,
	EXCLUSIVE,
	EX_FREE,
	INVALID
};

enum dsq_type {
	GLOBAL_DSQ,
	PCP_DSQ,
	OTHER,
	MAX_DSQ_TYPE,
};

static enum cpu_type cpu_cluster(int cpu)
{
	struct rq *rq = cpu_rq(cpu);

	if (get_hmbird_rq(rq)->exclusive) {
		return EXCLUSIVE;
	} else {
		if (cpumask_test_cpu(cpu, iso_masks.little))
			return LITTLE;
		else if (cpumask_test_cpu(cpu, iso_masks.big))
			return BIG;
		else if (cpumask_test_cpu(cpu, iso_masks.partial))
			return PARTIAL;
		else if (cpumask_test_cpu(cpu, iso_masks.exclusive))
			return EXCLUSIVE;
		else if (cpumask_test_cpu(cpu, iso_masks.ex_free))
			return EX_FREE;
	}
	return INVALID;
}

static enum dsq_type get_dsq_type(struct hmbird_dispatch_q *dsq)
{
	if (!dsq)
		return OTHER;

	if ((dsq->id & HMBIRD_DSQ_FLAG_BUILTIN) &&
		((dsq->id & 0xff) >= GDSQS_ID_BASE) &&
		((dsq->id & 0xff) < MAX_GLOBAL_DSQS))
		return GLOBAL_DSQ;

	if ((dsq->id & HMBIRD_DSQ_FLAG_BUILTIN) &&
		((dsq->id & 0xff) >= MAX_GLOBAL_DSQS) &&
		((dsq->id & 0xff) < max_hmbird_dsq_internal_id))
		return PCP_DSQ;

	return OTHER;
}

static int dsq_id_to_internal(struct hmbird_dispatch_q *dsq)
{
	enum dsq_type type;

	type = get_dsq_type(dsq);
	switch (type) {
	case GLOBAL_DSQ:
	case PCP_DSQ:
		return (dsq->id & 0xff) - GDSQS_ID_BASE;
	default:
		return -1;
	}
	return -1;
}

static void update_cpus_idle(bool set, struct cpumask *mask)
{
	int cpu;
	struct rq *rq;

	if (set)
	{
		for_each_cpu(cpu, mask) {
			rq = cpu_rq(cpu);
			if (is_idle_task(rq->curr))
				cpumask_set_cpu(cpu, idle_masks.cpu);
		}
	} else 
		cpumask_andnot(idle_masks.cpu, idle_masks.cpu, mask);
}

/*
 * Need more synchronization for these two variables?
 * I choose not to.
 */
static int l_need_rescue, b_need_rescue;
static void set_partial_status(bool enable, bool little, bool big)
{
	WRITE_ONCE(partial_enable, enable);
	WRITE_ONCE(l_need_rescue, little);
	WRITE_ONCE(b_need_rescue, big);
}

static bool is_little_need_rescue(void)
{
	return READ_ONCE(l_need_rescue);
}

static bool is_big_need_rescue(void)
{
	return READ_ONCE(b_need_rescue);
}

static bool is_partial_enabled(void)
{
	return READ_ONCE(partial_enable);
}

static bool is_partial_cpu(int cpu)
{
	return cpumask_test_cpu(cpu, iso_masks.partial);
}

static void set_iso_par_free(bool enable)
{
	WRITE_ONCE(isolate_ctrl, enable);
}

static bool is_iso_par_free(void)
{
	return READ_ONCE(isolate_ctrl);
}

static bool skip_update_idle(void)
{
	int cpu = smp_processor_id();
	enum cpu_type type = cpu_cluster(cpu);

	if ((type == EXCLUSIVE && !is_iso_par_free()) ||
		/* partial enable may changed during idle, it doesn't matter. */
		(!is_partial_enabled() && type == PARTIAL))
		return true;

	return false;
}

static void init_isolate_cpus(void)
{
	WARN_ON(!alloc_cpumask_var(&iso_masks.ex_free, GFP_KERNEL));
	WARN_ON(!alloc_cpumask_var(&iso_masks.partial, GFP_KERNEL));
	WARN_ON(!alloc_cpumask_var(&iso_masks.exclusive, GFP_KERNEL));
	WARN_ON(!alloc_cpumask_var(&iso_masks.big, GFP_KERNEL));
	WARN_ON(!alloc_cpumask_var(&iso_masks.little, GFP_KERNEL));
	cpumask_set_cpu(0, iso_masks.big);
	cpumask_set_cpu(1, iso_masks.big);
	cpumask_set_cpu(2, iso_masks.big);
	cpumask_set_cpu(3, iso_masks.big);
	cpumask_set_cpu(4, iso_masks.big);
	cpumask_set_cpu(5, iso_masks.ex_free);
	cpumask_set_cpu(6, iso_masks.partial);
	cpumask_set_cpu(7, iso_masks.ex_free);
}

extern spinlock_t css_set_lock;

static struct cgroup *cgroup_ancestor_l1(struct cgroup *cgrp)
{
	int i;
	struct cgroup *anc;

	spin_lock_irq(&css_set_lock);
	for (i = 0; i < cgrp->level; i++) {
		anc = cgrp->ancestors[i];
		if (anc->level != CREATE_DSQ_LEVEL_WITHIN)
			continue;
		cgroup_get(anc);
		spin_unlock_irq(&css_set_lock);
		return anc;
	}
	spin_unlock_irq(&css_set_lock);
	hmbird_err("<fatal>:error cgroup = %s\n", cgrp->kn->name);
	return NULL;
}

#define PCP_IDX_BIT    (1 << 31)

static bool is_pcp_rt(struct task_struct *p)
{
	return rt_prio(p->prio) && (p->nr_cpus_allowed == 1);
}

static bool is_pcp_idx(int idx)
{
	return idx >= MAX_GLOBAL_DSQS;
}

static bool is_critical_system_task(struct task_struct *p)
{
	int sp_dl = get_hmbird_ts(p)->sched_prop & SCHED_PROP_DEADLINE_MASK;

	return (p->prio < (MAX_RT_PRIO >> 1) &&
		(sp_dl < SCHED_PROP_DEADLINE_LEVEL3));
}

#define ISOLATE_TASK_TOP_BIT	(1 << 17)
#define PIPELINE_TASK_TOP_BIT	(1 << 9)

static bool is_pipeline_task(struct task_struct *p)
{
	return (get_top_task_prop(p) & PIPELINE_TASK_TOP_BIT);
}

static bool is_isolate_task(struct task_struct *p)
{
	return (get_top_task_prop(p) & ISOLATE_TASK_TOP_BIT);
}

static bool is_critical_app_task_without_isolate(struct task_struct *p)
{
	return task_is_top_task(p) && !is_isolate_task(p);
}

static int find_idx_from_task(struct task_struct *p)
{
	int idx, cpu;
	int sp_dl;
	struct task_group *tg = p->sched_task_group;

	if (p->nr_cpus_allowed == 1 || is_migration_disabled(p)) {
		cpu = cpumask_any(p->cpus_ptr);
		idx = cpu + MAX_GLOBAL_DSQS;
		return idx;
	}

	if (is_critical_system_task(p)) {
		idx = SCHED_PROP_DEADLINE_LEVEL0;
		goto done;
	}

	if (is_critical_app_task_without_isolate(p)) {
		idx = SCHED_PROP_DEADLINE_LEVEL1;
		goto done;
	}

	sp_dl = hmbird_get_dsq_id(p);
	if (sp_dl) {
		idx = sp_dl;
		goto done;
	}

	if (rt_prio(p->prio)) {
		idx = SCHED_PROP_DEADLINE_LEVEL3;
		goto done;
	}

	if (tg && tg->css.cgroup && tg->css.cgroup->kn)
		idx = cgroup_ids_table[tg->css.cgroup->kn->id];
	else
		idx = DEFAULT_CGROUP_DL_IDX;

done:
	if (idx < 0 || idx >= MAX_GLOBAL_DSQS) {
		hmbird_err("<slim_sched><error> : idx error, idx = %d-----\n", idx);
		idx = DEFAULT_CGROUP_DL_IDX;
	}
	return idx;
}

static struct hmbird_dispatch_q *find_dsq_from_task(struct task_struct *p)
{
	int idx;
	unsigned long flags;
	struct hmbird_dispatch_q *dsq;

	if (!p)
		return NULL;

	idx = find_idx_from_task(p);
	if (is_pcp_idx(idx)) {
		idx -= MAX_GLOBAL_DSQS;
		dsq = &per_cpu(pcp_ldsq, idx);
		get_hmbird_ts(p)->gdsq_idx = dsq_id_to_internal(dsq);
		slim_stats_record(PCP_LDSQ_CNT, 0, 0, idx);
	} else {
		dsq = &gdsqs[idx];
		get_hmbird_ts(p)->gdsq_idx = idx;
		slim_stats_record(GDSQ_CNT, 0, idx, 0);
	}

	raw_spin_lock_irqsave(&dsq->lock, flags);
	if (list_empty(&dsq->fifo))
		dsq->last_consume_at = jiffies;

	raw_spin_unlock_irqrestore(&dsq->lock, flags);

	return dsq;
}


bool consume_dispatch_q(struct rq *rq, struct rq_flags *rf,
						struct hmbird_dispatch_q *dsq);

static void set_partial_rescue(bool p_state, bool l_over, bool b_over)
{
	set_partial_status(p_state, l_over, b_over);
	update_cpus_idle(p_state, iso_masks.partial);
	hmbird_internal_systrace("C|9999|partial_enable|%d\n", is_partial_enabled());
	hmbird_internal_systrace("C|9999|l_need_rescue|%d\n", is_little_need_rescue());
	hmbird_internal_systrace("C|9999|b_need_rescue|%d\n", is_big_need_rescue());
}

static void free_isocpu(bool enable)
{
	set_iso_par_free(enable);
	update_cpus_idle(enable, iso_masks.ex_free);
	hmbird_internal_systrace("C|9999|free_iso|%d\n", is_iso_par_free());
}

inline u64 get_hmbird_cpu_util(int cpu)
{
		struct rq *rq = cpu_rq(cpu);
		u64 prev_runnable_sum_fixed = *(u64 *)(get_hmbird_rq(rq)->prev_runnable_sum_fixed);
		u32 prev_window_size = *(u32 *)(get_hmbird_rq(rq)->prev_window_size);
		do_div(prev_runnable_sum_fixed, prev_window_size >> SCHED_CAPACITY_SHIFT);

		return prev_runnable_sum_fixed;
}

static u64 get_cpus_max_util(struct cpumask *mask)
{
	int cpu;
	u64 max = 0;
	u64 util, ratio;

	for_each_cpu(cpu, mask) {
				util = get_hmbird_cpu_util(cpu);
				ratio = util * 100 / arch_scale_cpu_capacity(cpu);
		hmbird_info_systrace("C|9999|Cpu%d_util|%llu\n", cpu, util);
		hmbird_info_systrace("C|9999|Cpu%d_cap|%llu\n", cpu, (u64)arch_scale_cpu_capacity(cpu));
				if (ratio > max)
						max = ratio;
		}
	return max;
}

static bool cluster_need_rescue(struct cpumask *mask, int hres)
{
	return get_cpus_max_util(mask) > hres;
}

void partial_dynamic_ctrl(void)
{
	u64 lmax = 0, bmax = 0;
	bool l_over, l_under, b_over, b_under;
	static bool last_l_over, last_b_over;
	static unsigned long last_check;

	/* Check partial every jiffies. need lock? */
	if (time_before_eq(jiffies, READ_ONCE(last_check)))
		return;

	WRITE_ONCE(last_check, jiffies);

	lmax = get_cpus_max_util(iso_masks.little);
	l_over = lmax > parctrl_high_ratio_l;
	l_under = lmax < parctrl_low_ratio_l;

	bmax = get_cpus_max_util(iso_masks.big);
	b_over = bmax > parctrl_high_ratio;
	b_under = bmax < parctrl_low_ratio;

	if (is_partial_enabled() && (l_over || b_over)) {
		if (last_l_over != l_over || last_b_over != b_over)
			set_partial_rescue(true, l_over, b_over);
	} else if (!is_partial_enabled() && (l_over || b_over)) {
		set_partial_rescue(true, l_over, b_over);
	} else if (is_partial_enabled() && l_under && b_under) {
		set_partial_rescue(false, false, false);
	}
	last_l_over = l_over;
	last_b_over = b_over;

	if (is_partial_enabled()) {
		bmax = get_cpus_max_util(iso_masks.partial);
		if (!is_iso_par_free() && bmax > isoctrl_high_ratio) {
			hmbird_info_trace("<par>partial max = %llu\n", bmax);
			free_isocpu(true);
		}
		else if (is_iso_par_free() && bmax < isoctrl_low_ratio)
			free_isocpu(false);
	} else if (is_iso_par_free()) {
		free_isocpu(false);
	}
}

static inline void slim_trace_show_cpu_consume_dsq_idx(unsigned int cpu, unsigned int idx)
{
	hmbird_info_systrace("C|9999|Cpu%d_dsq_id|%d\n", cpu, idx);
}

static int consume_target_dsq(struct rq *rq, struct rq_flags *rf, unsigned int idx)
{
	if (idx < 0 || idx >= MAX_GLOBAL_DSQS)
		return 0;

	if (consume_dispatch_q(rq, rf, &gdsqs[idx])) {
		slim_stats_record(GDSQ_CNT, 1, idx, 0);
		return 1;
	}
	return 0;
}

static int consume_period_dsq(struct rq *rq, struct rq_flags *rf)
{
	int i;

	for (i = 0; i < NON_PERIOD_START; i++) {
		if (consume_dispatch_q(rq, rf, &gdsqs[i])) {
			slim_stats_record(GDSQ_CNT, 1, i, 0);
			return 1;
		}
	}
	return 0;
}

static void update_timeout_stats(struct rq *rq, struct hmbird_dispatch_q *dsq, u64 deadline)
{
	struct hmbird_entity *entity;
	unsigned long flags;

	raw_spin_lock_irqsave(&dsq->lock, flags);
	if (list_empty(&dsq->fifo))
		goto clear_timeout;

	entity = list_first_entry(&dsq->fifo, struct hmbird_entity, dsq_node.fifo);
	if (time_before_eq(jiffies, entity->runnable_at + msecs_to_jiffies(deadline)))
		goto clear_timeout;

	raw_spin_unlock_irqrestore(&dsq->lock, flags);
	hmbird_info_trace(
				"dsq[%d] still timeout task-%s, jiffies = %lu, deadline = %lu, runnable at = %lu\n",
				dsq_id_to_internal(dsq), entity->task->comm,
				jiffies, msecs_to_jiffies(deadline), entity->runnable_at);
	hmbird_info_systrace("C|9999|dsq_%d_timeout|%d\n", dsq_id_to_internal(dsq), 1);
	return;

clear_timeout:
	hmbird_info_trace("dsq[%d] clear timeout\n",
				dsq_id_to_internal(dsq));
	hmbird_info_systrace("C|9999|dsq_%d_timeout|%d\n", dsq_id_to_internal(dsq), 0);
	dsq->is_timeout = false;
	slim_stats_record(PCP_TIMEOUT_CNT, 0, 0, cpu_of(rq));
	raw_spin_unlock_irqrestore(&dsq->lock, flags);
}

static void systrace_output_rtime_state(struct hmbird_dispatch_q *dsq, int rtime)
{
	hmbird_info_systrace("C|9999|dsq%d_rtime|%d\n", dsq_id_to_internal(dsq), rtime);
}

static int consume_pcp_dsq(struct rq *rq, struct rq_flags *rf, bool any)
{
	bool is_timeout;
	int cpu = cpu_of(rq);
	unsigned long flags;
	struct hmbird_dispatch_q *dsq = &per_cpu(pcp_ldsq, cpu);

	raw_spin_lock_irqsave(&dsq->lock, flags);
	is_timeout = dsq->is_timeout;
	raw_spin_unlock_irqrestore(&dsq->lock, flags);

	/*
	 * dsq->is_timeout may change here, let it be.
	 * it won't cause serious problems.
	 * the same for consume_dispatch_q later.
	 */
	if (!is_timeout && !any)
		return 0;

	if (consume_dispatch_q(rq, rf, dsq)) {
		if (is_timeout) {
			hmbird_info_trace("dsq[%d] consume a pcp timeout task\n",
						dsq_id_to_internal(dsq));
			update_timeout_stats(rq, dsq, pcp_dsq_deadline);
			slim_stats_record(PCP_TIMEOUT_CNT, 0, 0, cpu);
		}
		slim_stats_record(PCP_LDSQ_CNT, 1, 0, cpu);
		return 1;
	}
	/*
	 * No pcp task, clear quota.
	 */
	if (any) {
		if (per_cpu(pcp_info, cpu_of(rq)).pcp_round) {
			per_cpu(pcp_info, cpu).rtime = 0;
			per_cpu(pcp_info, cpu).pcp_round = false;
			hmbird_info_systrace("C|9999|pcp_%d_round|%d\n", cpu, false);
			systrace_output_rtime_state(&per_cpu(pcp_ldsq, cpu),
					per_cpu(pcp_info, cpu).rtime);
		}
	}
	return 0;
}

static int consume_timeout_dsq(struct rq *rq, struct rq_flags *rf, struct cluster_ctx *ctx)
{
	int i;
	bool is_timeout;
	unsigned long flags;

	/* Third param <false> means only consume timeout pcp dsq. */
	if (consume_pcp_dsq(rq, rf, false))
		return 1;

	for (i = ctx->lower; i < ctx->upper; i++) {
		raw_spin_lock_irqsave(&gdsqs[i].lock, flags);
		is_timeout = gdsqs[i].is_timeout;
		raw_spin_unlock_irqrestore(&gdsqs[i].lock, flags);
		/* gdsqs[i].is_timeout may change here, let it be... */
		if (is_timeout) {
			/*
			 * consume_dispatch_q will acquire dsq-lock,
			 * So cannot keep lock here, annoy enough.
			 * may rewrite a consume_dispatch_q_locked, TODO.
			 */
			if (consume_dispatch_q(rq, rf, &gdsqs[i])) {
				hmbird_info_trace("dsq[%d] consume a timeout task\n", i);
				slim_stats_record(TIMEOUT_CNT, ctx->tidx, 0, 0);
				update_timeout_stats(rq, &gdsqs[i], HMBIRD_BPF_DSQS_DEADLINE[i]);
				return 1;
			}
		}
	}
	return 0;
}

static int check_pcp_dsq_round(struct rq *rq, struct rq_flags *rf)
{
	if (per_cpu(pcp_info, cpu_of(rq)).pcp_round) {
		if (consume_pcp_dsq(rq, rf, true))
			return 1;
	}
	return 0;
}

static int check_non_period_dsq_phase(struct rq *rq, struct rq_flags *rf,
									int tmp, int cidx, int tidx)
{
	unsigned long flags;

	if (consume_dispatch_q(rq, rf, &gdsqs[tmp])) {
		if (tmp != cidx) {
			spin_lock(&sinfo.lock);
			sinfo.curr_idx[tidx] = tmp;
			spin_unlock(&sinfo.lock);
			hmbird_info_systrace("C|9999|cidx_%d|%d\n", tidx, sinfo.curr_idx[tidx]);
			slim_stats_record(SWITCH_IDX, 0, 0, 0);
		}
		slim_stats_record(GDSQ_CNT, 1, tmp, 0);

		raw_spin_lock_irqsave(&gdsqs[tmp].lock, flags);
		gdsqs[tmp].last_consume_at = jiffies;
		raw_spin_unlock_irqrestore(&gdsqs[tmp].lock, flags);
		return 1;
	}
	return 0;

}

static int get_cidx(struct cluster_ctx *ctx)
{
	int cidx;

	spin_lock(&sinfo.lock);
	cidx = sinfo.curr_idx[ctx->tidx];
	if (cidx < ctx->lower || cidx >= ctx->upper) {
		sinfo.curr_idx[ctx->tidx] = ctx->lower;
		hmbird_info_systrace("C|9999|cidx_%d|%d\n", ctx->tidx, sinfo.curr_idx[ctx->tidx]);
		slim_stats_record(ERR_IDX, ctx->tidx + 3, 0, 0);
		cidx = sinfo.curr_idx[ctx->tidx];
	}
	spin_unlock(&sinfo.lock);

	return cidx;
}


static int gen_cluster_ctx(struct cluster_ctx *ctx, enum cpu_type type)
{
	switch (type) {
	case PARTIAL:
		if (!is_partial_enabled())
			return -1;
		fallthrough;
#ifdef CLUSTER_SEPARATE
	case BIG:
		ctx->lower = NON_PERIOD_START;
		ctx->upper = CLUSTER_SEPARATE_IDX;
		ctx->tidx = 0;
		break;
	case LITTLE:
		ctx->lower = CLUSTER_SEPARATE_IDX;
		ctx->upper = NON_PERIOD_END;
		ctx->tidx = 1;
		break;
#else
	case BIG:
	case LITTLE:
		ctx->lower = NON_PERIOD_START;
		ctx->upper = NON_PERIOD_END;
		ctx->tidx = 0;
		break;
#endif
	default:
		WARN_ON(1);
		return -1;
	}
	return 0;
}

static int consume_non_period_dsq(struct rq *rq, struct rq_flags *rf, enum cpu_type type)
{
	struct cluster_ctx ctx;
	int cidx;
	int tmp;

	if (gen_cluster_ctx(&ctx, type))
		return 0;

	if (consume_timeout_dsq(rq, rf, &ctx))
		return 1;

	cidx = get_cidx(&ctx);
	tmp = cidx;
	do {
		if (check_pcp_dsq_round(rq, rf))
			return 1;
		if (check_non_period_dsq_phase(rq, rf, tmp, cidx, ctx.tidx))
			return 1;
		spin_lock(&sinfo.lock);
		sinfo.rtime[tmp] = 0;
		systrace_output_rtime_state(&gdsqs[tmp], sinfo.rtime[tmp]);
		tmp++;
		if (tmp >= ctx.upper) {
			atomic64_inc(&pcp_dsq_round);
			hmbird_info_systrace("C|9999|pcp_dsq_round|%lld\n",
							atomic64_read(&pcp_dsq_round));
			tmp = ctx.lower;
		}
		spin_unlock(&sinfo.lock);
	} while (tmp != cidx);

	return consume_pcp_dsq(rq, rf, true);
}

static bool consume_hmbird_global_dsq(struct rq *rq, struct rq_flags *rf)
{
	enum cpu_type type = cpu_cluster(cpu_of(rq));
	bool period_allowed = !get_hmbird_rq(rq)->period_disallow;
	bool non_period_allowed = !get_hmbird_rq(rq)->nonperiod_disallow;

	switch (type) {
	case EX_FREE:
		if (consume_period_dsq(rq, rf))
			return 1;
		if (consume_non_period_dsq(rq, rf, BIG))
			return 1;
		if (is_little_need_rescue())
			if (consume_non_period_dsq(rq, rf, LITTLE))
				return 1;
		break;
	case EXCLUSIVE:
		if (!is_iso_par_free()) {
			if (consume_pcp_dsq(rq, rf, true))
				return 1;
		break;
		}
		/* Only non-period task can run on isolate cpus */
		if (!READ_ONCE(iso_free_rescue)) {
			if (is_big_need_rescue())
				if (consume_non_period_dsq(rq, rf, BIG))
					return 1;
			if (is_little_need_rescue())
				if (consume_non_period_dsq(rq, rf, LITTLE))
					return 1;
		} else {
		/* Free run, can run on any task. */
			if (consume_period_dsq(rq, rf))
				return 1;
			if (consume_non_period_dsq(rq, rf, BIG))
				return 1;
			if (consume_non_period_dsq(rq, rf, LITTLE))
				return 1;
		}
		if (consume_pcp_dsq(rq, rf, true))
			return 1;
		break;
	case PARTIAL:
		if (!is_partial_enabled()) {
			if (consume_pcp_dsq(rq, rf, true))
				return 1;
			return 0;
		}
		if (is_big_need_rescue()) {
			if (period_allowed && consume_period_dsq(rq, rf))
				return 1;
			if (non_period_allowed && consume_non_period_dsq(rq, rf, BIG))
				return 1;
		}
		if (is_little_need_rescue()) {
			if (consume_target_dsq(rq, rf, SCHED_PROP_DEADLINE_LEVEL0))
				return 1;
			if (consume_target_dsq(rq, rf, SCHED_PROP_DEADLINE_LEVEL2))
				return 1;
			if (consume_target_dsq(rq, rf, SCHED_PROP_DEADLINE_LEVEL3))
				return 1;
			if (consume_target_dsq(rq, rf, SCHED_PROP_DEADLINE_LEVEL4))
				return 1;
			if (non_period_allowed && consume_non_period_dsq(rq, rf, LITTLE))
				return 1;
		}
		if (consume_pcp_dsq(rq, rf, true))
			return 1;
		break;

	case BIG:
		if (period_allowed && consume_period_dsq(rq, rf))
			return 1;
		if (non_period_allowed && consume_non_period_dsq(rq, rf, type))
			return 1;
		if (is_iso_par_free() || (is_little_need_rescue() &&
			!cluster_need_rescue(iso_masks.big, parctrl_high_ratio))) {
			if (consume_non_period_dsq(rq, rf, LITTLE)) {
				hmbird_internal_systrace("C|9999|b_rescue_l|%d\n", b_rescue_l++);
				return 1;
			}
		}
		break;

	case LITTLE:
		if (consume_target_dsq(rq, rf, SCHED_PROP_DEADLINE_LEVEL0))
			return 1;
		if (consume_target_dsq(rq, rf, SCHED_PROP_DEADLINE_LEVEL2))
			return 1;
		if (consume_target_dsq(rq, rf, SCHED_PROP_DEADLINE_LEVEL3))
			return 1;
		if (consume_target_dsq(rq, rf, SCHED_PROP_DEADLINE_LEVEL4))
			return 1;

		if (non_period_allowed && consume_non_period_dsq(rq, rf, type))
			return 1;
		if (is_iso_par_free() || (is_big_need_rescue() &&
			!cluster_need_rescue(iso_masks.little, parctrl_high_ratio_l))) {
			if (consume_non_period_dsq(rq, rf, BIG)) {
				hmbird_internal_systrace("C|9999|l_rescue_b|%d\n", l_rescue_b++);
				return 1;
			}
		}
		break;

	default:
		break;
	}
	return 0;
}

static int consume_dispatch_global(struct rq *rq, struct rq_flags *rf)
{
	return consume_hmbird_global_dsq(rq, rf);
}


static void update_runningtime(struct rq *rq, struct task_struct *p, unsigned long exec_time)
{
	int idx;

	/* which dsq belongs to while task enqueue, task will consume its running time. */
	idx = get_hmbird_ts(p)->gdsq_idx;
	/* Only non-period dsq share running time between each other. */
	if (idx < NON_PERIOD_START || idx >= max_hmbird_dsq_internal_id)
		return;

	if (idx >= MAX_GLOBAL_DSQS) {
		per_cpu(pcp_info, cpu_of(rq)).rtime += exec_time;
		systrace_output_rtime_state(&per_cpu(pcp_ldsq, cpu_of(rq)),
					per_cpu(pcp_info, cpu_of(rq)).rtime);
	} else {
		spin_lock(&sinfo.lock);
		sinfo.rtime[idx] += exec_time;
		spin_unlock(&sinfo.lock);
		systrace_output_rtime_state(&gdsqs[idx], sinfo.rtime[idx]);
	}
}

static void update_dsq_idx(struct rq *rq, struct task_struct *p, enum cpu_type type)
{
	int cidx;
	struct cluster_ctx ctx;
	int cpu = cpu_of(rq);

	if (gen_cluster_ctx(&ctx, type))
		return;

	spin_lock(&sinfo.lock);
	cidx = sinfo.curr_idx[ctx.tidx];
	if (cidx < ctx.lower || cidx >= ctx.upper) {
		sinfo.curr_idx[ctx.tidx] = ctx.lower;
		hmbird_info_systrace("C|9999|cidx_%d|%d\n", ctx.tidx, sinfo.curr_idx[ctx.tidx]);
		slim_stats_record(ERR_IDX, ctx.tidx, 0, 0);
		cidx = sinfo.curr_idx[ctx.tidx];
	}

	while (1) {
		if (per_cpu(pcp_info, cpu).pcp_round) {
			if (per_cpu(pcp_info, cpu).rtime >= pcp_dsq_quota) {
				hmbird_info_trace("cpu[%d] pcp_dsq_round is full, rtime = %d\n",
								cpu, per_cpu(pcp_info, cpu).rtime);
				per_cpu(pcp_info, cpu).rtime = 0;
				per_cpu(pcp_info, cpu).pcp_round = false;
				hmbird_info_systrace("C|9999|pcp_%d_round|%d\n", cpu, false);
				systrace_output_rtime_state(&per_cpu(pcp_ldsq, cpu),
						per_cpu(pcp_info, cpu_of(rq)).rtime);
			}
		}
		if (sinfo.rtime[cidx] < dsq_quota[cidx])
			break;

		/* clear current dsq rtime */
		sinfo.rtime[cidx] = 0;
		systrace_output_rtime_state(&gdsqs[cidx], sinfo.rtime[cidx]);

		sinfo.curr_idx[ctx.tidx]++;
		hmbird_info_systrace("C|9999|cidx_%d|%d\n", ctx.tidx, sinfo.curr_idx[ctx.tidx]);
		if (sinfo.curr_idx[ctx.tidx] >= ctx.upper) {
			atomic64_inc(&pcp_dsq_round);
			hmbird_info_systrace("C|9999|pcp_dsq_round|%lld\n",
								atomic64_read(&pcp_dsq_round));
			sinfo.curr_idx[ctx.tidx] = ctx.lower;
			hmbird_info_systrace("C|9999|cidx_%d|%d\n",
								ctx.tidx, sinfo.curr_idx[ctx.tidx]);
		}
		cidx = sinfo.curr_idx[ctx.tidx];
		slim_stats_record(SWITCH_IDX, 1, 0, 0);
	}
	spin_unlock(&sinfo.lock);
}


static void update_dispatch_dsq_info(struct rq *rq, struct task_struct *p)
{
	enum cpu_type type;

	if (!rq || !p)
		return;

	type = cpu_cluster(cpu_of(rq));
	switch (type) {
	case PARTIAL:
		return;
	case EXCLUSIVE:
		return;
	case EX_FREE:
		return;
	default:
		break;
	}
	update_dsq_idx(rq, p, type);
}


static bool scan_dsq_timeout(struct rq *rq, struct hmbird_dispatch_q *dsq, u64 deadline)
{
	struct hmbird_entity *entity;

	raw_spin_lock(&dsq->lock);
	if (list_empty(&dsq->fifo) || dsq->is_timeout) {
		raw_spin_unlock(&dsq->lock);
		return false;
	}

	entity = list_first_entry(&dsq->fifo, struct hmbird_entity, dsq_node.fifo);
	if (!entity) {
		WARN_ON(1);
		raw_spin_unlock(&dsq->lock);
		return false;
	}

	if (time_before_eq(jiffies, entity->runnable_at + msecs_to_jiffies(deadline))) {
		raw_spin_unlock(&dsq->lock);
		return false;
	}

	dsq->is_timeout = true;
	hmbird_info_trace("dsq[%d] has timeout task-%s, jiffies = %lu, runnable at = %lu\n",
			dsq_id_to_internal(dsq), entity->task->comm, jiffies, entity->runnable_at);
	hmbird_info_systrace("C|9999|dsq_%d_timeout|%d\n", dsq_id_to_internal(dsq), 1);
	raw_spin_unlock(&dsq->lock);

	return true;
}

void scan_timeout(struct rq *rq)
{
	int i;
	int cpu = cpu_of(rq);
	struct hmbird_dispatch_q *dsq;
	static u64 last_scan_at;
	static DEFINE_PER_CPU(u64, pcp_last_scan_at);

	if (time_before_eq(jiffies, (unsigned long)per_cpu(pcp_last_scan_at, cpu)))
		return;
	per_cpu(pcp_last_scan_at, cpu) = jiffies;

	dsq = &per_cpu(pcp_ldsq, cpu);
	scan_dsq_timeout(rq, dsq, pcp_dsq_deadline);

	if (time_before_eq(jiffies, (unsigned long)last_scan_at))
		return;
	last_scan_at = jiffies;

	for (i = NON_PERIOD_START; i < NON_PERIOD_END; i++) {
		dsq = &gdsqs[i];
		scan_dsq_timeout(rq, dsq, HMBIRD_BPF_DSQS_DEADLINE[i]);
	}
}

/*******************************Initialize***********************************/

void init_dsq(struct hmbird_dispatch_q *dsq, u64 dsq_id);
static void init_dsq_at_boot(void)
{
	int i, cpu;

	for (i = 0; i < MAX_GLOBAL_DSQS; i++) {
		init_dsq(&gdsqs[i], (u64)HMBIRD_DSQ_FLAG_BUILTIN |
				(GDSQS_ID_BASE + i));
	}
	for_each_possible_cpu(cpu)
		init_dsq(&per_cpu(pcp_ldsq, cpu), (u64)HMBIRD_DSQ_FLAG_BUILTIN |
				(GDSQS_ID_BASE + i + cpu));

	max_hmbird_dsq_internal_id = GDSQS_ID_BASE + i + cpu;
	spin_lock_init(&sinfo.lock);
}

static inline void update_cgroup_ids_table(int ids, int hmbird_cgroup_deadline_idx)
{
	if (ids < 0 || ids >= NUMS_CGROUP_KINDS) {
		pr_err("update_cgroup_ids_tab idx err!\n");
		return;
	}
	cgroup_ids_table[ids] = hmbird_cgroup_deadline_idx;
}

static int cgrp_name_to_idx(struct cgroup *cgrp)
{
	int idx;

	if (!cgrp)
		return -1;

	if (!strcmp(cgrp->kn->name, "display")
			|| !strcmp(cgrp->kn->name, "multimedia"))
		idx = 5; /* 8ms */
	else if (!strcmp(cgrp->kn->name, "top-app")
			|| !strcmp(cgrp->kn->name, "ss-top"))
		idx = 6; /* 16ms */
	else if (!strcmp(cgrp->kn->name, "ssfg")
			|| !strcmp(cgrp->kn->name, "foreground"))
		idx = 7; /* 32ms */
	else if (!strcmp(cgrp->kn->name, "bg")
			|| !strcmp(cgrp->kn->name, "log")
			|| !strcmp(cgrp->kn->name, "dex2oat")
			|| !strcmp(cgrp->kn->name, "background"))
		idx = 9; /* 128ms */
	else
		idx = DEFAULT_CGROUP_DL_IDX; /* 64ms */

	return idx;
}

static void init_root_tg(struct cgroup  *cgrp, struct task_group *tg)
{
	if (!cgrp || !tg || !(cgrp->kn))
		return;
	update_cgroup_ids_table(cgrp->kn->id, DEFAULT_CGROUP_DL_IDX);
}

static void init_level1_tg(struct cgroup *cgrp, struct task_group *tg)
{
	if (!cgrp || !tg || !(cgrp->kn))
		return;

	if (cgroup_ids_table[cgrp->kn->id] == -1)
		update_cgroup_ids_table(cgrp->kn->id, cgrp_name_to_idx(cgrp));
}

static void init_child_tg(struct cgroup *cgrp, struct task_group *tg)
{
	struct cgroup *l1cgrp;

	if (!cgrp || !tg || !(cgrp->kn))
		return;

	l1cgrp = cgroup_ancestor_l1(cgrp);
	if (l1cgrp)
		update_cgroup_ids_table(cgrp->kn->id, cgrp_name_to_idx(l1cgrp));
	cgroup_put(l1cgrp);
}

static void cgrp_dsq_idx_init(struct cgroup *cgrp, struct task_group *tg)
{
	switch (cgrp->level) {
	case 0:
		init_root_tg(cgrp, tg);
		break;
	case 1:
		init_level1_tg(cgrp, tg);
		break;
	default:
		init_child_tg(cgrp, tg);
		break;
	}
}

/**************************************************************************/

static u64 tick_sched_clock;
static DECLARE_COMPLETION(tick_sched_clock_completion);

void hmbird_scheduler_tick(void)
{
	int cpu = smp_processor_id();
	struct rq *rq = cpu_rq(cpu);

	if (!atomic_cmpxchg(&set_sched_clock_prepare, true, false))
		return;

	if (unlikely(!tick_sched_clock)) {
		/*
		 * Let the window begin 20us prior to the tick,
		 * that way we are guaranteed a rollover when the tick occurs.
		 * Use rq->clock directly instead of rq_clock() since
		 * we do not have the rq lock and
		 * rq->clock was updated in the tick callpath.
		 */
		if (cmpxchg64(&tick_sched_clock, 0, rq->clock - 20000))
			return;
		for_each_possible_cpu(cpu) {
			struct hmbird_sched_rq_stats *srq = &per_cpu(hmbird_sched_rq_stats, cpu);

			srq->window_start = tick_sched_clock;
		}
		complete(&tick_sched_clock_completion);
	}
}


struct hmbird_task_iter {
	struct hmbird_entity		cursor;
	struct task_struct		*locked;
	struct rq			*rq;
	struct rq_flags			rf;
};

/**
 * hmbird_task_iter_init - Initialize a task iterator
 * @iter: iterator to init
 *
 * Initialize @iter. Must be called with hmbird_tasks_lock held. Once initialized,
 * @iter must eventually be exited with hmbird_task_iter_exit().
 *
 * hmbird_tasks_lock may be released between this and the first next() call or
 * between any two next() calls. If hmbird_tasks_lock is released between two
 * next() calls, the caller is responsible for ensuring that the task being
 * iterated remains accessible either through RCU read lock or obtaining a
 * reference count.
 *
 * All tasks which existed when the iteration started are guaranteed to be
 * visited as long as they still exist.
 */
static void hmbird_task_iter_init(struct hmbird_task_iter *iter)
{
	lockdep_assert_held(&hmbird_tasks_lock);

	iter->cursor = (struct hmbird_entity){ .flags = HMBIRD_TASK_CURSOR };
	list_add(&iter->cursor.tasks_node, &hmbird_tasks);
	iter->locked = NULL;
}

/**
 * hmbird_task_iter_exit - Exit a task iterator
 * @iter: iterator to exit
 *
 * Exit a previously initialized @iter. Must be called with hmbird_tasks_lock held.
 * If the iterator holds a task's rq lock, that rq lock is released. See
 * hmbird_task_iter_init() for details.
 */
static void hmbird_task_iter_exit(struct hmbird_task_iter *iter)
{
	struct list_head *cursor = &iter->cursor.tasks_node;

	lockdep_assert_held(&hmbird_tasks_lock);

	if (iter->locked) {
		task_rq_unlock(iter->rq, iter->locked, &iter->rf);
		iter->locked = NULL;
	}

	if (list_empty(cursor))
		return;

	list_del_init(cursor);
}

/**
 * hmbird_task_iter_next - Next task
 * @iter: iterator to walk
 *
 * Visit the next task. See hmbird_task_iter_init() for details.
 */
static struct task_struct *hmbird_task_iter_next(struct hmbird_task_iter *iter)
{
	struct list_head *cursor = &iter->cursor.tasks_node;
	struct hmbird_entity *pos;

	lockdep_assert_held(&hmbird_tasks_lock);

	list_for_each_entry(pos, cursor, tasks_node) {
		if (&pos->tasks_node == &hmbird_tasks)
			return NULL;
		if (!(pos->flags & HMBIRD_TASK_CURSOR)) {
			list_move(cursor, &pos->tasks_node);
			return pos->task;
		}
	}

	/* can't happen, should always terminate at hmbird_tasks above */
	WARN_ON(1);
	return NULL;
}

/**
 * hmbird_task_iter_next_filtered - Next non-idle task
 * @iter: iterator to walk
 *
 * Visit the next non-idle task. See hmbird_task_iter_init() for details.
 */
static struct task_struct *
hmbird_task_iter_next_filtered(struct hmbird_task_iter *iter)
{
	struct task_struct *p;

	while ((p = hmbird_task_iter_next(iter))) {
		if (!is_idle_task(p))
			return p;
	}
	return NULL;
}

/**
 * hmbird_task_iter_next_filtered_locked - Next non-idle task with its rq locked
 * @iter: iterator to walk
 *
 * Visit the next non-idle task with its rq lock held. See hmbird_task_iter_init()
 * for details.
 */
static struct task_struct *
hmbird_task_iter_next_filtered_locked(struct hmbird_task_iter *iter)
{
	struct task_struct *p;

	if (iter->locked) {
		task_rq_unlock(iter->rq, iter->locked, &iter->rf);
		iter->locked = NULL;
	}

	p = hmbird_task_iter_next_filtered(iter);
	if (!p)
		return NULL;

	iter->rq = task_rq_lock(p, &iter->rf);
	iter->locked = p;
	return p;
}

static enum hmbird_ops_enable_state hmbird_ops_enable_state(void)
{
	return atomic_read(&hmbird_ops_enable_state_var);
}

static enum hmbird_ops_enable_state
hmbird_ops_set_enable_state(enum hmbird_ops_enable_state to)
{
	return atomic_xchg(&hmbird_ops_enable_state_var, to);
}

static bool hmbird_ops_tryset_enable_state(enum hmbird_ops_enable_state to,
					enum hmbird_ops_enable_state from)
{
	int from_v = from;

	return atomic_try_cmpxchg(&hmbird_ops_enable_state_var, &from_v, to);
}

static bool hmbird_ops_disabling(void)
{
	return false;
/*	return unlikely(hmbird_ops_enable_state() == HMBIRD_OPS_DISABLING);*/
}

/**
 * wait_ops_state - Busy-wait the specified ops state to end
 * @p: target task
 * @opss: state to wait the end of
 *
 * Busy-wait for @p to transition out of @opss. This can only be used when the
 * state part of @opss is %HMBIRD_QUEUEING or %HMBIRD_DISPATCHING. This function also
 * has load_acquire semantics to ensure that the caller can see the updates made
 * in the enqueueing and dispatching paths.
 */
static void wait_ops_state(struct task_struct *p, u64 opss)
{
	do {
		cpu_relax();
	} while (atomic64_read_acquire(&(get_hmbird_ts(p)->ops_state)) == opss);
}


static void update_curr_hmbird(struct rq *rq)
{
	struct task_struct *curr = rq->curr;
	u64 now = rq_clock_task(rq);
	u64 delta_exec;

	if (time_before_eq64(now, curr->se.exec_start))
		return;

	delta_exec = now - curr->se.exec_start;
	curr->se.exec_start = now;
	update_runningtime(rq, curr, delta_exec);
	curr->se.sum_exec_runtime += delta_exec;
	account_group_exec_runtime(curr, delta_exec);
	cgroup_account_cputime(curr, delta_exec);

	if (get_hmbird_ts(curr)->slice != HMBIRD_SLICE_INF)
		get_hmbird_ts(curr)->slice -= min(get_hmbird_ts(curr)->slice, delta_exec);

	trace_sched_stat_runtime(curr, delta_exec, 0);
}

static bool hmbird_dsq_priq_less(struct rb_node *node_a,
					const struct rb_node *node_b)
{
	const struct hmbird_entity *a =
		container_of(node_a, struct hmbird_entity, dsq_node.priq);
	const struct hmbird_entity *b =
		container_of(node_b, struct hmbird_entity, dsq_node.priq);

	return time_before64(a->dsq_vtime, b->dsq_vtime);
}

static void dispatch_enqueue(struct hmbird_dispatch_q *dsq, struct task_struct *p,
							u64 enq_flags)
{
	bool is_local = dsq->id == HMBIRD_DSQ_LOCAL;
	unsigned long flags;

	WARN_ON_ONCE(get_hmbird_ts(p)->dsq || !list_empty(&get_hmbird_ts(p)->dsq_node.fifo));
	WARN_ON_ONCE((get_hmbird_ts(p)->dsq_flags & HMBIRD_TASK_DSQ_ON_PRIQ) ||
			!RB_EMPTY_NODE(&get_hmbird_ts(p)->dsq_node.priq));

	if (!is_local) {
		raw_spin_lock_irqsave(&dsq->lock, flags);
		if (unlikely(dsq->id == HMBIRD_DSQ_INVALID)) {
			hmbird_ops_error("attempting to dispatch to a destroyed dsq");
			/* fall back to the global dsq */
			raw_spin_unlock_irqrestore(&dsq->lock, flags);
			dsq = &hmbird_dsq_global;
			raw_spin_lock_irqsave(&dsq->lock, flags);
		}
	}

	if (enq_flags & HMBIRD_ENQ_DSQ_PRIQ) {
		get_hmbird_ts(p)->dsq_flags |= HMBIRD_TASK_DSQ_ON_PRIQ;
		rb_add_cached(&get_hmbird_ts(p)->dsq_node.priq, &dsq->priq,
					hmbird_dsq_priq_less);
	} else {
		if (enq_flags & (HMBIRD_ENQ_HEAD | HMBIRD_ENQ_PREEMPT))
			list_add(&get_hmbird_ts(p)->dsq_node.fifo, &dsq->fifo);
		else
			list_add_tail(&get_hmbird_ts(p)->dsq_node.fifo, &dsq->fifo);
	}
	dsq->nr++;
	get_hmbird_ts(p)->dsq = dsq;

	/*
	 * We're transitioning out of QUEUEING or DISPATCHING. store_release to
	 * match waiters' load_acquire.
	 */
	if (enq_flags & HMBIRD_ENQ_CLEAR_OPSS)
		atomic64_set_release(&get_hmbird_ts(p)->ops_state, HMBIRD_OPSS_NONE);

	if (is_local) {
		struct hmbird_rq *hmbird = container_of(dsq, struct hmbird_rq, local_dsq);
		struct rq *rq = hmbird->rq;
		bool preempt = false;

		if ((enq_flags & HMBIRD_ENQ_PREEMPT) && p != rq->curr &&
			rq->curr->sched_class == &hmbird_sched_class) {
			get_hmbird_ts(rq->curr)->slice = 0;
			preempt = true;
		}

		if (preempt || sched_class_above(&hmbird_sched_class,
						rq->curr->sched_class))
			resched_curr(rq);
	} else {
		raw_spin_unlock_irqrestore(&dsq->lock, flags);
	}
}

static void task_unlink_from_dsq(struct task_struct *p,
				struct hmbird_dispatch_q *dsq)
{
	if (get_hmbird_ts(p)->dsq_flags & HMBIRD_TASK_DSQ_ON_PRIQ) {
		rb_erase_cached(&get_hmbird_ts(p)->dsq_node.priq, &dsq->priq);
		RB_CLEAR_NODE(&get_hmbird_ts(p)->dsq_node.priq);
		get_hmbird_ts(p)->dsq_flags &= ~HMBIRD_TASK_DSQ_ON_PRIQ;
	} else {
		list_del_init(&get_hmbird_ts(p)->dsq_node.fifo);
	}
}

static bool task_linked_on_dsq(struct task_struct *p)
{
	return !list_empty(&get_hmbird_ts(p)->dsq_node.fifo) ||
		!RB_EMPTY_NODE(&get_hmbird_ts(p)->dsq_node.priq);
}

static void dispatch_dequeue(struct hmbird_rq *hmbird_rq, struct task_struct *p)
{
	unsigned long flags;
	struct hmbird_dispatch_q *dsq = get_hmbird_ts(p)->dsq;
	bool is_local = dsq == &hmbird_rq->local_dsq;

	if (!dsq) {
		WARN_ON_ONCE(task_linked_on_dsq(p));
		/*
		 * When dispatching directly from the BPF scheduler to a local
		 * DSQ, the task isn't associated with any DSQ but
		 * @get_hmbird_ts(p)->holding_cpu may be set under the protection of
		 * %HMBIRD_OPSS_DISPATCHING.
		 */
		if (get_hmbird_ts(p)->holding_cpu >= 0)
			get_hmbird_ts(p)->holding_cpu = -1;
		return;
	}

	if (!is_local)
		raw_spin_lock_irqsave(&dsq->lock, flags);

	/*
	 * Now that we hold @dsq->lock, @p->holding_cpu and @get_hmbird_ts(p)->dsq_node
	 * can't change underneath us.
	 */
	if (get_hmbird_ts(p)->holding_cpu < 0) {
		/* @p must still be on @dsq, dequeue */
		WARN_ON_ONCE(!task_linked_on_dsq(p));
		task_unlink_from_dsq(p, dsq);
		dsq->nr--;
	} else {
		/*
		 * We're racing against dispatch_to_local_dsq() which already
		 * removed @p from @dsq and set @get_hmbird_ts(p)->holding_cpu. Clear the
		 * holding_cpu which tells dispatch_to_local_dsq() that it lost
		 * the race.
		 */
		WARN_ON_ONCE(task_linked_on_dsq(p));
		get_hmbird_ts(p)->holding_cpu = -1;
	}
	get_hmbird_ts(p)->dsq = NULL;

	if (!is_local)
		raw_spin_unlock_irqrestore(&dsq->lock, flags);
}


static bool test_rq_online(struct rq *rq)
{
#ifdef CONFIG_SMP
	return rq->online;
#else
	return true;
#endif
}

static void refill_task_slice(struct task_struct *p)
{
	if (is_isolate_task(p))
		get_hmbird_ts(p)->slice = HMBIRD_SLICE_ISO;
	else if (is_pipeline_task(p))
		get_hmbird_ts(p)->slice = HMBIRD_SLICE_ISO / 2;
	else
		get_hmbird_ts(p)->slice = HMBIRD_SLICE_DFL;
}

static void do_enqueue_task(struct rq *rq, struct task_struct *p, u64 enq_flags,
				int sticky_cpu)
{
	struct hmbird_dispatch_q *d;

	WARN_ON_ONCE(!test_bit(ffs(HMBIRD_TASK_QUEUED), (unsigned long *)&get_hmbird_ts(p)->flags));

	if (is_pcp_rt(p)) {
		/* Enqueue percpu rt task to local directly. */
		/* Or cause a bug when disable dispatch. */
		if (cpumask_test_cpu(cpu_of(rq), p->cpus_ptr))
			enq_flags |= HMBIRD_ENQ_LOCAL;
	}

	if (test_bit(ffs(HMBIRD_TASK_ENQ_LOCAL), (unsigned long *)&get_hmbird_ts(p)->flags)) {
		enq_flags |= HMBIRD_ENQ_LOCAL;
		clear_bit(ffs(HMBIRD_TASK_ENQ_LOCAL), (unsigned long *)&get_hmbird_ts(p)->flags);
	}
	/* rq migration */
	if (sticky_cpu == cpu_of(rq))
		goto local_norefill;
	/*
	 * If !rq->online, we already told the BPF scheduler that the CPU is
	 * offline. We're just trying to on/offline the CPU. Don't bother the
	 * BPF scheduler.
	 */
	if (unlikely(!test_rq_online(rq)))
		goto local;

	/* see %HMBIRD_OPS_ENQ_LAST */
	if (enq_flags & HMBIRD_ENQ_LAST)
		goto local;

	if (enq_flags & HMBIRD_ENQ_LOCAL)
		goto local;
	else
		goto global;
local:
	/* 
	 * For task-ordering, slice refill must be treated as implying the end
	 * of the current slice. Otherwise, the longer @p stays on the CPU, the
	 * higher priority it becomes from hmbird_prio_less()'s POV.
	 */
	refill_task_slice(p);
local_norefill:
	dispatch_enqueue(&get_hmbird_rq(rq)->local_dsq, p, enq_flags);
	slim_stats_record(PCP_ENQL_CNT, 0, 0, cpu_of(rq));
	return;

global:
	d = find_dsq_from_task(p);
	if (d) {
		refill_task_slice(p);
		dispatch_enqueue(d, p, enq_flags);
		return;
	}
	slim_stats_record(GLOBAL_STAT, 0, 0, 0);
	refill_task_slice(p);
	dispatch_enqueue(&hmbird_dsq_global, p, enq_flags);
}

static bool watchdog_task_watched(const struct task_struct *p)
{
	return !list_empty(&get_hmbird_ts(p)->watchdog_node);
}

static void watchdog_watch_task(struct rq *rq, struct task_struct *p)
{
	lockdep_assert_rq_held(rq);
	if (test_bit(ffs(HMBIRD_TASK_WATCHDOG_RESET), (unsigned long *)&get_hmbird_ts(p)->flags))
		get_hmbird_ts(p)->runnable_at = jiffies;
	clear_bit(ffs(HMBIRD_TASK_WATCHDOG_RESET), (unsigned long *)&get_hmbird_ts(p)->flags);
	list_add_tail(&get_hmbird_ts(p)->watchdog_node, &get_hmbird_rq(rq)->watchdog_list);
}

static void watchdog_unwatch_task(struct task_struct *p, bool reset_timeout)
{
	list_del_init(&get_hmbird_ts(p)->watchdog_node);
	if (reset_timeout)
		set_bit(ffs(HMBIRD_TASK_WATCHDOG_RESET), (unsigned long *)&get_hmbird_ts(p)->flags);
}

static void enqueue_task_hmbird(struct rq *rq, struct task_struct *p, int enq_flags)
{
	int sticky_cpu = get_hmbird_ts(p)->sticky_cpu;

	enq_flags |= get_hmbird_rq(rq)->extra_enq_flags;

	if (sticky_cpu >= 0)
		get_hmbird_ts(p)->sticky_cpu = -1;

	/*
	 * Restoring a running task will be immediately followed by
	 * set_next_task_hmbird() which expects the task to not be on the BPF
	 * scheduler as tasks can only start running through local DSQs. Force
	 * direct-dispatch into the local DSQ by setting the sticky_cpu.
	 */
	if (unlikely(enq_flags & ENQUEUE_RESTORE) && task_current(rq, p))
		sticky_cpu = cpu_of(rq);

	if (test_bit(ffs(HMBIRD_TASK_QUEUED), (unsigned long *)&get_hmbird_ts(p)->flags)) {
		WARN_ON_ONCE(!watchdog_task_watched(p));
		return;
	}

	watchdog_watch_task(rq, p);
	set_bit(ffs(HMBIRD_TASK_QUEUED), (unsigned long *)&get_hmbird_ts(p)->flags);
	get_hmbird_rq(rq)->nr_running++;
	add_nr_running(rq, 1);

	do_enqueue_task(rq, p, enq_flags, sticky_cpu);
}

static void ops_dequeue(struct task_struct *p, u64 deq_flags)
{
	u64 opss;

	watchdog_unwatch_task(p, false);

	/* acquire ensures that we see the preceding updates on QUEUED */
	opss = atomic64_read_acquire(&get_hmbird_ts(p)->ops_state);

	switch (opss & HMBIRD_OPSS_STATE_MASK) {
	case HMBIRD_OPSS_NONE:
		break;
	case HMBIRD_OPSS_QUEUEING:
		/*
		 * QUEUEING is started and finished while holding @p's rq lock.
		 * As we're holding the rq lock now, we shouldn't see QUEUEING.
		 */
		WARN_ON(1);
		break;
	case HMBIRD_OPSS_QUEUED:
		if (atomic64_try_cmpxchg(&get_hmbird_ts(p)->ops_state, &opss,
					 HMBIRD_OPSS_NONE))
			break;
		fallthrough;
	case HMBIRD_OPSS_DISPATCHING:
		/*
		 * If @p is being dispatched from the BPF scheduler to a DSQ,
		 * wait for the transfer to complete so that @p doesn't get
		 * added to its DSQ after dequeueing is complete.
		 *
		 * As we're waiting on DISPATCHING with the rq locked, the
		 * dispatching side shouldn't try to lock the rq while
		 * DISPATCHING is set. See dispatch_to_local_dsq().
		 *
		 * DISPATCHING shouldn't have qseq set and control can reach
		 * here with NONE @opss from the above QUEUED case block.
		 * Explicitly wait on %HMBIRD_OPSS_DISPATCHING instead of @opss.
		 */
		wait_ops_state(p, HMBIRD_OPSS_DISPATCHING);
		WARN_ON(atomic64_read(&get_hmbird_ts(p)->ops_state) != HMBIRD_OPSS_NONE);
		break;
	}
}

static void dequeue_task_hmbird(struct rq *rq, struct task_struct *p, int deq_flags)
{
	struct hmbird_rq *hmbird_rq = get_hmbird_rq(rq);

	if (!test_bit(ffs(HMBIRD_TASK_QUEUED), (unsigned long *)&get_hmbird_ts(p)->flags)) {
		WARN_ON_ONCE(watchdog_task_watched(p));
		return;
	}

	ops_dequeue(p, deq_flags);

	if (deq_flags & HMBIRD_DEQ_SLEEP)
		set_bit(ffs(HMBIRD_TASK_DEQD_FOR_SLEEP), (unsigned long *)&get_hmbird_ts(p)->flags);
	else
		clear_bit(ffs(HMBIRD_TASK_DEQD_FOR_SLEEP),
			(unsigned long *)&get_hmbird_ts(p)->flags);

	clear_bit(ffs(HMBIRD_TASK_QUEUED), (unsigned long *)&get_hmbird_ts(p)->flags);
	WARN_ON(!hmbird_rq->nr_running);
	hmbird_rq->nr_running--;
	sub_nr_running(rq, 1);
	dispatch_dequeue(hmbird_rq, p);
}

static void yield_task_hmbird(struct rq *rq)
{
	struct task_struct *p = rq->curr;

	get_hmbird_ts(p)->slice = 0;
}

static bool yield_to_task_hmbird(struct rq *rq, struct task_struct *to)
{
	return false;
}

#ifdef CONFIG_SMP
/**
 * move_task_to_local_dsq - Move a task from a different rq to a local DSQ
 * @rq: rq to move the task into, currently locked
 * @p: task to move
 * @enq_flags: %HMBIRD_ENQ_*
 *
 * Move @p which is currently on a different rq to @rq's local DSQ. The caller
 * must:
 *
 * 1. Start with exclusive access to @p either through its DSQ lock or
 *    %HMBIRD_OPSS_DISPATCHING flag.
 *
 * 2. Set @get_hmbird_ts(p)->holding_cpu to raw_smp_processor_id().
 *
 * 3. Remember task_rq(@p). Release the exclusive access so that we don't
 *    deadlock with dequeue.
 *
 * 4. Lock @rq and the task_rq from #3.
 *
 * 5. Call this function.
 *
 * Returns %true if @p was successfully moved. %false after racing dequeue and
 * losing.
 */
static bool move_task_to_local_dsq(struct rq *rq, struct task_struct *p,
					u64 enq_flags)
{
	struct rq *task_rq;

	lockdep_assert_rq_held(rq);

	/*
	 * If dequeue got to @p while we were trying to lock both rq's, it'd
	 * have cleared @get_hmbird_ts(p)->holding_cpu to -1. While other cpus may have
	 * updated it to different values afterwards, as this operation can't be
	 * preempted or recurse, @get_hmbird_ts(p)->holding_cpu can never become
	 * raw_smp_processor_id() again before we're done. Thus, we can tell
	 * whether we lost to dequeue by testing whether @get_hmbird_ts(p)->holding_cpu is
	 * still raw_smp_processor_id().
	 *
	 * See dispatch_dequeue() for the counterpart.
	 */
	if (unlikely(get_hmbird_ts(p)->holding_cpu != raw_smp_processor_id()))
		return false;

	/* @p->rq couldn't have changed if we're still the holding cpu */
	task_rq = task_rq(p);
	lockdep_assert_rq_held(task_rq);
	deactivate_task(task_rq, p, 0);
	set_task_cpu(p, cpu_of(rq));
	get_hmbird_ts(p)->sticky_cpu = cpu_of(rq);

	/*
	 * We want to pass hmbird-specific enq_flags but activate_task() will
	 * truncate the upper 32 bit. As we own @rq, we can pass them through
	 * @get_hmbird_rq(rq)->extra_enq_flags instead.
	 */
	WARN_ON_ONCE(get_hmbird_rq(rq)->extra_enq_flags);
	get_hmbird_rq(rq)->extra_enq_flags = enq_flags;
	activate_task(rq, p, 0);
	get_hmbird_rq(rq)->extra_enq_flags = 0;

	return true;
}

#endif	/* CONFIG_SMP */

static int task_fits_cpu_hmbird(struct task_struct *p, int cpu)
{
	int fitable = 1;

	return fitable;
}

static bool task_can_run_on_rq(struct task_struct *p, struct rq *rq, struct hmbird_dispatch_q *dsq)
{
	if (!task_fits_cpu_hmbird(p, cpu_of(rq)))
		return false;

	return likely(test_rq_online(rq));
}

static void set_skip_num(struct hmbird_dispatch_q *dsq, int *skipn, bool add)
{
	int idx = dsq_id_to_internal(dsq);
	int type = get_dsq_type(dsq);

	if (type != GLOBAL_DSQ)
		return;

	if (add)
		skipn[idx]++;
	else
		skipn[idx] = 0;
}

static bool skip_too_much(struct hmbird_dispatch_q *dsq)
{
	int idx = dsq_id_to_internal(dsq);
	int type = get_dsq_type(dsq);

	if (type != GLOBAL_DSQ)
		return false;

	if (skip_num[idx] > 3) {
		skip_num[idx] = 0;
		return true;
	}

	return false;
}

bool consume_dispatch_q(struct rq *rq, struct rq_flags *rf,
					struct hmbird_dispatch_q *dsq)
{
	struct hmbird_rq *hmbird_rq = get_hmbird_rq(rq);
	struct hmbird_entity *entity;
	struct task_struct *p;
	struct rb_node *rb_node;
	struct rq *task_rq;
	unsigned long flags;
	bool moved = false;
	struct task_struct *may_fit = NULL;
	int skip = 0;

retry:
	if (list_empty(&dsq->fifo) && !rb_first_cached(&dsq->priq))
		return false;

	raw_spin_lock_irqsave(&dsq->lock, flags);

	list_for_each_entry(entity, &dsq->fifo, dsq_node.fifo) {
		p = entity->task;
		task_rq = task_rq(p);
		if (!task_can_run_on_rq(p, rq, dsq))
			continue;
		if (rq == task_rq) {
			set_skip_num(dsq, skip_num, (bool)may_fit);
			goto this_rq;
		}
		if (skip_too_much(dsq))
			goto remote_rq;
		if (!may_fit)
			may_fit = p;
		if (++skip <= 3)
			continue;
		/*
		 * If the recent 3 tasks not fit, use the first one.
		 * and clear the skip, because the first one is consumed.
		 */
		set_skip_num(dsq, skip_num, false);
		p = may_fit;
		task_rq = task_rq(p);
		goto remote_rq;
	}
	/* No more task, use the first may fit task.*/
	if (may_fit) {
		p = may_fit;
		task_rq = task_rq(p);
		goto remote_rq;
	}

	for (rb_node = rb_first_cached(&dsq->priq); rb_node;
								rb_node = rb_next(rb_node)) {
		entity = container_of(rb_node, struct hmbird_entity, dsq_node.priq);
		p = entity->task;
		task_rq = task_rq(p);
		if (!task_can_run_on_rq(p, rq, dsq))
			continue;
		if (rq == task_rq)
			goto this_rq;
		goto remote_rq;
	}

	raw_spin_unlock_irqrestore(&dsq->lock, flags);
	return false;

this_rq:
	/* @dsq is locked and @p is on this rq */
	WARN_ON_ONCE(get_hmbird_ts(p)->holding_cpu >= 0);
	task_unlink_from_dsq(p, dsq);
	list_add_tail(&get_hmbird_ts(p)->dsq_node.fifo, &hmbird_rq->local_dsq.fifo);
	dsq->nr--;
	hmbird_rq->local_dsq.nr++;
	get_hmbird_ts(p)->dsq = &hmbird_rq->local_dsq;
	raw_spin_unlock_irqrestore(&dsq->lock, flags);
	slim_stats_record(TOTAL_DSP_CNT, 0, 0, 0);
	return true;

remote_rq:
#ifdef CONFIG_SMP
	if (cpu_same_cluster_stat(p, rq, task_rq))
		slim_stats_record(MOVE_RQ_CNT, 0, 0, 0);
	else
		slim_stats_record(MOVE_RQ_CNT, 1, 0, 0);
	/*
	 * @dsq is locked and @p is on a remote rq. @p is currently protected by
	 * @dsq->lock. We want to pull @p to @rq but may deadlock if we grab
	 * @task_rq while holding @dsq and @rq locks. As dequeue can't drop the
	 * rq lock or fail, do a little dancing from our side. See
	 * move_task_to_local_dsq().
	 */
	WARN_ON_ONCE(get_hmbird_ts(p)->holding_cpu >= 0);
	task_unlink_from_dsq(p, dsq);
	dsq->nr--;
	get_hmbird_ts(p)->holding_cpu = raw_smp_processor_id();
	raw_spin_unlock_irqrestore(&dsq->lock, flags);

	rq_unpin_lock(rq, rf);
	double_lock_balance(rq, task_rq);
	rq_repin_lock(rq, rf);

	moved = move_task_to_local_dsq(rq, p, 0);

	double_unlock_balance(rq, task_rq);
#endif /* CONFIG_SMP */
	if (likely(moved)) {
		slim_stats_record(TOTAL_DSP_CNT, 0, 0, 0);
		return true;
	}
	may_fit = NULL;
	goto retry;
}


static int balance_one(struct rq *rq, struct task_struct *prev,
				struct rq_flags *rf, bool local)
{
	struct hmbird_rq *hmbird_rq = get_hmbird_rq(rq);
	bool prev_on_hmbird = prev->sched_class == &hmbird_sched_class;

	if (!hmbird_rq)
		return 1;

	lockdep_assert_rq_held(rq);

	if (static_branch_unlikely(&hmbird_ops_cpu_preempt) &&
		unlikely(get_hmbird_rq(rq)->cpu_released)) {
		/*
		 * If the previous sched_class for the current CPU was not HMBIRD,
		 * notify the BPF scheduler that it again has control of the
		 * core. This callback complements ->cpu_release(), which is
		 * emitted in hmbird_notify_pick_next_task().
		 */
		get_hmbird_rq(rq)->cpu_released = false;
	}

	if (prev_on_hmbird)
		update_curr_hmbird(rq);
	/* if there already are tasks to run, nothing to do */
	if (hmbird_rq->local_dsq.nr)
		return 1;

	if (consume_dispatch_q(rq, rf, &hmbird_dsq_global)) {
		slim_stats_record(GLOBAL_STAT, 1, 0, 0);
		return 1;
	}

	if (consume_dispatch_global(rq, rf))
		return 1;

	return 0;
}

static int balance_hmbird(struct rq *rq, struct task_struct *prev,
						struct rq_flags *rf)
{
	return balance_one(rq, prev, rf, true);
}

static void set_next_task_hmbird(struct rq *rq, struct task_struct *p, bool first)
{
	if (test_bit(ffs(HMBIRD_TASK_QUEUED), (unsigned long *)&get_hmbird_ts(p)->flags)) {
		/*
		 * Core-sched might decide to execute @p before it is
		 * dispatched. Call ops_dequeue() to notify the BPF scheduler.
		 */
		ops_dequeue(p, HMBIRD_DEQ_CORE_SCHED_EXEC);
		dispatch_dequeue(get_hmbird_rq(rq), p);
	}

	p->se.exec_start = rq_clock_task(rq);

	watchdog_unwatch_task(p, true);
	slim_trace_show_cpu_consume_dsq_idx(smp_processor_id(), get_hmbird_ts(p)->gdsq_idx);
	/*
	 * @p is getting newly scheduled or got kicked after someone updated its
	 * slice. Refresh whether tick can be stopped. See can_stop_tick_hmbird().
	 */
	if ((get_hmbird_ts(p)->slice == HMBIRD_SLICE_INF) !=
	    (bool)(get_hmbird_rq(rq)->flags & HMBIRD_RQ_CAN_STOP_TICK)) {
		if (get_hmbird_ts(p)->slice == HMBIRD_SLICE_INF)
			get_hmbird_rq(rq)->flags |= HMBIRD_RQ_CAN_STOP_TICK;
		else
			get_hmbird_rq(rq)->flags &= ~HMBIRD_RQ_CAN_STOP_TICK;

		sched_update_tick_dependency(rq);
	}
}

static void put_prev_task_hmbird(struct rq *rq, struct task_struct *p)
{
	update_curr_hmbird(rq);

	update_dispatch_dsq_info(rq, p);

	slim_trace_show_cpu_consume_dsq_idx(smp_processor_id(), 0);

	if (test_bit(ffs(HMBIRD_TASK_QUEUED), (unsigned long *)&get_hmbird_ts(p)->flags)) {
		watchdog_watch_task(rq, p);

		if (is_pipeline_task(p)) {
			do_enqueue_task(rq, p, HMBIRD_ENQ_LOCAL, -1);
			return;
		}

		/*
		 * If we're in the pick_next_task path, balance_hmbird() should
		 * have already populated the local DSQ if there are any other
		 * available tasks. If empty, tell ops.enqueue() that @p is the
		 * only one available for this cpu. ops.enqueue() should put it
		 * on the local DSQ so that the subsequent pick_next_task_hmbird()
		 * can find the task unless it wants to trigger a separate
		 * follow-up scheduling event.
		 */
		if (list_empty(&get_hmbird_rq(rq)->local_dsq.fifo))
			do_enqueue_task(rq, p, HMBIRD_ENQ_LAST | HMBIRD_ENQ_LOCAL, -1);
		else
			do_enqueue_task(rq, p, 0, -1);
	}
}

static struct task_struct *first_local_task(struct rq *rq)
{
	struct rb_node *rb_node;
	struct hmbird_entity *entity;

	if (!list_empty(&get_hmbird_rq(rq)->local_dsq.fifo)) {
		entity = list_first_entry(&get_hmbird_rq(rq)->local_dsq.fifo,
							struct hmbird_entity, dsq_node.fifo);
		return entity->task;
	}

	rb_node = rb_first_cached(&get_hmbird_rq(rq)->local_dsq.priq);
	if (rb_node) {
		entity = container_of(rb_node, struct hmbird_entity, dsq_node.priq);
		return entity->task;
	}
	return NULL;
}

static struct task_struct *pick_next_task_hmbird(struct rq *rq)
{
	struct task_struct *p;

	p = first_local_task(rq);
	if (!p)
		return NULL;

	if (unlikely(!get_hmbird_ts(p)->slice)) {
		if (!hmbird_ops_disabling() && !hmbird_warned_zero_slice)
			hmbird_warned_zero_slice = true;

		refill_task_slice(p);
	}

	set_next_task_hmbird(rq, p, true);

	return p;
}

void __hmbird_notify_pick_next_task(struct rq *rq, struct task_struct *task,
				const struct sched_class *active)
{
	lockdep_assert_rq_held(rq);

	/*
	 * The callback is conceptually meant to convey that the CPU is no
	 * longer under the control of HMBIRD. Therefore, don't invoke the
	 * callback if the CPU is staying on HMBIRD, or going idle (in which
	 * case the HMBIRD scheduler has actively decided not to schedule any
	 * tasks on the CPU).
	 */
	if (likely(active >= &hmbird_sched_class))
		return;

	/*
	 * At this point we know that HMBIRD was preempted by a higher priority
	 * sched_class, so invoke the ->cpu_release() callback if we have not
	 * done so already. We only send the callback once between HMBIRD being
	 * preempted, and it regaining control of the CPU.
	 *
	 * ->cpu_release() complements ->cpu_acquire(), which is emitted the
	 *  next time that balance_hmbird() is invoked.
	 */
	if (!get_hmbird_rq(rq)->cpu_released)
		get_hmbird_rq(rq)->cpu_released = true;
}

#ifdef CONFIG_SMP

static bool test_and_clear_cpu_idle(int cpu)
{
	if (cpumask_test_and_clear_cpu(cpu, idle_masks.cpu)) {
		if (cpumask_empty(idle_masks.cpu))
			hmbird_has_idle_cpus = false;
		return true;
	} else {
		return false;
	}
}

static s32 hmbird_pick_idle_cpu(const struct cpumask *cpus_allowed)
{
	int cpu;

	do {
		cpu = cpumask_any_and_distribute(idle_masks.cpu, cpus_allowed);
		if (cpu >= nr_cpu_ids)
			return -EBUSY;
	} while (!test_and_clear_cpu_idle(cpu));

	return cpu;
}


static bool prev_cpu_misfit(int prev)
{
	if (!is_partial_enabled() && is_partial_cpu(prev))
		return true;

	return false;
}

static int heavy_rt_placement(struct task_struct *p, int prev)
{
	struct cpumask tmp = {.bits = {0}};
	int cpu;

	if (!rt_prio(p->prio))
		return -EFAULT;

	if (get_hmbird_ts(p)->demand_scaled < misfit_ds)
		return -EFAULT;

	if (is_partial_enabled())
		cpumask_or(&tmp, iso_masks.big, iso_masks.partial);
	else
		cpumask_copy(&tmp, iso_masks.big);

	cpu = hmbird_pick_idle_cpu(&tmp);
	if (cpu >= 0)
		return cpu;

	if (cpumask_test_cpu(prev, iso_masks.big) ||
		(is_partial_enabled() && is_partial_cpu(prev)))
		return prev;

	return cpumask_first(&tmp);
}

static int spec_task_before_pick_idle(struct task_struct *p, int prev)
{
	int cpu;

	cpu = heavy_rt_placement(p, prev);
	if (cpu >= 0)
		return cpu;
	return -EFAULT;
}

static int cpumask_distribute_next(struct cpumask *mask, int *prev)
{
	int p = *prev, n;

	n = find_next_bit_wrap(cpumask_bits(mask), nr_cpumask_bits, p + 1);
	if (n < nr_cpu_ids)
		WRITE_ONCE(*prev, n);
	return n;
}

static int repick_fallback_cpu(void)
{
	/*
	 * partial cpu follow big cluster's Scheduling policy,
	 * simply return first bit cpu.
	 */
	return cpumask_distribute_next(iso_masks.big, &big_distribute_mask_prev);
}

/*
 * Must return a valid cpu num, as this task's cpu.
 */
static int select_cpu_from_cluster(struct task_struct *p, int prev_cpu,
					struct cpumask *mask, int *prev_mask)
{
	int cpu;

	cpu = hmbird_pick_idle_cpu(mask);
	if (cpu >= 0)
		return cpu;
	return cpumask_distribute_next(mask, prev_mask);
}

static bool task_only_blongs_to_cluster(struct task_struct *p, enum cpu_type type)
{
	int idx;
	struct cluster_ctx ctx;

	idx = find_idx_from_task(p);
	if (idx < NON_PERIOD_START || idx >= MAX_GLOBAL_DSQS)
		return false;

	gen_cluster_ctx(&ctx, type);
	if (idx >= ctx.lower && idx < ctx.upper)
		return true;
	return false;
}

static s32 hmbird_select_cpu_dfl(struct task_struct *p, s32 prev_cpu, u64 wake_flags)
{
	s32 cpu = -1;
	struct cpumask mask = {.bits = {0}};

	cpu = get_hmbird_ts(p)->critical_affinity_cpu;
	if (cpu >= 0) {
		set_bit(ffs(HMBIRD_TASK_ENQ_LOCAL), (unsigned long *)&get_hmbird_ts(p)->flags);
		slim_stats_record(SELECT_CPU, 0, 0, 0);
		return cpu;
	}

	partial_dynamic_ctrl();

	if (is_critical_app_task_without_isolate(p) && !cpumask_empty(iso_masks.big)) {
		cpu = select_cpu_from_cluster(p, prev_cpu, iso_masks.big, &big_distribute_mask_prev);
		slim_stats_record(SELECT_CPU, 0, 0, 0);
		return cpu;
	}

	if (p->nr_cpus_allowed == 1) {
		cpu = cpumask_any(p->cpus_ptr);
		slim_stats_record(SELECT_CPU, 0, 0, 0);
		return cpu;
	}

	/* For non-period global dsq, not contain pcp task. */
	if (task_only_blongs_to_cluster(p, LITTLE)) {
		cpumask_copy(&mask, iso_masks.little);
		if (unlikely(l_need_rescue))
			cpumask_or(&mask, iso_masks.partial, &mask);
		if (!cpumask_empty(&mask)) {
			cpu = select_cpu_from_cluster(p, prev_cpu, &mask, &little_distribute_mask_prev);
			slim_stats_record(SELECT_CPU, 1, 0, 0);
			return cpu;
		}
	}

	if (task_only_blongs_to_cluster(p, BIG)) {
		cpumask_copy(&mask, iso_masks.big);
		if (unlikely(b_need_rescue))
			cpumask_or(&mask, iso_masks.partial, &mask);
		if (!cpumask_empty(&mask)) {
			cpu = select_cpu_from_cluster(p, prev_cpu, &mask, &big_distribute_mask_prev);
			slim_stats_record(SELECT_CPU, 1, 0, 0);
			return cpu;
		}
	}

	cpu = spec_task_before_pick_idle(p, prev_cpu);
	if (cpu >= 0) {
		slim_stats_record(SELECT_CPU, 0, 0, 0);
		return cpu;
	}

	/* if the previous CPU is idle, dispatch directly to it */
	if (test_and_clear_cpu_idle(prev_cpu)) {
		slim_stats_record(SELECT_CPU, 0, 0, 0);
		return prev_cpu;
	}

	cpu = hmbird_pick_idle_cpu(cpu_possible_mask);
	if (cpu >= 0) {
		slim_stats_record(SELECT_CPU, 0, 0, 0);
		return cpu;
	}

	if (prev_cpu_misfit(prev_cpu)) {
		slim_stats_record(SELECT_CPU, 0, 0, 0);
		return repick_fallback_cpu();
	}

	slim_stats_record(SELECT_CPU, 0, 0, 0);
	return prev_cpu;
}

static int select_task_rq_hmbird(struct task_struct *p, int prev_cpu, int wake_flags)
{
	return hmbird_select_cpu_dfl(p, prev_cpu, wake_flags);
}

static void set_cpus_allowed_hmbird(struct task_struct *p, struct affinity_context *ctx)
{
	set_cpus_allowed_common(p, ctx);
}

static void reset_idle_masks(void)
{
	cpumask_or(idle_masks.cpu, idle_masks.cpu, iso_masks.little);
	cpumask_or(idle_masks.cpu, idle_masks.cpu, iso_masks.big);
	if (is_partial_enabled())
		cpumask_or(idle_masks.cpu, idle_masks.cpu, iso_masks.partial);
	hmbird_has_idle_cpus = true;
}

void __hmbird_update_idle(struct rq *rq, bool idle)
{
	int cpu = cpu_of(rq);
	struct cpumask *sib_mask = topology_sibling_cpumask(cpu);

	if (skip_update_idle())
		return;

	if (idle) {
		cpumask_set_cpu(cpu, idle_masks.cpu);
		if (!hmbird_has_idle_cpus)
			hmbird_has_idle_cpus = true;

		/*
		 * idle_masks.smt handling is racy but that's fine as it's only
		 * for optimization and self-correcting.
		 */
		for_each_cpu(cpu, sib_mask) {
			if (!cpumask_test_cpu(cpu, idle_masks.cpu))
				return;
		}
		cpumask_or(idle_masks.smt, idle_masks.smt, sib_mask);
	} else {
		cpumask_clear_cpu(cpu, idle_masks.cpu);
		if (hmbird_has_idle_cpus && cpumask_empty(idle_masks.cpu))
			hmbird_has_idle_cpus = false;

		cpumask_andnot(idle_masks.smt, idle_masks.smt, sib_mask);
	}
}

#else /* !CONFIG_SMP */

static bool test_and_clear_cpu_idle(int cpu) { return false; }
static s32 hmbird_pick_idle_cpu(const struct cpumask *cpus_allowed) { return -EBUSY; }
static void reset_idle_masks(void) {}

#endif /* CONFIG_SMP */

static bool check_rq_for_timeouts(struct rq *rq)
{
	struct hmbird_entity *entity;
	struct task_struct *p;
	struct rq_flags rf;

	rq_lock_irqsave(rq, &rf);
	list_for_each_entry(entity, &get_hmbird_rq(rq)->watchdog_list, watchdog_node) {
		unsigned long last_runnable;

		p = entity->task;
		last_runnable = get_hmbird_ts(p)->runnable_at;

		if (unlikely(time_after(jiffies,
					last_runnable + hmbird_watchdog_timeout)) || watchdog_enable) {
			u32 dur_ms = jiffies_to_msecs(jiffies - last_runnable);

			rq_unlock_irqrestore(rq, &rf);
			hmbird_ops_error_type(HMBIRD_EXIT_ERROR_STALL,
						"%s[%d] failed to run for %u.%03us,get_hmbird_ts(p)->dsq->id = %llu, p->cpumask = %*pb",
						p->comm, p->pid,
						dur_ms / 1000, dur_ms % 1000,
						get_hmbird_ts(p)->dsq ?
						get_hmbird_ts(p)->dsq->id : 0,
						cpumask_pr_args(&p->cpus_mask));
			return 1;
		}
	}
	rq_unlock_irqrestore(rq, &rf);
	return 0;
}

static void hmbird_watchdog_workfn(struct work_struct *work)
{
	int cpu;
	bool timeout;

	hmbird_watchdog_timestamp = jiffies;

	for_each_online_cpu(cpu) {
		timeout = check_rq_for_timeouts(cpu_rq(cpu));
		if (unlikely(timeout))
			break;

		cond_resched();
	}
	if (!timeout)
		queue_delayed_work(system_unbound_wq, to_delayed_work(work),
						hmbird_watchdog_timeout / 2);
}

static void set_pcp_round(struct rq *rq)
{
	int cpu = cpu_of(rq);

	if (atomic64_read(&pcp_dsq_round) != per_cpu(pcp_info, cpu).pcp_seq) {
		per_cpu(pcp_info, cpu).pcp_seq = atomic64_read(&pcp_dsq_round);
		per_cpu(pcp_info, cpu).pcp_round = true;
		hmbird_info_systrace("C|9999|pcp_%d_round|%d\n", cpu, true);
		per_cpu(pcp_info, cpu).rtime = 0;
		systrace_output_rtime_state(&per_cpu(pcp_ldsq, cpu),
						per_cpu(pcp_info, cpu).rtime);
	}
}


/*
 * Just for debug: output hmbird on/off state per 10s.
 */
#define OUTPUT_INTVAL	(msecs_to_jiffies(10 * 1000))
static void inform_hmbird_onoff_from_systrace(void)
{
	static unsigned long __read_mostly next_print;

	if (time_before(jiffies, READ_ONCE(next_print)))
		return;

	WRITE_ONCE(next_print, jiffies + OUTPUT_INTVAL);
	hmbird_internal_systrace("C|9999|hmbird_status|%d\n", curr_ss);
}

static void task_tick_hmbird(struct rq *rq, struct task_struct *curr, int queued)
{
	update_curr_hmbird(rq);

	set_pcp_round(rq);
	/*
	 * While disabling, always resched and refresh core-sched timestamp as
	 * we can't trust the slice management or ops.core_sched_before().
	 */
	if (hmbird_ops_disabling())
		get_hmbird_ts(curr)->slice = 0;

	if (!get_hmbird_ts(curr)->slice)
		resched_curr(rq);

	inform_hmbird_onoff_from_systrace();
}

static int hmbird_ops_prepare_task(struct task_struct *p, struct task_group *tg)
{
	WARN_ON_ONCE(test_bit(ffs(HMBIRD_TASK_OPS_PREPPED),
			(unsigned long *)&get_hmbird_ts(p)->flags));

	get_hmbird_ts(p)->disallow = false;

	set_bit(ffs(HMBIRD_TASK_OPS_PREPPED), (unsigned long *)&get_hmbird_ts(p)->flags);
	set_bit(ffs(HMBIRD_TASK_WATCHDOG_RESET), (unsigned long *)&get_hmbird_ts(p)->flags);
	return 0;
}

static void hmbird_ops_enable_task(struct task_struct *p)
{
	lockdep_assert_rq_held(task_rq(p));
	WARN_ON_ONCE(!test_bit(ffs(HMBIRD_TASK_OPS_PREPPED),
			(unsigned long *)&get_hmbird_ts(p)->flags));

	clear_bit(ffs(HMBIRD_TASK_OPS_PREPPED), (unsigned long *)&get_hmbird_ts(p)->flags);
	set_bit(ffs(HMBIRD_TASK_OPS_ENABLED), (unsigned long *)&get_hmbird_ts(p)->flags);
}

static void hmbird_ops_disable_task(struct task_struct *p)
{
	lockdep_assert_rq_held(task_rq(p));

	if (test_bit(ffs(HMBIRD_TASK_OPS_PREPPED), (unsigned long *)&get_hmbird_ts(p)->flags))
		clear_bit(ffs(HMBIRD_TASK_OPS_PREPPED), (unsigned long *)&get_hmbird_ts(p)->flags);
	else if (test_bit(ffs(HMBIRD_TASK_OPS_ENABLED), (unsigned long *)&get_hmbird_ts(p)->flags))
		clear_bit(ffs(HMBIRD_TASK_OPS_ENABLED), (unsigned long *)&get_hmbird_ts(p)->flags);
}

static void set_task_hmbird_weight(struct task_struct *p)
{
	u32 weight = sched_prio_to_weight[p->static_prio - MAX_RT_PRIO];

	get_hmbird_ts(p)->weight = hmbird_sched_weight_to_cgroup(weight);
}

/**
 * refresh_hmbird_weight - Refresh a task's hmbird weight
 * @p: task to refresh hmbird weight for
 *
 * @get_hmbird_ts(p)->weight carries the task's static priority in cgroup weight scale to
 * enable easy access from the BPF scheduler. To keep it synchronized with the
 * current task priority, this function should be called when a new task is
 * created, priority is changed for a task on hmbird, and a task is switched
 * to hmbird from other classes.
 */
static void refresh_hmbird_weight(struct task_struct *p)
{
	lockdep_assert_rq_held(task_rq(p));
	set_task_hmbird_weight(p);
}

int hmbird_pre_fork(struct task_struct *p)
{
	int ret = 0;

	p->android_oem_data1[HMBIRD_TS_IDX] =
		(u64)(kmalloc(sizeof(struct hmbird_entity), GFP_KERNEL));
	if (!get_hmbird_ts(p)) {
		ret = -1;
		goto lock;
	}

	get_hmbird_ts(p)->dsq              = NULL;
	INIT_LIST_HEAD(&get_hmbird_ts(p)->dsq_node.fifo);
	RB_CLEAR_NODE(&get_hmbird_ts(p)->dsq_node.priq);
	INIT_LIST_HEAD(&get_hmbird_ts(p)->watchdog_node);
	get_hmbird_ts(p)->flags            = 0;
	get_hmbird_ts(p)->weight           = 0;
	get_hmbird_ts(p)->sticky_cpu       = -1;
	get_hmbird_ts(p)->holding_cpu      = -1;
	get_hmbird_ts(p)->kf_mask          = 0;
	atomic64_set(&get_hmbird_ts(p)->ops_state, 0);
	get_hmbird_ts(p)->runnable_at      = INITIAL_JIFFIES;
	get_hmbird_ts(p)->slice            = HMBIRD_SLICE_DFL;
	get_hmbird_ts(p)->task             = p;
	hmbird_set_sched_prop(p, 0);

	get_hmbird_ts(p)->critical_affinity_cpu = -1;
	get_hmbird_ts(p)->sched_class      = &hmbird_sched_class;

	/*
	 * BPF scheduler enable/disable paths want to be able to iterate and
	 * update all tasks which can become complex when racing forks. As
	 * enable/disable are very cold paths, let's use a percpu_rwsem to
	 * exclude forks.
	 */
lock:
	percpu_down_read(&hmbird_fork_rwsem);

	return ret;
}

int hmbird_fork(struct task_struct *p)
{
	percpu_rwsem_assert_held(&hmbird_fork_rwsem);

	if (hmbird_enabled())
		return hmbird_ops_prepare_task(p, task_group(p));
	else
		return 0;
}

void hmbird_post_fork(struct task_struct *p)
{
	if (hmbird_enabled()) {
		struct rq_flags rf;
		struct rq *rq;

		rq = task_rq_lock(p, &rf);
		/*
		 * Set the weight manually before calling ops.enable() so that
		 * the scheduler doesn't see a stale value if they inspect the
		 * task struct. We'll invoke ops.set_weight() afterwards, as it
		 * would be odd to receive a callback on the task before we
		 * tell the scheduler that it's been fully enabled.
		 */
		set_task_hmbird_weight(p);
		hmbird_ops_enable_task(p);
		refresh_hmbird_weight(p);
		task_rq_unlock(rq, p, &rf);
	}

	spin_lock_irq(&hmbird_tasks_lock);
	list_add_tail(&get_hmbird_ts(p)->tasks_node, &hmbird_tasks);
	spin_unlock_irq(&hmbird_tasks_lock);

	percpu_up_read(&hmbird_fork_rwsem);
}

void hmbird_cancel_fork(struct task_struct *p)
{
	struct hmbird_entity *see = get_hmbird_ts(p);

	if (hmbird_enabled())
		hmbird_ops_disable_task(p);

	kfree(get_hmbird_ts(p));
	see = NULL;

	percpu_up_read(&hmbird_fork_rwsem);
}

void hmbird_free(struct task_struct *p)
{
	unsigned long flags;
	struct hmbird_entity *see = get_hmbird_ts(p);

	spin_lock_irqsave(&hmbird_tasks_lock, flags);
	list_del_init(&get_hmbird_ts(p)->tasks_node);
	spin_unlock_irqrestore(&hmbird_tasks_lock, flags);

	/*
	 * @p is off hmbird_tasks and wholly ours. hmbird_ops_enable()'s PREPPED ->
	 * ENABLED transitions can't race us. Disable ops for @p.
	 */
	if (test_bit(ffs(HMBIRD_TASK_OPS_PREPPED), (unsigned long *)&get_hmbird_ts(p)->flags) ||
		test_bit(ffs(HMBIRD_TASK_OPS_ENABLED), (unsigned long *)&get_hmbird_ts(p)->flags)) {
		struct rq_flags rf;
		struct rq *rq;

		rq = task_rq_lock(p, &rf);
		hmbird_ops_disable_task(p);
		task_rq_unlock(rq, p, &rf);
	}
	kfree(get_hmbird_ts(p));
	see = NULL;
}

static void prio_changed_hmbird(struct rq *rq, struct task_struct *p, int oldprio)
{
}

static void check_preempt_curr_hmbird(struct rq *rq, struct task_struct *p, int wake_flags)
{
	enum cpu_type type;
	int sp_dl;

	if ((is_pipeline_task(p) && !is_pipeline_task(rq->curr)) ||
		(is_critical_system_task(p) && !is_critical_system_task(rq->curr)))
		goto preempt;

	sp_dl = find_idx_from_task(p);
	if (sp_dl >= SCHED_PROP_DEADLINE_LEVEL1)
		return;

	type = cpu_cluster(cpu_of(rq));
	if (type == EXCLUSIVE || ((type == PARTIAL) && !is_partial_enabled()))
		return;

	if (rq->curr->prio > p->prio)
		goto preempt;

	return;

preempt:
	resched_curr(rq);
}

static void switched_to_hmbird(struct rq *rq, struct task_struct *p) {}

int hmbird_check_setscheduler(struct task_struct *p, int policy)
{
	lockdep_assert_rq_held(task_rq(p));

	/* if disallow, reject transitioning into HMBIRD */
	if (hmbird_enabled() && READ_ONCE(get_hmbird_ts(p)->disallow) &&
			p->policy != policy && policy == SCHED_HMBIRD)
		return -EACCES;

	return 0;
}

#ifdef CONFIG_NO_HZ_FULL
bool hmbird_can_stop_tick(struct rq *rq)
{
	struct task_struct *p = rq->curr;

	if (hmbird_ops_disabling())
		return false;

	if (p->sched_class != &hmbird_sched_class)
		return true;

	return get_hmbird_rq(rq)->flags & HMBIRD_RQ_CAN_STOP_TICK;
}
#endif

int hmbird_tg_online(struct task_group *tg)
{
	struct cgroup *cgrp;

	if (!tg)
		return 0;
	cgrp = tg->css.cgroup;
	if (!cgrp || !(cgrp->kn))
		return 0;
	update_cgroup_ids_table(cgrp->kn->id, -1);
	return 0;
}

/*
 * Omitted operations:
 *
 * - check_preempt_curr: NOOP as it isn't useful in the wakeup path because the
 *   task isn't tied to the CPU at that point. Preemption is implemented by
 *   resetting the victim task's slice to 0 and triggering reschedule on the
 *   target CPU.
 *
 * - migrate_task_rq: Unncessary as task to cpu mapping is transient.
 *
 * - task_fork/dead: We need fork/dead notifications for all tasks regardless of
 *   their current sched_class. Call them directly from sched core instead.
 *
 * - task_woken, switched_from: Unnecessary.
 */
DEFINE_SCHED_CLASS(hmbird) = {
	.enqueue_task		= enqueue_task_hmbird,
	.dequeue_task		= dequeue_task_hmbird,
	.yield_task		= yield_task_hmbird,
	.yield_to_task		= yield_to_task_hmbird,

	.check_preempt_curr	= check_preempt_curr_hmbird,

	.pick_next_task		= pick_next_task_hmbird,

	.put_prev_task		= put_prev_task_hmbird,
	.set_next_task		= set_next_task_hmbird,

#ifdef CONFIG_SMP
	.balance		= balance_hmbird,
	.select_task_rq		= select_task_rq_hmbird,
	.set_cpus_allowed	= set_cpus_allowed_hmbird,
#endif
	.task_tick		= task_tick_hmbird,

	.switched_to		= switched_to_hmbird,
	.prio_changed		= prio_changed_hmbird,

	.update_curr		= update_curr_hmbird,

#ifdef CONFIG_UCLAMP_TASK
	.uclamp_enabled		= 0,
#endif
};

/*
 * Must with rq lock held.
 */
bool task_is_hmbird(struct task_struct *p)
{
	return p->sched_class == &hmbird_sched_class;
}


void init_dsq(struct hmbird_dispatch_q *dsq, u64 dsq_id)
{
	memset(dsq, 0, sizeof(*dsq));

	raw_spin_lock_init(&dsq->lock);
	INIT_LIST_HEAD(&dsq->fifo);
	dsq->id = dsq_id;
}

static int hmbird_cgroup_init(void)
{
	struct cgroup_subsys_state *css;

	css_for_each_descendant_pre(css, &root_task_group.css) {
		struct task_group *tg = css_tg(css);

		cgrp_dsq_idx_init(css->cgroup, tg);
	}
	return 0;
}

/*
 * Used by sched_fork() and __setscheduler_prio() to pick the matching
 * sched_class. dl/rt are already handled.
 */
bool task_on_hmbird(struct task_struct *p)
{
	return hmbird_enabled();
}

static void update_load_enable_hmbird(bool enable)
{
	static bool prev;

	if (prev == enable)
		return;

	prev = enable;
	WRITE_ONCE(tick_sched_clock, 0);
	if (enable)
		atomic_set_release(&set_sched_clock_prepare, true);
}

static void __setscheduler_prio(struct task_struct *p, int prio)
{
	bool on_hmbird = task_on_hmbird(p);

	if (p->sched_class == &stop_sched_class) {
		p->prio = prio;
		return;
	} else if (dl_prio(prio))
		p->sched_class = &dl_sched_class;
	else if (on_hmbird && rt_prio(prio))
		p->sched_class = &hmbird_sched_class;
	else if (rt_prio(prio))
		p->sched_class = &rt_sched_class;
	else if (on_hmbird)
		p->sched_class = &hmbird_sched_class;
	else
		p->sched_class = &fair_sched_class;

	p->prio = prio;
}

/*
 * Heartbeat, avoid humbird keep running while APP already exit.
 * Check whether APP send alive-signal periodly.
 */
#define HEARTBEAT_TIMEOUT		(msecs_to_jiffies(2500))
#define HEARTBEAT_CHECK_INTERVAL	(msecs_to_jiffies(1000))
static struct timer_list hb_timer;
static unsigned long next_hb;

void hb_timer_handler(struct timer_list *timer)
{
	if (!heartbeat_enable)
		goto refill;

	pr_err("<hmbird_sched>: enter timer.\n");
	if (heartbeat) {
		heartbeat = 0;
		WRITE_ONCE(next_hb, jiffies + HEARTBEAT_TIMEOUT);
	}

	/* can't detect heartbeat, disable ext. */
	if(time_after(jiffies, READ_ONCE(next_hb)))
		hmbird_ops_error("<hmbird_sched><err>:can't detect heartbeat, disable ext\n");
refill:
	mod_timer(&hb_timer, jiffies + HEARTBEAT_CHECK_INTERVAL);
}

static void hb_timer_start(void)
{
	mod_timer(&hb_timer, jiffies + HEARTBEAT_CHECK_INTERVAL);
}

static void hb_timer_init(void)
{
	timer_setup(&hb_timer, hb_timer_handler, 0);
}

static void hb_timer_exit(void)
{
	del_timer(&hb_timer);
}

static void scheduler_switch_done(bool final_state)
{
	/* set scx_enable again, switch may fail. */
	scx_enable = final_state;
	/* reset heartbeat*/
	WRITE_ONCE(next_hb, jiffies + HEARTBEAT_TIMEOUT);

	if (final_state)
		hb_timer_start();
	else
		hb_timer_exit();
}

static void hmbird_switch_log(enum switch_stat ss, bool finish, bool enable)
{
	char *s1 = finish ? "finished" : "failed";
	char *s2 = enable ? "enabled" : "disabled";

	hmbird_internal_systrace("C|9999|hmbird_status|%d\n", ss);
	if (ss == HMBIRD_DISABLED || ss == HMBIRD_ENABLED)
		hmbird_debug("hmbird %s %s at jiffies = %lu, clock = %lu\n",
				s2, s1, jiffies, (unsigned long)sched_clock());
	curr_ss = ss;
}

bool get_hmbird_ops_enabled(void)
{
	return atomic_read(&__hmbird_ops_enabled);
}

bool get_non_hmbird_task(void)
{
	return atomic_read(&non_hmbird_task);
}

static void hmbird_ops_disable_workfn(struct kthread_work *work)
{
	struct hmbird_task_iter sti;
	struct task_struct *p;
	int cpu;

	hmbird_switch_log(HMBIRD_SWITCH_PREP, 0, 0);
	cancel_delayed_work_sync(&hmbird_watchdog_work);

	mutex_lock(&hmbird_ops_enable_mutex);
	switch (hmbird_ops_set_enable_state(HMBIRD_OPS_DISABLING)) {
	case HMBIRD_OPS_DISABLED:
		WARN_ON_ONCE(hmbird_ops_set_enable_state(HMBIRD_OPS_DISABLED) !=
					HMBIRD_OPS_DISABLING);
		hmbird_switch_log(HMBIRD_DISABLED, 0, 0);
		scheduler_switch_done(false);
		return;
	case HMBIRD_OPS_PREPPING:
		fallthrough;
	case HMBIRD_OPS_DISABLING:
		/* shouldn't happen but handle it like ENABLING if it does */
		WARN_ONCE(true, "hmbird: duplicate disabling instance?");
		fallthrough;
	case HMBIRD_OPS_ENABLING:
	case HMBIRD_OPS_ENABLED:
		break;
	}

	/* kick all CPUs to restore ticks */
	for_each_possible_cpu(cpu)
		resched_cpu(cpu);

	/* avoid racing against fork and cgroup changes */
	cpus_read_lock();
	percpu_down_write(&hmbird_fork_rwsem);

	hmbird_switch_log(HMBIRD_RQ_SWITCH_BEGIN, 0, 0);
	spin_lock_irq(&hmbird_tasks_lock);
	atomic_set(&__hmbird_ops_enabled, false);
	hmbird_task_iter_init(&sti);
	while ((p = hmbird_task_iter_next_filtered_locked(&sti))) {
		const struct sched_class *old_class = p->sched_class;
		struct rq *rq = task_rq(p);
		bool alive = READ_ONCE(p->__state) != TASK_DEAD;

		update_rq_clock(rq);

		SCHED_CHANGE_BLOCK(rq, p, DEQUEUE_SAVE | DEQUEUE_MOVE |
					DEQUEUE_NOCLOCK) {
			get_hmbird_ts(p)->slice = min_t(u64,
					get_hmbird_ts(p)->slice, HMBIRD_SLICE_DFL);

			__setscheduler_prio(p, p->prio);
		}

		if (alive)
			check_class_changed(task_rq(p), p, old_class, p->prio);

		hmbird_ops_disable_task(p);
	}
	hmbird_task_iter_exit(&sti);
	spin_unlock_irq(&hmbird_tasks_lock);
	hmbird_switch_log(HMBIRD_RQ_SWITCH_DONE, 0, 0);

	atomic_set(&non_hmbird_task, true);
	/* no task is on hmbird, turn off all the switches and flush in-progress calls */
	static_branch_disable_cpuslocked(&hmbird_ops_cpu_preempt);
	synchronize_rcu();

	percpu_up_write(&hmbird_fork_rwsem);
	cpus_read_unlock();

	update_load_enable_hmbird(false);

	mutex_unlock(&hmbird_ops_enable_mutex);

	WARN_ON_ONCE(hmbird_ops_set_enable_state(HMBIRD_OPS_DISABLED) !=
			HMBIRD_OPS_DISABLING);
	hmbird_switch_log(HMBIRD_DISABLED, 1, 0);
	scheduler_switch_done(false);
}

static DEFINE_KTHREAD_WORK(hmbird_ops_disable_work, hmbird_ops_disable_workfn);

static void schedule_hmbird_ops_disable_work(void)
{
	struct kthread_worker *helper = READ_ONCE(hmbird_ops_helper);

	/*
	 * We may be called spuriously before the first bpf_hmbird_reg(). If
	 * hmbird_ops_helper isn't set up yet, there's nothing to do.
	 */
	if (helper)
		kthread_queue_work(helper, &hmbird_ops_disable_work);
}

static void hmbird_ops_disable(enum hmbird_exit_type type)
{
	if (WARN_ON_ONCE(type == HMBIRD_EXIT_NONE || type == HMBIRD_EXIT_DONE))
		type = HMBIRD_EXIT_ERROR;

	schedule_hmbird_ops_disable_work();
}

static void hmbird_err_exit_workfn(struct work_struct *work)
{
	int cpu;
	struct cpufreq_policy *policy;

	hmbird_ctrl(false);

	for_each_present_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (cpu != policy->cpu)
			continue;
		down_write(&policy->rwsem);
		WARN_ON(store_scaling_governor(policy, saved_gov[cpu], strlen(saved_gov[cpu])) <= 0);
		up_write(&policy->rwsem);
		hmbird_info_trace("<heartbeat>:restore origin gov : %s\n", saved_gov[cpu]);
		cpufreq_cpu_put(policy);
	}
	memset((char*)saved_gov, 0 , sizeof(saved_gov));
}

__printf(2, 3) void hmbird_ops_error_type(enum hmbird_exit_type type,
						const char *fmt, ...)
{
	queue_work(system_unbound_wq, &hmbird_err_exit_work);
}

static struct kthread_worker *hmbird_create_rt_helper(const char *name)
{
	struct kthread_worker *helper;

	helper = kthread_create_worker(0, name);
	if (helper)
		sched_set_fifo(helper->task);
	return helper;
}

static inline void set_audio_thread_sched_prop(struct task_struct *p)
{
	struct cgroup_subsys_state *css;

	if (likely(p->prio >= MAX_RT_PRIO))
		return;

	rcu_read_lock();
	css = task_css(p, cpuset_cgrp_id);
	if (!css) {
		rcu_read_unlock();
		return;
	}
	rcu_read_unlock();

	if (!strcmp(css->cgroup->kn->name, "audio-app"))
		hmbird_set_sched_prop(p, SCHED_PROP_DEADLINE_LEVEL1);
}

static int hmbird_ops_enable(void *unused)
{
	struct hmbird_task_iter sti;
	struct task_struct *p;
	int ret;
	int tcnt = 0;
	unsigned long long start = 0;

	hmbird_switch_log(HMBIRD_SWITCH_PREP, 0, 0);
	mutex_lock(&hmbird_ops_enable_mutex);

	if (!hmbird_ops_helper) {
		WRITE_ONCE(hmbird_ops_helper,
				hmbird_create_rt_helper("hmbird_ops_helper"));
		if (!hmbird_ops_helper) {
			ret = -ENOMEM;
			goto err_unlock;
		}
	}

	if (hmbird_ops_enable_state() != HMBIRD_OPS_DISABLED) {
		ret = -EBUSY;
		goto err_unlock;
	}

	update_load_enable_hmbird(true);

	WARN_ON_ONCE(hmbird_ops_set_enable_state(HMBIRD_OPS_PREPPING) !=
				HMBIRD_OPS_DISABLED);

	hmbird_warned_zero_slice = false;

	atomic64_set(&hmbird_nr_rejected, 0);

	/*
	 * Keep CPUs stable during enable so that the BPF scheduler can track
	 * online CPUs by watching ->on/offline_cpu() after ->init().
	 */
	cpus_read_lock();

	hmbird_watchdog_timeout = HMBIRD_WATCHDOG_MAX_TIMEOUT;

	hmbird_watchdog_timestamp = jiffies;
	queue_delayed_work(system_unbound_wq, &hmbird_watchdog_work,
				hmbird_watchdog_timeout / 2);

	/*
	 * Lock out forks, cgroup on/offlining and moves before opening the
	 * floodgate so that they don't wander into the operations prematurely.
	 */
	percpu_down_write(&hmbird_fork_rwsem);

	reset_idle_masks();

	/*
	 * All cgroups should be initialized before letting in tasks. cgroup
	 * on/offlining and task migrations are already locked out.
	 */
	ret = hmbird_cgroup_init();
	if (ret)
		goto err_disable_unlock;


	wait_for_completion_interruptible(&tick_sched_clock_completion);

	/*
	 * Enable ops for every task. Fork is excluded by hmbird_fork_rwsem
	 * preventing new tasks from being added. No need to exclude tasks
	 * leaving as hmbird_free() can handle both prepped and enabled
	 * tasks. Prep all tasks first and then enable them with preemption
	 * disabled.
	 */
	spin_lock_irq(&hmbird_tasks_lock);

	atomic_set(&non_hmbird_task, false);
	atomic_set(&__hmbird_ops_enabled, true);

	hmbird_task_iter_init(&sti);
	while ((p = hmbird_task_iter_next_filtered(&sti)))
		hmbird_ops_prepare_task(p, task_group(p));

	hmbird_task_iter_exit(&sti);

	/*
	 * All tasks are prepped but are still ops-disabled. Ensure that
	 * %current can't be scheduled out and switch everyone.
	 * preempt_disable() is necessary because we can't guarantee that
	 * %current won't be starved if scheduled out while switching.
	 */
	preempt_disable();

	/*
	 * From here on, the disable path must assume that tasks have ops
	 * enabled and need to be recovered.
	 */
	if (!hmbird_ops_tryset_enable_state(HMBIRD_OPS_ENABLING, HMBIRD_OPS_PREPPING)) {
		preempt_enable();
		spin_unlock_irq(&hmbird_tasks_lock);
		ret = -EBUSY;
		goto err_disable_unlock;
	}

	/*
	 * We're fully committed and can't fail. The PREPPED -> ENABLED
	 * transitions here are synchronized against hmbird_free() through
	 * hmbird_tasks_lock.
	 */
	start = sched_clock();
	hmbird_switch_log(HMBIRD_RQ_SWITCH_BEGIN, 0, 0);
	hmbird_task_iter_init(&sti);
	while ((p = hmbird_task_iter_next_filtered_locked(&sti))) {
		tcnt++;
		if (READ_ONCE(p->__state) != TASK_DEAD) {
			const struct sched_class *old_class = p->sched_class;
			struct rq *rq = task_rq(p);

			set_audio_thread_sched_prop(p);
			update_rq_clock(rq);

			SCHED_CHANGE_BLOCK(rq, p, DEQUEUE_SAVE | DEQUEUE_MOVE |
						DEQUEUE_NOCLOCK) {
				hmbird_ops_enable_task(p);
				__setscheduler_prio(p, p->prio);
			}

			check_class_changed(task_rq(p), p, old_class, p->prio);
		} else {
			hmbird_ops_disable_task(p);
		}
	}
	hmbird_task_iter_exit(&sti);

	spin_unlock_irq(&hmbird_tasks_lock);
	hmbird_switch_log(HMBIRD_RQ_SWITCH_DONE, 0, 0);
	preempt_enable();
	percpu_up_write(&hmbird_fork_rwsem);

	if (!hmbird_ops_tryset_enable_state(HMBIRD_OPS_ENABLED, HMBIRD_OPS_ENABLING)) {
		ret = -EBUSY;
		goto err_disable;
	}

	cpus_read_unlock();
	mutex_unlock(&hmbird_ops_enable_mutex);
	hmbird_switch_log(HMBIRD_ENABLED, 1, 1);
	scheduler_switch_done(true);

	return 0;

err_unlock:
	update_load_enable_hmbird(false);
	mutex_unlock(&hmbird_ops_enable_mutex);
	hmbird_switch_log(HMBIRD_DISABLED, 0, 1);
	scheduler_switch_done(false);
	return ret;

err_disable_unlock:
	percpu_up_write(&hmbird_fork_rwsem);
err_disable:
	cpus_read_unlock();
	update_load_enable_hmbird(false);
	mutex_unlock(&hmbird_ops_enable_mutex);
	/* must be fully disabled before returning */
	hmbird_ops_disable(HMBIRD_EXIT_ERROR);
	kthread_flush_work(&hmbird_ops_disable_work);
	hmbird_switch_log(HMBIRD_DISABLED, 0, 1);
	scheduler_switch_done(false);
	return ret;
}

#ifdef CONFIG_SCHED_DEBUG
static const char *hmbird_ops_enable_state_str[] = {
	[HMBIRD_OPS_PREPPING]	= "prepping",
	[HMBIRD_OPS_ENABLING]	= "enabling",
	[HMBIRD_OPS_ENABLED]	= "enabled",
	[HMBIRD_OPS_DISABLING]	= "disabling",
	[HMBIRD_OPS_DISABLED]	= "disabled",
};

static int hmbird_debug_show(struct seq_file *m, void *v)
{
	mutex_lock(&hmbird_ops_enable_mutex);
	seq_printf(m, "%-30s: %d\n", "enabled", hmbird_enabled());
	seq_printf(m, "%-30s: %s\n", "enable_state",
			hmbird_ops_enable_state_str[hmbird_ops_enable_state()]);
	seq_printf(m, "%-30s: %llu\n", "nr_rejected",
			atomic64_read(&hmbird_nr_rejected));
	mutex_unlock(&hmbird_ops_enable_mutex);
	return 0;
}

static int hmbird_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, hmbird_debug_show, NULL);
}

const struct file_operations sched_hmbird_fops = {
	.open		= hmbird_debug_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};
#endif


static int bpf_hmbird_reg(void *kdata)
{
	return hmbird_ops_enable(kdata);
}

static void bpf_hmbird_unreg(void *kdata)
{
	hmbird_ops_disable(HMBIRD_EXIT_UNREG);
	kthread_flush_work(&hmbird_ops_disable_work);
}

void set_hmbird_module_loaded(int is_loaded)
{
	atomic_set(&hmbird_module_loaded, is_loaded);
}

/*
 * MUST load hmbird module before enable hmbird scheduler
 * load track & hmbird gover implemented in hmbird module
 */
void hmbird_ctrl(bool enable)
{
	if (!atomic_read(&hmbird_module_loaded)) {
		pr_err("hmbird_sched : hmbird module not loaded, failed to enable hmbird!\n");
		return;
	}

	if (enable && (hmbird_ops_enable_state() == HMBIRD_OPS_ENABLED
			|| hmbird_ops_enable_state() == HMBIRD_OPS_ENABLING
			|| hmbird_ops_enable_state() == HMBIRD_OPS_PREPPING))
		/* Executing or completed, no need to repeat. */
		return;
	if (!enable && (hmbird_ops_enable_state() == HMBIRD_OPS_DISABLING ||
			hmbird_ops_enable_state() == HMBIRD_OPS_DISABLED))
		/* Executing or completed, no need to repeat. */
		return;

	if (enable)
		bpf_hmbird_reg(NULL);
	else
		bpf_hmbird_unreg(NULL);
}

static void sysrq_handle_hmbird_reset(u8 key)
{
	if (hmbird_ops_helper)
		hmbird_ops_disable(HMBIRD_EXIT_SYSRQ);
	else
		pr_info("hmbird: BPF scheduler not yet used\n");
}

static const struct sysrq_key_op sysrq_hmbird_reset_op = {
	.handler	= sysrq_handle_hmbird_reset,
	.help_msg	= "reset-sched-hmbird(S)",
	.action_msg	= "Disable hmbird and revert all tasks to CFS",
	.enable_mask	= SYSRQ_ENABLE_RTNICE,
};

void __init init_sched_hmbird_class(void)
{
	int cpu;
	u32 v;
	struct hmbird_entity *init_hmbird;

	/*
	 * The following is to prevent the compiler from optimizing out the enum
	 * definitions so that BPF scheduler implementations can use them
	 * through the generated vmlinux.h.
	 */
	WRITE_ONCE(v, HMBIRD_WAKE_EXEC | HMBIRD_ENQ_WAKEUP | HMBIRD_DEQ_SLEEP |
			HMBIRD_KICK_PREEMPT);

	init_dsq(&hmbird_dsq_global, HMBIRD_DSQ_GLOBAL);
	init_dsq_at_boot();
	init_isolate_cpus();
	hb_timer_init();
#ifdef CONFIG_SMP
	WARN_ON(!alloc_cpumask_var(&idle_masks.cpu, GFP_KERNEL));
	WARN_ON(!alloc_cpumask_var(&idle_masks.smt, GFP_KERNEL));
#endif

	/*
	 * we can't static init init_task's hmbird struct, init here.
	 * init_task->hmbird would not use during boot.
	 */
	init_hmbird = kmalloc(sizeof(struct hmbird_entity), GFP_KERNEL);
	init_task.android_oem_data1[HMBIRD_TS_IDX] = (u64)init_hmbird;
	if (init_hmbird) {
		INIT_LIST_HEAD(&init_hmbird->dsq_node.fifo);
		INIT_LIST_HEAD(&init_hmbird->watchdog_node);
		init_hmbird->sticky_cpu = -1;
		init_hmbird->holding_cpu = -1;
		atomic64_set(&init_hmbird->ops_state, 0);
		init_hmbird->runnable_at = jiffies;
		init_hmbird->slice = HMBIRD_SLICE_DFL;
		init_hmbird->task = &init_task;
		hmbird_set_sched_prop(&init_task, 0);
	} else {
		pr_err("fatal error : alloc init_task.hmbird failed!!!\n");
	}

	for_each_possible_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);

		/*
		 * exec during boot phase, no need to care about alloc failed.
		 * lifecycle same to rq, no need to free.
		 */
		rq->android_oem_data1[HMBIRD_RQ_IDX] =
			(u64)kmalloc(sizeof(struct hmbird_rq), GFP_KERNEL);
		if (get_hmbird_rq(rq))
			get_hmbird_rq(rq)->rq = rq;
		else
			pr_err("fatal error : alloc get_hmbird_rq(rq) failed!!!\n");

		rq->android_oem_data1[HMBIRD_OPS_IDX] =
				(u64)kzalloc(sizeof(struct hmbird_ops), GFP_KERNEL);
		if (!get_hmbird_ops(rq))
			pr_err("fatal error : alloc get_hmbird_ops(rq) failed!!!\n");
		init_dsq(&get_hmbird_rq(rq)->local_dsq, HMBIRD_DSQ_LOCAL);
		INIT_LIST_HEAD(&get_hmbird_rq(rq)->watchdog_list);

		WARN_ON(!zalloc_cpumask_var(&get_hmbird_rq(rq)->cpus_to_kick, GFP_KERNEL));
		WARN_ON(!zalloc_cpumask_var(&get_hmbird_rq(rq)->cpus_to_preempt, GFP_KERNEL));
		WARN_ON(!zalloc_cpumask_var(&get_hmbird_rq(rq)->cpus_to_wait, GFP_KERNEL));

		hmbird_ops_init(get_hmbird_ops(rq));
	}

	register_sysrq_key('S', &sysrq_hmbird_reset_op);
	INIT_DELAYED_WORK(&hmbird_watchdog_work, hmbird_watchdog_workfn);
	INIT_WORK(&hmbird_err_exit_work, hmbird_err_exit_workfn);
	hmbird_misc_init();
	
}

