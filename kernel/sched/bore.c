/*
 *  Burst-Oriented Response Enhancer (BORE) CPU Scheduler
 *  Copyright (C) 2021-2025 Masahito Suzuki <firelzrd@gmail.com>
 */
#include <linux/cpuset.h>
#include <linux/sched/task.h>
#include <linux/sched/bore.h>
#include "sched.h"

#ifdef CONFIG_SCHED_BORE
u8   __read_mostly sched_bore                   = 1;
u8   __read_mostly sched_burst_exclude_kthreads = 1;
u8   __read_mostly sched_burst_smoothness       = 40;
u8   __read_mostly sched_burst_fork_atavistic   = 2;
u8   __read_mostly sched_burst_parity_threshold = 2;
u8   __read_mostly sched_burst_penalty_offset   = 24;
uint __read_mostly sched_burst_penalty_scale    = 3180;
uint __read_mostly sched_burst_cache_stop_count = 64;
uint __read_mostly sched_burst_cache_lifetime   = 75000000;
uint __read_mostly sched_deadline_boost_mask    = ENQUEUE_INITIAL
                                                | ENQUEUE_WAKEUP;
static int __maybe_unused maxval_6_bits  =   63;
static int __maybe_unused maxval_8_bits  =  255;
static int __maybe_unused maxval_12_bits = 4095;

#define BURST_PENALTY_SHIFT 12
#define MAX_BURST_PENALTY ((40U << BURST_PENALTY_SHIFT) - 1)

static u32 log2p1_u64_u32fp(u64 v, u8 fp) {
	if (!v) return 0;
	u32 exponent = fls64(v);
	u32 mantissa = (u32)(v << (64 - exponent) << 1 >> (64 - fp));
	return exponent << fp | mantissa;
}

static inline u32 calc_burst_penalty(u64 burst_time) {
	u32 greed, tolerance, penalty, scaled_penalty;
	
	greed = log2p1_u64_u32fp(burst_time, BURST_PENALTY_SHIFT);
	tolerance = sched_burst_penalty_offset << BURST_PENALTY_SHIFT;
	penalty = max(0, (s32)(greed - tolerance));
	scaled_penalty = penalty * sched_burst_penalty_scale >> 10;

	return min(MAX_BURST_PENALTY, scaled_penalty);
}

static inline u64 __scale_slice(u64 delta, u8 score)
{return mul_u64_u32_shr(delta, sched_prio_to_wmult[score], 22);}

static inline u64 __unscale_slice(u64 delta, u8 score)
{return mul_u64_u32_shr(delta, sched_prio_to_weight[score], 10);}

static void reweight_task_by_prio(struct task_struct *p, int prio) {
	struct sched_entity *se = &p->se;
	unsigned long weight = scale_load(sched_prio_to_weight[prio]);

	reweight_entity(cfs_rq_of(se), se, weight, true);
	se->load.inv_weight = sched_prio_to_wmult[prio];
}

static inline u8 effective_prio(struct task_struct *p) {
	u8 prio = p->static_prio - MAX_RT_PRIO;
	if (likely(sched_bore))
		prio += p->se.bore_stats->burst_score;
	return min(39, prio);
}

void update_burst_score(struct sched_entity *se) {
	if (!entity_is_task(se)) return;
	struct task_struct *p = task_of(se);
	u8 prev_prio = effective_prio(p);

	u8 burst_score = 0;
	if (!((p->flags & PF_KTHREAD) && likely(sched_burst_exclude_kthreads)))
		burst_score = se->bore_stats->burst_penalty >> BURST_PENALTY_SHIFT;
	se->bore_stats->burst_score = burst_score;

	u8 new_prio = effective_prio(p);
	if (new_prio != prev_prio)
		reweight_task_by_prio(p, new_prio);
}

