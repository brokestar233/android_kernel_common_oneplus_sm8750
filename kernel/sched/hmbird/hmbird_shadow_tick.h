/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __HMBIRD_SHADOW_TICK_H__
#define __HMBIRD_SHADOW_TICK_H__

#include <linux/sched.h>

void android_vh_tick_nohz_idle_stop_tick_handler(void *unused, void *data);
void scheduler_tick_handler(void *unused, struct rq *rq);
void sched_switch_handler(void *data, bool preempt, struct task_struct *prev,
		struct task_struct *next, unsigned int prev_state);
#endif
