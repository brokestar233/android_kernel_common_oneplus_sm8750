/* SPDX-License-Identifier: GPL-2.0 */
/*
 * HMBIRD scheduler class
 *
 * Copyright (c) 2024 OPlus.
 * Copyright (c) 2024 Dao Huang <huangdao1@oppo.com>
 * Copyright (c) 2024 Yuxing Wang <wangyuxing@oppo.com>
 * Copyright (c) 2024 Taiyu Li <litaiyu@oppo.com>
 */

#ifndef _HMBIRD_H_
#define _HMBIRD_H_

/*
 * Tag marking a kernel function as a kfunc. This is meant to minimize the
 * amount of copy-paste that kfunc authors have to include for correctness so
 * as to avoid issues such as the compiler inlining or eliding either a static
 * kfunc, or a global kfunc in an LTO build.
 */

#define DEBUG_INTERNAL		(1 << 0)
#define	DEBUG_INFO_TRACE	(1 << 1)
#define DEBUG_INFO_SYSTRACE	(1 << 2)

#define NUMS_CGROUP_KINDS	(256)
#define SLIM_FOR_SGAME		(1 << 0)
#define SLIM_FOR_GENSHIN	(1 << 1)

#define MAX_NR_CPUS					(1 << 3)
#ifdef MAX_TASK_NR
#define MAX_KEY_THREAD_RECORD		((MAX_TASK_NR + 1) >> 1)
#else
#define MAX_KEY_THREAD_RECORD		MAX_NR_CPUS
#endif /* MAX_TASK_NR */

#define TOP_TASK_SHIFT				(8)
#define TOP_TASK_MAX				(1 << TOP_TASK_SHIFT)
#ifndef TOP_TASK_BITS_MASK
#define	TOP_TASK_BITS_MASK			(TOP_TASK_MAX - 1)
#endif /* TOP_TASK_BITS_MASK */

#define debug_enabled()	\
	(unlikely(hmbirdcore_debug & DEBUG_INFO_TRACE | DEBUG_INFO_SYSTRACE))