void update_curr_bore(u64 delta_exec, struct sched_entity *se) {
	if (!entity_is_task(se)) return;

	se->bore_stats->burst_time += delta_exec;
	se->bore_stats->curr_burst_penalty = calc_burst_penalty(se->bore_stats->burst_time);
	if (se->bore_stats->curr_burst_penalty > se->bore_stats->prev_burst_penalty)
		se->bore_stats->burst_penalty = se->bore_stats->prev_burst_penalty +
		(se->bore_stats->curr_burst_penalty - se->bore_stats->prev_burst_penalty) / se->bore_stats->burst_count;
	update_burst_score(se);
}

static inline u32 binary_smooth(u32 new, u32 old, u8 dumper) {
	u32 abs_diff = (new > old)? (new - old): (old - new);
	u32 adj_diff = (abs_diff / dumper) + ((abs_diff % dumper) != 0);
	return (new > old)? (old + adj_diff): (old - adj_diff);
}

static void __restart_burst(struct sched_entity *se) {
	se->bore_stats->prev_burst_penalty = binary_smooth(
		se->bore_stats->curr_burst_penalty, se->bore_stats->prev_burst_penalty, se->bore_stats->burst_count);
	se->bore_stats->burst_time = 0;
	se->bore_stats->curr_burst_penalty = 0;

	u8 smoothness = sched_burst_smoothness;
	if (se->bore_stats->burst_count < smoothness)
		se->bore_stats->burst_count++;
	else if (unlikely(se->bore_stats->burst_count > smoothness))
		se->bore_stats->burst_count = smoothness;
}

inline void restart_burst(struct sched_entity *se) {
	__restart_burst(se);
	se->bore_stats->burst_penalty = se->bore_stats->prev_burst_penalty;
	update_burst_score(se);
}

void restart_burst_rescale_deadline(struct sched_entity *se) {
	s64 vscaled, wremain, vremain = se->deadline - se->vruntime;
	struct task_struct *p = task_of(se);
	u8 prev_prio = effective_prio(p);
	restart_burst(se);
	u8 new_prio = effective_prio(p);
	if (prev_prio > new_prio) {
		wremain = __unscale_slice(abs(vremain), prev_prio);
		vscaled = __scale_slice(wremain, new_prio);
		if (unlikely(vremain < 0))
			vscaled = -vscaled;
		se->deadline = se->vruntime + vscaled;
	}
}

static inline bool task_is_bore_eligible(struct task_struct *p)
{return p && p->sched_class == &fair_sched_class && !p->exit_state;}

static inline void reset_task_weights_bore(void) {
	struct task_struct *task;
	struct rq *rq;
	struct rq_flags rf;

	write_lock_irq(&tasklist_lock);
	for_each_process(task) {
		if (!task_is_bore_eligible(task)) continue;
		rq = task_rq(task);
		rq_pin_lock(rq, &rf);
		update_rq_clock(rq);
		reweight_task_by_prio(task, effective_prio(task));
		rq_unpin_lock(rq, &rf);
	}
	write_unlock_irq(&tasklist_lock);
}

