// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Oplus. All rights reserved.
 */

#include <linux/kprobes.h>
#include "hmbird_sched.h"
#include <linux/sched/hmbird.h>
#include <linux/kernel.h>

noinline int tracing_mark_write(const char *buf)
{
	trace_printk(buf);
	return 0;
}

#ifdef CONFIG_SCX_USE_UTIL_TRACK
static void android_vh_hmbird_update_load_handler(
					void *unused, struct task_struct *p,
					struct rq *rq, int event, u64 wallclock)
{
	hmbird_update_task_ravg(p, rq, event, wallclock);
}

static void android_vh_hmbird_init_task_handler(
					void *unused, struct task_struct *p)
{
	hmbird_sched_init_task(p);
}

static void android_vh_hmbird_update_load_enable_handler(
					void *unused, bool enable)
{
	if (enable) {
		oplus_lk_feat_enable(LK_FEATURE_MASK, false);
		oplus_bd_feat_enable(BD_FEATURE_MASK, false);
	} else {
		oplus_lk_feat_enable(LK_FEATURE_MASK, true);
		oplus_bd_feat_enable(BD_FEATURE_MASK, true);
	}
	slim_walt_enable(enable);
	preempt_enable();
	if (enable)
		walt_disable_wait_for_completion();
	else
		walt_enable_wait_for_completion();
	preempt_disable();
}

static void android_vh_get_util_handler(
			void *unused, int cpu, struct task_struct *p, u64 *util)
{
	struct walt_task_struct *wts;
	if ((cpu < 0) && NULL == p)
		return;

	if (p) {
		wts = (struct walt_task_struct *) p->android_vendor_data1;
		*util = wts->pred_demand_scaled;
	} else {
		*util = cpu_util(cpu);
	}
}
#endif

DEFINE_PER_CPU(struct sched_yield_state, ystate);
void hmbird_skip_yield(long *skip)
{
	if (!get_hmbird_ops_enabled())
		return;
	unsigned long flags, usleep;
	struct sched_yield_state *ys;
	int cpu = raw_smp_processor_id();
	struct rq *rq = cpu_rq(cpu);

	if ((get_hmbird_rq(rq)->es4g_select && get_hmbird_rq(rq)->es4g_isolate) && !(*skip)) {
		ys = &per_cpu(ystate, cpu);
		raw_spin_lock_irqsave(&ys->lock, flags);
		if (ys->usleep > MIN_YIELD_SLEEP || ys->cnt >= DEFAULT_YIELD_SLEEP_TH) {
			*skip = true;
			usleep = ys->usleep_times ?
					max(ys->usleep >> ys->usleep_times, MIN_YIELD_SLEEP):ys->usleep;
			raw_spin_unlock_irqrestore(&ys->lock, flags);
			usleep_range_state(usleep, usleep, TASK_IDLE);
			ys->usleep_times++;
			return;
		}
		(ys->cnt)++;
		raw_spin_unlock_irqrestore(&ys->lock, flags);
	}
}

void hmbird_ops_init(struct hmbird_ops *hmbird_ops)
{
	hmbird_ops->scx_enable = get_hmbird_ops_enabled;
	hmbird_ops->check_non_task = get_non_hmbird_task;
	hmbird_ops->do_sched_yield_before = hmbird_skip_yield;
	hmbird_ops->window_rollover_run_once = hmbird_window_rollover_run_once;
}

void hmbird_misc_init(void)
{
	int cpu;
	for_each_possible_cpu(cpu) {
		struct sched_yield_state *ys = &per_cpu(ystate, cpu);
		raw_spin_lock_init(&ys->lock);
	}

	set_hmbird_module_loaded(1);
}