#define hmbird_debug(fmt, ...)	\
	pr_info("<hmbird_sched>:"fmt, ##__VA_ARGS__)

#define hmbird_err(fmt, ...)	\
	pr_err("<hmbird_sched>:"fmt, ##__VA_ARGS__)

#define hmbird_info_trace(fmt, ...)			\
do {						\
	if (unlikely(hmbirdcore_debug & DEBUG_INFO_TRACE))		\
		trace_printk("<hmbird_sched>:"fmt, ##__VA_ARGS__); \
} while (0)


#define hmbird_info_systrace(fmt, ...)	\
do {					\
	if (unlikely(hmbirdcore_debug & DEBUG_INFO_SYSTRACE)) {	\
		char buf[256];		\
		snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__);	\
		tracing_mark_write(buf);			\
	}				\
} while (0)


#define hmbird_internal_trace(fmt, ...)			\
do {						\
	if (unlikely(hmbirdcore_debug & DEBUG_INTERNAL))		\
		trace_printk("<hmbird_sched>:"fmt, ##__VA_ARGS__); \
} while (0)

#define hmbird_internal_systrace(fmt, ...)	\
do {					\
	if (unlikely(hmbirdcore_debug & DEBUG_INTERNAL)) {	\
		char buf[256];		\
		snprintf(buf, sizeof(buf), fmt, ##__VA_ARGS__);	\
		tracing_mark_write(buf);			\
	}				\
} while (0)

enum hmbird_wake_flags {
	/* expose select WF_* flags as enums */
	HMBIRD_WAKE_EXEC		= WF_EXEC,
	HMBIRD_WAKE_FORK		= WF_FORK,
	HMBIRD_WAKE_TTWU		= WF_TTWU,
	HMBIRD_WAKE_SYNC		= WF_SYNC,
};

enum hmbird_enq_flags {
	/* expose select ENQUEUE_* flags as enums */
	HMBIRD_ENQ_WAKEUP		= ENQUEUE_WAKEUP,
	HMBIRD_ENQ_HEAD		= ENQUEUE_HEAD,

	/* high 32bits are HMBIRD specific */

	/*
	 * Set the following to trigger preemption when calling
	 * hmbird_bpf_dispatch() with a local dsq as the target. The slice of the
	 * current task is cleared to zero and the CPU is kicked into the
	 * scheduling path. Implies %HMBIRD_ENQ_HEAD.
	 */
	HMBIRD_ENQ_PREEMPT		= 1LLU << 32,
	HMBIRD_ENQ_REENQ		= 1LLU << 40,
	HMBIRD_ENQ_LAST		= 1LLU << 41,

	/*
	 * A hint indicating that it's advisable to enqueue the task on the
	 * local dsq of the currently selected CPU. Currently used by
	 * select_cpu_dfl() and together with %HMBIRD_ENQ_LAST.
	 */
	HMBIRD_ENQ_LOCAL		= 1LLU << 42,

	/* high 8 bits are internal */
	__HMBIRD_ENQ_INTERNAL_MASK	= 0xffLLU << 56,

	HMBIRD_ENQ_CLEAR_OPSS	= 1LLU << 56,
	HMBIRD_ENQ_DSQ_PRIQ	= 1LLU << 57,
};

enum hmbird_deq_flags {
	/* expose select DEQUEUE_* flags as enums */
	HMBIRD_DEQ_SLEEP		= DEQUEUE_SLEEP,

	/* high 32bits are HMBIRD specific */

	/*
	 * The generic core-sched layer decided to execute the task even though
	 * it hasn't been dispatched yet. Dequeue from the BPF side.
	 */
	HMBIRD_DEQ_CORE_SCHED_EXEC	= 1LLU << 32,
};

enum hmbird_kick_flags {
	HMBIRD_KICK_PREEMPT	= 1LLU << 0,	/* force scheduling on the CPU */
	HMBIRD_KICK_WAIT		= 1LLU << 1,	/* wait for the CPU to be rescheduled */
};

extern const struct sched_class hmbird_sched_class;
extern const struct bpf_verifier_ops bpf_sched_hmbird_verifier_ops;
extern const struct file_operations sched_hmbird_fops;
extern unsigned long hmbird_watchdog_timeout;
extern unsigned long hmbird_watchdog_timestamp;

enum scx_rq_flags {
	HMBIRD_RQ_CAN_STOP_TICK	= 1 << 0,
};

struct hmbird_rq {
	struct hmbird_dispatch_q	local_dsq;
	struct list_head	watchdog_list;
	u64			ops_qseq;
	u64			extra_enq_flags;	/* see move_task_to_local_dsq() */
	u32			nr_running;
	u32			flags;
	bool			cpu_released;
	cpumask_var_t		cpus_to_kick;
	cpumask_var_t		cpus_to_preempt;
	cpumask_var_t		cpus_to_wait;
	u64			pnt_seq;
	u64*		hmbird_cpu_util;
	struct irq_work		kick_cpus_irq_work;
	bool			es4g_select;
	bool 			es4g_isolate;
	bool 			es4g_low_isolate;
	struct rq		*rq;
};

struct hmbird_sched_change_guard {
        struct task_struct      *p;
        struct rq               *rq;
        bool                    queued;
        bool                    running;
        bool                    done;
};

extern struct hmbird_sched_change_guard
hmbird_sched_change_guard_init(struct rq *rq, struct task_struct *p, int flags);

extern void hmbird_sched_change_guard_fini(struct hmbird_sched_change_guard *cg, int flags);

#define SCHED_CHANGE_BLOCK(__rq, __p, __flags)                                  \
        for (struct hmbird_sched_change_guard __cg =                                   \
                        hmbird_sched_change_guard_init(__rq, __p, __flags);            \
             !__cg.done; hmbird_sched_change_guard_fini(&__cg, __flags))

DECLARE_STATIC_KEY_FALSE(hmbird_ops_cpu_preempt);

bool task_on_hmbird(struct task_struct *p);
int hmbird_pre_fork(struct task_struct *p);
int hmbird_fork(struct task_struct *p);
void hmbird_post_fork(struct task_struct *p);
void hmbird_cancel_fork(struct task_struct *p);
int hmbird_check_setscheduler(struct task_struct *p, int policy);
bool hmbird_can_stop_tick(struct rq *rq);
void init_sched_hmbird_class(void);

__printf(2, 3) void hmbird_ops_error_type(enum hmbird_exit_type type,
				       const char *fmt, ...);
#define hmbird_ops_error(fmt, args...)						\
	hmbird_ops_error_type(HMBIRD_EXIT_ERROR, fmt, ##args)

void __hmbird_notify_pick_next_task(struct rq *rq,
				 struct task_struct *p,
				 const struct sched_class *active);

static inline unsigned long hmbird_sched_weight_to_cgroup(unsigned long weight)
{
        return clamp_t(unsigned long,
                       DIV_ROUND_CLOSEST_ULL(weight * CGROUP_WEIGHT_DFL, 1024),
                       CGROUP_WEIGHT_MIN, CGROUP_WEIGHT_MAX);
}

static inline void hmbird_notify_pick_next_task(struct rq *rq,
						struct task_struct *p,
						const struct sched_class *active)
{
	if (!hmbird_enabled())
		return;
#ifdef CONFIG_SMP
	/*
	 * Pairs with the smp_load_acquire() issued by a CPU in
	 * kick_cpus_irq_workfn() who is waiting for this CPU to perform a
	 * resched.
	 */
	smp_store_release(&get_hmbird_rq(rq)->pnt_seq, get_hmbird_rq(rq)->pnt_seq + 1);
#endif
	if (!static_branch_unlikely(&hmbird_ops_cpu_preempt))
		return;
	__hmbird_notify_pick_next_task(rq, p, active);
}

extern void hmbird_scheduler_tick(void);
void scan_timeout(struct rq *rq);
static inline void hmbird_notify_sched_tick(void)
{
	unsigned long last_check;
	int cpu = smp_processor_id();
	struct rq *rq = cpu_rq(cpu);

	hmbird_scheduler_tick();

	if (!hmbird_enabled())
		return;

	last_check = hmbird_watchdog_timestamp;
	if (unlikely(time_after(jiffies, last_check + hmbird_watchdog_timeout))) {
		u32 dur_ms = jiffies_to_msecs(jiffies - last_check);

		hmbird_ops_error_type(HMBIRD_EXIT_ERROR_STALL,
				   "watchdog failed to check in for %u.%03us",
				   dur_ms / 1000, dur_ms % 1000);
	}
	scan_timeout(rq);
}

static inline const struct sched_class *next_active_class(const struct sched_class *class)
{
	class++;

	if (hmbird_enabled() && class == &fair_sched_class)
		class++;
	if (!hmbird_enabled() && class == &hmbird_sched_class)
		class++;
	return class;
}

#define for_active_class_range(class, _from, _to)				\
	for (class = (_from); class != (_to); class = next_active_class(class))

#define for_each_active_class(class)						\
	for_active_class_range(class, __sched_class_highest, __sched_class_lowest)

/*
 * HMBIRD requires a balance() call before every pick_next_task() call including
 * when waking up from idle.
 */
#define for_balance_class_range(class, prev_class, end_class)			\
	for_active_class_range(class, (prev_class) > &hmbird_sched_class ?		\
				&hmbird_sched_class : (prev_class), (end_class))

#define MAX_GLOBAL_DSQS (10)
#define MIN_CGROUP_DL_IDX (5)      /* 8ms */
#define DEFAULT_CGROUP_DL_IDX (8)  /* 64ms */
extern u32 HMBIRD_BPF_DSQS_DEADLINE[MAX_GLOBAL_DSQS];


void __hmbird_update_idle(struct rq *rq, bool idle);

static inline void hmbird_update_idle(struct rq *rq, bool idle)
{
	if (hmbird_enabled())
		__hmbird_update_idle(rq, idle);
}

void hmbird_ctrl(bool enable);

int hmbird_tg_online(struct task_group *tg);


static inline u16 hmbird_task_util(struct task_struct *p)
{
	return (p->sched_class == &hmbird_sched_class) ? get_hmbird_ts(p)->sts.demand_scaled : 0;
}
u16 hmbird_cpu_util(int cpu);

bool get_hmbird_ops_enabled(void);
bool get_non_hmbird_task(void);
#endif /*_EXT_H_*/
