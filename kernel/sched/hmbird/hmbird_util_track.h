/* SPDX-License-Identifier: GPL-2.0 */
/*
 * HMBIRD scheduler class
 *
 * Copyright (c) 2024 OPlus.
 * Copyright (c) 2024 Dao Huang
 * Copyright (c) 2024 Yuxing Wang
 * Copyright (c) 2024 Taiyu Li
 */
#ifndef __HMBIRD_UTIL_TRACK_H__
#define __HMBIRD_UTIL_TRACK_H__

void hmbird_update_task_ravg(struct task_struct *p,
				struct rq *rq, int event, u64 wallclock);
void hmbird_sched_init_task(struct task_struct *p);
void slim_walt_enable(int enable);
void slim_get_cpu_util(int cpu, u64 *util);
void slim_get_task_util(struct task_struct *p, u64 *util);

extern atomic64_t hmbird_irq_work_lastq_ws;

enum task_event {
	PUT_PREV_TASK   = 0,
	PICK_NEXT_TASK  = 1,
	TASK_WAKE       = 2,
	TASK_MIGRATE    = 3,
	TASK_UPDATE     = 4,
	IRQ_UPDATE      = 5,
};

extern DEFINE_PER_CPU(struct hmbird_sched_rq_stats, hmbird_sched_rq_stats);

#endif /* __HMBIRD_UTIL_TRACK_H__ */
