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

DEFINE_PER_CPU(struct sched_yield_state, ystate);
void hmbird_skip_yield(long *skip)
{
	if (!get_hmbird_ops_enabled()  || !yield_opt)
		return;
	unsigned long flags, usleep;
	struct sched_yield_state *ys;
	int cpu = raw_smp_processor_id();
	struct rq *rq = cpu_rq(cpu);

	if (get_hmbird_rq(rq)->exclusive && !(*skip)) {
		ys = &per_cpu(ystate, cpu);
		raw_spin_lock_irqsave(&ys->lock, flags);
		if (ys->usleep > MIN_YIELD_SLEEP || ys->cnt >= DEFAULT_YIELD_SLEEP_TH) {
			*skip = true;
			usleep = ys->usleep_times ? max(ys->usleep >>
					ys->usleep_times, MIN_YIELD_SLEEP):ys->usleep;
			raw_spin_unlock_irqrestore(&ys->lock, flags);
			usleep_range_state(usleep, usleep, TASK_IDLE);
			ys->usleep_times++;
			return;
		}
		(ys->cnt)++;
		raw_spin_unlock_irqrestore(&ys->lock, flags);
	}
}

void hmbird_yield_state_update_per_frame(void)
{
	if (yield_opt) {
		int cpu;
		unsigned long flags;
		struct sched_yield_state *ys;
		struct rq *tmp_rq;

		for_each_online_cpu(cpu) {
			tmp_rq = cpu_rq(cpu);
			if (get_hmbird_rq(tmp_rq)->exclusive) {
				ys = &per_cpu(ystate, cpu);
				raw_spin_lock_irqsave(&ys->lock, flags);
				if (ys->cnt >= DEFAULT_YIELD_SLEEP_TH || ys->usleep_times > 1) {
					ys->usleep = min(ys->usleep + MIN_YIELD_SLEEP,
							MAX_YIELD_SLEEP);
				} else if (!ys->cnt && (ys->usleep_times == 1)) {
					ys->usleep = max(ys->usleep - MIN_YIELD_SLEEP,
							MIN_YIELD_SLEEP);
				}
				ys->cnt = 0;
				ys->usleep_times = 0;
				raw_spin_unlock_irqrestore(&ys->lock, flags);
			}
		}
	}
}

void hmbird_ops_init(struct hmbird_ops *hmbird_ops)
{
	hmbird_ops->scx_enable = get_hmbird_ops_enabled;
	hmbird_ops->check_non_task = get_non_hmbird_task;
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
