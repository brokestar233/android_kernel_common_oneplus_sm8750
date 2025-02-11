// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Oplus. All rights reserved.
 */
#include <linux/tick.h>
#include <../../kernel/time/tick-sched.h>
#include <linux/sched/hmbird_version.h>

#include "hmbird_shadow_tick.h"

#define HIGHRES_WATCH_CPU       0

#include <linux/sched/hmbird_proc_val.h>
static bool shadow_tick_enable(void) {return highres_tick_ctrl; }
#ifdef CONFIG_HMBIRD_DEBUG_MODE
static bool shadow_tick_dbg_enable(void) {return highres_tick_ctrl_dbg; }
#endif
static bool shadow_tick_timer_init_flag;

#ifdef CONFIG_HMBIRD_DEBUG_MODE
#define shadow_tick_printk(fmt, args...)	\
do {							\
	int cpu = smp_processor_id();			\
	if (shadow_tick_dbg_enable() && cpu == HIGHRES_WATCH_CPU)	\
		trace_printk("hmbird shadow tick :"fmt, args);	\
} while (0)
#else
#define shadow_tick_printk(fmt, args...)
#endif

DEFINE_PER_CPU(struct hrtimer, stt);
#define shadow_tick_timer(cpu) (&per_cpu(stt, (cpu)))
#define STOP_IDLE_TRIGGER     (1)
#define PERIODIC_TICK_TRIGGER (2)
#define TICK_INTVAL	(1000000ULL)
/*
 * restart hrtimer while resume from idle. scheduler tick may resume after 4ms,
 * so we can't restart hrtimer in scheduler tick.
 */
static DEFINE_PER_CPU(u8, trigger_event);

/*
 * Implement 1ms tick by inserting 3 hrtimer ticks to schduler tick.
 * stop hrtimer when tick reachs 4, then restart it at scheduler timer handler.
 */
static DEFINE_PER_CPU(u8, tick_phase);

static inline void highres_timer_ctrl(bool enable, int cpu)
{
	if (enable && hmbird_enabled()) {
		if (!hrtimer_active(shadow_tick_timer(cpu)))
			hrtimer_start(shadow_tick_timer(cpu),
				ns_to_ktime(TICK_INTVAL), HRTIMER_MODE_REL_PINNED);
	} else {
		if (!enable)
			hrtimer_cancel(shadow_tick_timer(cpu));
	}
}

static inline void high_res_clear_phase(int cpu)
{
	per_cpu(tick_phase, cpu) = 0;
}

static enum hrtimer_restart highres_next_phase(int cpu, struct hrtimer *timer)
{
	per_cpu(tick_phase, cpu) = ++per_cpu(tick_phase, cpu) % 3;
	if (per_cpu(tick_phase, cpu)) {
		hrtimer_forward_now(timer, ns_to_ktime(TICK_INTVAL));
		return HRTIMER_RESTART;
	}
	return HRTIMER_NORESTART;
}

void sched_switch_handler(void *data, bool preempt, struct task_struct *prev,
		struct task_struct *next, unsigned int prev_state)
{
	int cpu = smp_processor_id();

	if (shadow_tick_enable() && (cpu_rq(cpu)->idle == prev)) {
		per_cpu(trigger_event, cpu) = STOP_IDLE_TRIGGER;
		high_res_clear_phase(cpu);
		highres_timer_ctrl(true, cpu);
	}
}

static enum hrtimer_restart scheduler_tick_no_balance(struct hrtimer *timer)
{
	int cpu = smp_processor_id();
	struct rq *rq = cpu_rq(cpu);
	struct task_struct *curr = rq->curr;
	struct rq_flags rf;

	rq_lock(rq, &rf);
	update_rq_clock(rq);
	curr->sched_class->task_tick(rq, curr, 0);
	rq_unlock(rq, &rf);

	return highres_next_phase(cpu, timer);
}

void shadow_tick_timer_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		hrtimer_init(shadow_tick_timer(cpu), CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
		shadow_tick_timer(cpu)->function = &scheduler_tick_no_balance;
	}
}

void start_shadow_tick_timer(void)
{
	int cpu = smp_processor_id();

	if (shadow_tick_enable()) {
		if (per_cpu(trigger_event, cpu) == STOP_IDLE_TRIGGER)
			highres_timer_ctrl(false, cpu);
		per_cpu(trigger_event, cpu) = PERIODIC_TICK_TRIGGER;
		high_res_clear_phase(cpu);
		highres_timer_ctrl(true, cpu);
	}
}

static void stop_shadow_tick_timer(void)
{
	int cpu = smp_processor_id();

	per_cpu(trigger_event, cpu) = 0;
	high_res_clear_phase(cpu);
	highres_timer_ctrl(false, cpu);
}

void android_vh_tick_nohz_idle_stop_tick_handler(void *unused, void *data)
{
	if (!shadow_tick_timer_init_flag)
		return;
	stop_shadow_tick_timer();
}

void scheduler_tick_handler(void *unused, struct rq *rq)
{
	if (!shadow_tick_timer_init_flag)
		return;
	start_shadow_tick_timer();
}

static int __init hmbird_shadow_tick_init(void)
{
	int ret = 0;

	if (get_hmbird_version_type() != HMBIRD_OGKI_VERSION)
		return 0;
	shadow_tick_timer_init();
	shadow_tick_timer_init_flag = true;
	return ret;
}

device_initcall(hmbird_shadow_tick_init);