int sched_bore_update_handler(struct ctl_table *table, int write,
	void __user *buffer, size_t *lenp, loff_t *ppos) {
	int ret = proc_dou8vec_minmax(table, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;

	reset_task_weights_bore();

	return 0;
}

#define for_each_child(p, t) \
	list_for_each_entry(t, &(p)->children, sibling)

static inline u32 count_entries_upto2(struct list_head *head) {
	struct list_head *next = head->next;
	return (next != head) + (next->next != head);
}

static inline bool burst_cache_expired(struct sched_burst_cache *bc, u64 now)
{return (s64)(bc->timestamp + sched_burst_cache_lifetime - now) < 0;}

static void update_burst_cache(struct sched_burst_cache *bc,
	struct task_struct *p, u32 cnt, u32 sum, u64 now) {
	u32 avg = cnt ? sum / cnt : 0;
	bc->value = max(avg, p->se.bore_stats->burst_penalty);
	bc->count = cnt;
	bc->timestamp = now;
}

static inline void update_child_burst_direct(struct task_struct *p, u64 now) {
	u32 cnt = 0, sum = 0;
	struct task_struct *child;

	for_each_child(p, child) {
		if (!task_is_bore_eligible(child)) continue;
		cnt++;
		sum += child->se.bore_stats->burst_penalty;
	}

	update_burst_cache(&p->se.bore_stats->child_burst, p, cnt, sum, now);
}

static inline u32 inherit_burst_direct(
	struct task_struct *p, u64 now, u64 clone_flags) {
	struct task_struct *parent = p;
	struct sched_burst_cache *bc;

	if (clone_flags & CLONE_PARENT)
		parent = parent->real_parent;

	bc = &parent->se.bore_stats->child_burst;
	if (burst_cache_expired(bc, now))
		update_child_burst_direct(parent, now);

	return bc->value;
}

static void update_child_burst_topological(
	struct task_struct *p, u64 now, u32 depth, u32 *acnt, u32 *asum) {
	u32 cnt = 0, dcnt = 0, sum = 0;
	struct task_struct *child, *dec;
	struct sched_burst_cache *bc __maybe_unused;

	for_each_child(p, child) {
		dec = child;
		while ((dcnt = count_entries_upto2(&dec->children)) == 1)
			dec = list_first_entry(&dec->children, struct task_struct, sibling);
		
		if (!dcnt || !depth) {
			if (!task_is_bore_eligible(dec)) continue;
			cnt++;
			sum += dec->se.bore_stats->burst_penalty;
			continue;
		}
		bc = &dec->se.bore_stats->child_burst;
		if (!burst_cache_expired(bc, now)) {
			cnt += bc->count;
			sum += bc->value * bc->count;
			if (sched_burst_cache_stop_count <= cnt) break;
			continue;
		}
		update_child_burst_topological(dec, now, depth - 1, &cnt, &sum);
	}

	update_burst_cache(&p->se.bore_stats->child_burst, p, cnt, sum, now);
	*acnt += cnt;
	*asum += sum;
}

static inline u32 inherit_burst_topological(
	struct task_struct *p, u64 now, u64 clone_flags) {
	struct task_struct *anc = p;
	struct sched_burst_cache *bc;
	u32 cnt = 0, sum = 0;
	u32 base_child_cnt = 0;

	if (clone_flags & CLONE_PARENT) {
		anc = anc->real_parent;
		base_child_cnt = 1;
	}

	for (struct task_struct *next;
		 anc != (next = anc->real_parent) &&
		 	count_entries_upto2(&anc->children) <= base_child_cnt;) {
		anc = next;
		base_child_cnt = 1;
	}

	bc = &anc->se.bore_stats->child_burst;
	if (burst_cache_expired(bc, now))
		update_child_burst_topological(
			anc, now, sched_burst_fork_atavistic - 1, &cnt, &sum);

	return bc->value;
}

static inline void update_tg_burst(struct task_struct *p, u64 now) {
	struct task_struct *task;
	u32 cnt = 0, sum = 0;

	for_each_thread(p, task) {
		if (!task_is_bore_eligible(task)) continue;
		cnt++;
		sum += task->se.bore_stats->burst_penalty;
	}

	update_burst_cache(&p->se.bore_stats->group_burst, p, cnt, sum, now);
}

static inline u32 inherit_burst_tg(struct task_struct *p, u64 now) {
	struct task_struct *parent = p->group_leader;
	struct sched_burst_cache *bc = &parent->se.bore_stats->group_burst;
	if (burst_cache_expired(bc, now))
		update_tg_burst(parent, now);

	return bc->value;
}

void sched_clone_bore(struct task_struct *p,
	struct task_struct *parent, u64 clone_flags, u64 now) {
	struct sched_entity *se = &p->se;
	u32 penalty;

	if (!task_is_bore_eligible(p)) return;

	penalty = (clone_flags & CLONE_THREAD)?
		inherit_burst_tg(parent, now):
		(likely(sched_burst_fork_atavistic)?
			inherit_burst_topological(parent, now, clone_flags):
			inherit_burst_direct(parent, now, clone_flags));

	__restart_burst(se);
	se->bore_stats->burst_penalty = se->bore_stats->prev_burst_penalty =
		max(se->bore_stats->prev_burst_penalty, penalty);
	se->bore_stats->burst_count = 1;
	se->bore_stats->child_burst.timestamp = 0;
	se->bore_stats->group_burst.timestamp = 0;
}

void reset_task_bore(struct task_struct *p) {
	p->se.bore_stats->burst_time = 0;
	p->se.bore_stats->prev_burst_penalty = 0;
	p->se.bore_stats->curr_burst_penalty = 0;
	p->se.bore_stats->burst_penalty = 0;
	p->se.bore_stats->burst_score = 0;
	p->se.bore_stats->burst_count = 1;
	memset(&p->se.bore_stats->child_burst, 0, sizeof(struct sched_burst_cache));
	memset(&p->se.bore_stats->group_burst, 0, sizeof(struct sched_burst_cache));
}

void __init sched_bore_init(void) {
	printk(KERN_INFO "%s %s by %s\n",
		SCHED_BORE_PROGNAME, SCHED_BORE_VERSION, SCHED_BORE_AUTHOR);
	init_task.se.bore_stats = kzalloc(sizeof(struct sched_bore_stats), GFP_KERNEL);
    if (unlikely(!init_task.se.bore_stats)) {
        pr_err("Failed to allocate bore_stats for init_task\n");
    }
	reset_task_bore(&init_task);
}

#ifdef CONFIG_SYSCTL
static struct ctl_table sched_bore_sysctls[] = {
	{
		.procname	= "sched_bore",
		.data		= &sched_bore,
		.maxlen		= sizeof(u8),
		.mode		= 0644,
		.proc_handler = sched_bore_update_handler,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{
		.procname	= "sched_burst_exclude_kthreads",
		.data		= &sched_burst_exclude_kthreads,
		.maxlen		= sizeof(u8),
		.mode		= 0644,
		.proc_handler = proc_dou8vec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{
		.procname	= "sched_burst_smoothness",
		.data		= &sched_burst_smoothness,
		.maxlen		= sizeof(u8),
		.mode		= 0644,
		.proc_handler = proc_dou8vec_minmax,
		.extra1		= SYSCTL_ONE,
		.extra2		= &maxval_8_bits,
	},
	{
		.procname	= "sched_burst_fork_atavistic",
		.data		= &sched_burst_fork_atavistic,
		.maxlen		= sizeof(u8),
		.mode		= 0644,
		.proc_handler = proc_dou8vec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_THREE,
	},
	{
		.procname	= "sched_burst_parity_threshold",
		.data		= &sched_burst_parity_threshold,
		.maxlen		= sizeof(u8),
		.mode		= 0644,
		.proc_handler = proc_dou8vec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= &maxval_8_bits,
	},
	{
		.procname	= "sched_burst_penalty_offset",
		.data		= &sched_burst_penalty_offset,
		.maxlen		= sizeof(u8),
		.mode		= 0644,
		.proc_handler = proc_dou8vec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= &maxval_6_bits,
	},
	{
		.procname	= "sched_burst_penalty_scale",
		.data		= &sched_burst_penalty_scale,
		.maxlen		= sizeof(uint),
		.mode		= 0644,
		.proc_handler = proc_douintvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= &maxval_12_bits,
	},
	{
		.procname	= "sched_burst_cache_stop_count",
		.data		= &sched_burst_cache_stop_count,
		.maxlen		= sizeof(uint),
		.mode		= 0644,
		.proc_handler = proc_douintvec,
	},
	{
		.procname	= "sched_burst_cache_lifetime",
		.data		= &sched_burst_cache_lifetime,
		.maxlen		= sizeof(uint),
		.mode		= 0644,
		.proc_handler = proc_douintvec,
	},
	{
		.procname	= "sched_deadline_boost_mask",
		.data		= &sched_deadline_boost_mask,
		.maxlen		= sizeof(uint),
		.mode		= 0644,
		.proc_handler = proc_douintvec,
	},
};

static int __init sched_bore_sysctl_init(void) {
	register_sysctl_init("kernel", sched_bore_sysctls);
	return 0;
}
late_initcall(sched_bore_sysctl_init);
#endif // CONFIG_SYSCTL
#endif // CONFIG_SCHED_BORE
