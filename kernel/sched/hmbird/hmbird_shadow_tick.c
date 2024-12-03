// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Oplus. All rights reserved.
 */
#include <linux/tick.h>
#include <../../kernel/time/tick-sched.h>

#include "hmbird_shadow_tick.h"

#define HIGHRES_WATCH_CPU       0

#include <linux/sched/hmbird_proc_val.h>
static bool shadow_tick_enable(void) {return highres_tick_ctrl;}
static bool shadow_tick_dbg_enable(void) {return highres_tick_ctrl_dbg;}

static bool shadow_tick_timer_init_flag = false;

#define shadow_tick_printk(fmt, args...)	\
do {							\
	int cpu = smp_processor_id();			\
	if (shadow_tick_dbg_enable() && cpu == HIGHRES_WATCH_CPU)	\
		trace_printk("hmbird shadow tick :"fmt, args);	\
} while (0)

#define NUM_SHADOW_TICK_TIMER (3)
DEFINE_PER_CPU(struct hrtimer[NUM_SHADOW_TICK_TIMER], stt);
#define shadow_tick_timer(cpu, id) (&per_cpu(stt[id], (cpu)))

#define STOP_IDLE_TRIGGER     (1)
#define PERIODIC_TICK_TRIGGER (2)
static DEFINE_PER_CPU(u8, trigger_event);

void sched_switch_handler(void *data, bool preempt, struct task_struct *prev,
		struct task_struct *next, unsigned int prev_state)
{
	int i, cpu = smp_processor_id();

	if(!shadow_tick_timer_init_flag)
		return;
	if (shadow_tick_enable() && (cpu_rq(cpu)->idle == prev)) {
		this_cpu_write(trigger_event, STOP_IDLE_TRIGGER);
		for (i = 0; i < NUM_SHADOW_TICK_TIMER; i++) {
			if (!hrtimer_active(shadow_tick_timer(cpu, i)))
				hrtimer_start(shadow_tick_timer(cpu, i),
					ns_to_ktime(1000000ULL * (i + 1)), HRTIMER_MODE_REL);
		}
		if (shadow_tick_dbg_enable() && cpu == HIGHRES_WATCH_CPU)
			trace_printk("hmbird_sched : enter tick triggered by stop_idle events\n");
	}
}

enum hrtimer_restart scheduler_tick_no_balance(struct hrtimer *timer)
{
	int cpu = smp_processor_id();
	struct rq *rq = cpu_rq(cpu);
	struct task_struct *curr = rq->curr;
	struct rq_flags rf;

	rq_lock(rq, &rf);
	update_rq_clock(rq);
	curr->sched_class->task_tick(rq, curr, 0);
	shadow_tick_printk("enter 1ms tick on cpu%d \n", HIGHRES_WATCH_CPU);
	rq_unlock(rq, &rf);

	return HRTIMER_NORESTART;
}

void shadow_tick_timer_init(void)
{
	int i, cpu;

	for_each_possible_cpu(cpu) {
		for (i = 0; i < NUM_SHADOW_TICK_TIMER; i++) {
			hrtimer_init(shadow_tick_timer(cpu, i),
				     CLOCK_MONOTONIC, HRTIMER_MODE_REL);
			shadow_tick_timer(cpu, i)->function = &scheduler_tick_no_balance;
		}
	}
}

void start_shadow_tick_timer(void)
{
	int i, cpu = smp_processor_id();

	if (shadow_tick_enable()) {
		if (this_cpu_read(trigger_event) == STOP_IDLE_TRIGGER) {
			for (i = 0; i < NUM_SHADOW_TICK_TIMER; i++)
				hrtimer_cancel(shadow_tick_timer(cpu, i));
		}

		this_cpu_write(trigger_event, PERIODIC_TICK_TRIGGER);

		for (i = 0; i < NUM_SHADOW_TICK_TIMER; i++) {
			if (!hrtimer_active(shadow_tick_timer(cpu, i)))
				hrtimer_start(shadow_tick_timer(cpu, i),
								ns_to_ktime(1000000ULL * (i + 1)),
								HRTIMER_MODE_REL);
			shadow_tick_printk("restart 1ms tick on cpu%d \n",
							HIGHRES_WATCH_CPU);
		}
	}
}

static void stop_shadow_tick_timer(void)
{
	int i, cpu = smp_processor_id();

	this_cpu_write(trigger_event, 0);
	for (i = 0; i < NUM_SHADOW_TICK_TIMER; i++)
		hrtimer_cancel(shadow_tick_timer(cpu, i));
	shadow_tick_printk("stop 1ms tick on cpu%d \n", HIGHRES_WATCH_CPU);
}

void android_vh_tick_nohz_idle_stop_tick_handler(void *unused, void *data)
{
	if(!shadow_tick_timer_init_flag)
		return;
	stop_shadow_tick_timer();
}

void scheduler_tick_handler(void *unused, struct rq *rq)
{
	if(!shadow_tick_timer_init_flag)
		return;
	start_shadow_tick_timer();
}

static int __init hmbird_shadow_tick_init(void)
{
	int ret = 0;
	shadow_tick_timer_init();
	shadow_tick_timer_init_flag = true;
	return ret;
}

__initcall(hmbird_shadow_tick_init);
