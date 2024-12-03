// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Oplus. All rights reserved.
 */
#include "hmbird_sched.h"

#define CREATE_TRACE_POINTS
#include "hmbird_trace.h"
#undef CREATE_TRACE_POINTS

#define HMBIRD_DEBUG_PANIC		(1 << 3)

#define hmbird_trace_printk(fmt, args...)	\
do {								\
		trace_printk("hmbird_sched_ext :"fmt, args);	\
} while (0)

extern noinline int tracing_mark_write(const char *buf);

#define HMBIRD_BUG(fmt, ...)		\
do {										\
	printk_deferred("hmbird[%s]:"fmt, __func__, ##__VA_ARGS__);	\
} while (0)

void hmbird_window_rollover_run_once(struct rq *rq)
{
	int cpu;
	unsigned long flags;
	struct sched_yield_state *ys;
	struct rq *tmp_rq;

	for_each_online_cpu(cpu) {
		tmp_rq = cpu_rq(cpu);
		if (get_hmbird_rq(tmp_rq)->es4g_select && get_hmbird_rq(tmp_rq)->es4g_isolate) {
			ys = &per_cpu(ystate, cpu);
			raw_spin_lock_irqsave(&ys->lock, flags);
			if (ys->cnt >= DEFAULT_YIELD_SLEEP_TH || ys->usleep_times > 1) {
				ys->usleep = min(ys->usleep + MIN_YIELD_SLEEP, MAX_YIELD_SLEEP);
			} else if (!ys->cnt && (ys->usleep_times == 1)) {
				ys->usleep = max(ys->usleep - MIN_YIELD_SLEEP, MIN_YIELD_SLEEP);
			}
			ys->cnt = 0;
			ys->usleep_times = 0;
			raw_spin_unlock_irqrestore(&ys->lock, flags);
		}
	}
}

