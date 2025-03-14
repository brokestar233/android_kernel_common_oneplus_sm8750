// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Oplus. All rights reserved.
 */
#include "hmbird_sched.h"
#include "hmbird_util_track.h"

#define CREATE_TRACE_POINTS
#include "hmbird_trace.h"
#undef CREATE_TRACE_POINTS

#define HMBIRD_DEBUG_PANIC		(1 << 3)
static bool init_irq_work_inited;

extern noinline int tracing_mark_write(const char *buf);

int hmbird_sched_ravg_window = 8000000;
int new_hmbird_sched_ravg_window = 8000000;
DEFINE_SPINLOCK(new_sched_ravg_window_lock);
DEFINE_PER_CPU(struct hmbird_sched_rq_stats, hmbird_sched_rq_stats);


static struct irq_work hmbird_slim_walt_irq_work;

/*Sysctl related interface*/
#define WINDOW_STATS_RECENT		0
#define WINDOW_STATS_MAX		1
#define WINDOW_STATS_MAX_RECENT_AVG	2
#define WINDOW_STATS_AVG		3
#define WINDOW_STATS_INVALID_POLICY	4


#define SCX_SCHED_CAPACITY_SHIFT  10
#define SCHED_ACCOUNT_WAIT_TIME 0

atomic64_t hmbird_irq_work_lastq_ws;
static u64 tick_sched_clock;

static inline u64 scale_exec_time(u64 delta, struct rq *rq)
{
	struct hmbird_sched_rq_stats *srq = &per_cpu(hmbird_sched_rq_stats, cpu_of(rq));

	return (delta * srq->task_exec_scale) >> SCX_SCHED_CAPACITY_SHIFT;
}

static inline u64 scale_time_to_util(u64 d)
{
	do_div(d, hmbird_sched_ravg_window >> SCX_SCHED_CAPACITY_SHIFT);
	return d;
}

static u64 add_to_task_demand(struct rq *rq, struct task_struct *p, u64 delta)
{
	struct hmbird_sched_task_stats *sts = &(get_hmbird_ts(p)->sts);

	delta = scale_exec_time(delta, rq);
	sts->sum += delta;
	if (unlikely(sts->sum > hmbird_sched_ravg_window))
		sts->sum = hmbird_sched_ravg_window;

	return delta;
}


static int
account_busy_for_task_demand(struct rq *rq, struct task_struct *p, int event)
{
	/*
	 * No need to bother updating task demand for the idle task.
	 */
	if (is_idle_task(p))
		return 0;

	/*
	 * When a task is waking up it is completing a segment of non-busy
	 * time. Likewise, if wait time is not treated as busy time, then
	 * when a task begins to run or is migrated, it is not running and
	 * is completing a segment of non-busy time.
	 */
	if (event == TASK_WAKE || (!SCHED_ACCOUNT_WAIT_TIME &&
			 (event == PICK_NEXT_TASK || event == TASK_MIGRATE)))
		return 0;

	/*
	 * The idle exit time is not accounted for the first task _picked_ up to
	 * run on the idle CPU.
	 */
	if (event == PICK_NEXT_TASK && rq->curr == rq->idle)
		return 0;

	/*
	 * TASK_UPDATE can be called on sleeping task, when its moved between
	 * related groups
	 */
	if (event == TASK_UPDATE) {
		if (rq->curr == p)
			return 1;

		return p->on_rq ? SCHED_ACCOUNT_WAIT_TIME : 0;
	}

	return 1;
}

static void rollover_cpu_window(struct rq *rq, bool full_window)
{
	struct hmbird_sched_rq_stats *srq = &per_cpu(hmbird_sched_rq_stats, cpu_of(rq));
	u64 curr_sum = srq->curr_runnable_sum;

	if (unlikely(full_window))
		curr_sum = 0;

	srq->prev_runnable_sum = curr_sum;
	srq->curr_runnable_sum = 0;
}

static u64
update_window_start(struct rq *rq, u64 wallclock, int event)
{
	s64 delta;
	int nr_windows;
	bool full_window;

	struct hmbird_sched_rq_stats *srq = &per_cpu(hmbird_sched_rq_stats, cpu_of(rq));
	u64 old_window_start = srq->window_start;

	if (wallclock < srq->latest_clock)
		wallclock = srq->latest_clock;
	delta = wallclock - srq->window_start;
	if (delta < 0) {
		delta = 0;
		wallclock = srq->window_start;
	}
	srq->latest_clock = wallclock;
	if (delta < hmbird_sched_ravg_window)
		return old_window_start;

	nr_windows = div64_u64(delta, hmbird_sched_ravg_window);
	srq->window_start += (u64)nr_windows * (u64)hmbird_sched_ravg_window;

	srq->prev_window_size = hmbird_sched_ravg_window;
	full_window = nr_windows > 1;
	rollover_cpu_window(rq, full_window);

	return old_window_start;
}

#define DIV64_U64_ROUNDUP(X, Y) div64_u64((X) + (Y - 1), Y)

static inline unsigned int get_max_freq(unsigned int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get_raw(cpu);

	return (policy == NULL) ? 0 : policy->cpuinfo.max_freq;
}

static inline unsigned int cpu_cur_freq(int cpu)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get_raw(cpu);

	return (policy == NULL) ? 0 : policy->cur;
}

static void
update_task_rq_cpu_cycles(struct task_struct *p, struct rq *rq, int event,
			  u64 wallclock)
{
	int cpu = cpu_of(rq);
	struct hmbird_sched_rq_stats *srq = &per_cpu(hmbird_sched_rq_stats, cpu_of(rq));

	srq->task_exec_scale = DIV64_U64_ROUNDUP(cpu_cur_freq(cpu) *
					arch_scale_cpu_capacity(cpu), get_max_freq(cpu));
}

/*
 * Called when new window is starting for a task, to record cpu usage over
 * recently concluded window(s). Normally 'samples' should be 1. It can be > 1
 * when, say, a real-time task runs without preemption for several windows at a
 * stretch.
 */
static void update_history(struct rq *rq, struct task_struct *p,
			 u32 runtime, int samples, int event)
{
	struct hmbird_sched_task_stats *sts = &(get_hmbird_ts(p)->sts);
	u32 *hist = &sts->sum_history[0];
	int i;
	u32 max = 0, avg, demand;
	u64 sum = 0;
	u16 demand_scaled;

	/* Ignore windows where task had no activity */
	if (!runtime || is_idle_task(p) || !samples)
		goto done;

	/* Push new 'runtime' value onto stack */
	for (; samples > 0; samples--) {
		hist[sts->cidx] = runtime;
		sts->cidx = ++(sts->cidx) % RAVG_HIST_SIZE;
	}

	for (i = 0; i < RAVG_HIST_SIZE; i++) {
		sum += hist[i];
		if (hist[i] > max)
			max = hist[i];
	}

	sts->sum = 0;
	avg = div64_u64(sum, RAVG_HIST_SIZE);

	switch (slim_walt_policy) {
	case WINDOW_STATS_RECENT:
		demand = runtime;
		break;
	case WINDOW_STATS_MAX:
		demand = max;
		break;
	case WINDOW_STATS_AVG:
		demand = avg;
		break;
	default:
		demand = max(avg, runtime);
	}

	demand_scaled = scale_time_to_util(demand);

	sts->demand = demand;
	sts->demand_scaled = demand_scaled;

done:
	return;
}


static u64 update_task_demand(struct task_struct *p, struct rq *rq,
			       int event, u64 wallclock)
{
	struct hmbird_sched_task_stats *sts = &(get_hmbird_ts(p)->sts);

	u64 mark_start = sts->mark_start;
	struct hmbird_sched_rq_stats *srq = &per_cpu(hmbird_sched_rq_stats, cpu_of(rq));


	u64 delta, window_start = srq->window_start;
	int new_window, nr_full_windows;
	u32 window_size = hmbird_sched_ravg_window;
	u64 runtime;

	new_window = mark_start < window_start;
	if (!account_busy_for_task_demand(rq, p, event)) {
		if (new_window)
			/*
			 * If the time accounted isn't being accounted as
			 * busy time, and a new window started, only the
			 * previous window need be closed out with the
			 * pre-existing demand. Multiple windows may have
			 * elapsed, but since empty windows are dropped,
			 * it is not necessary to account those.
			 */
			update_history(rq, p, sts->sum, 1, event);
		return 0;
	}

	if (!new_window) {
		/*
		 * The simple case - busy time contained within the existing
		 * window.
		 */
		return add_to_task_demand(rq, p, wallclock - mark_start);
	}

	/*
	 * Busy time spans at least two windows. Temporarily rewind
	 * window_start to first window boundary after mark_start.
	 */
	delta = window_start - mark_start;
	nr_full_windows = div64_u64(delta, window_size);
	window_start -= (u64)nr_full_windows * (u64)window_size;

	/* Process (window_start - mark_start) first */
	runtime = add_to_task_demand(rq, p, window_start - mark_start);

	/* Push new sample(s) into task's demand history */
	update_history(rq, p, sts->sum, 1, event);
	if (nr_full_windows) {
		u64 scaled_window = scale_exec_time(window_size, rq);

		update_history(rq, p, scaled_window, nr_full_windows, event);
		runtime += nr_full_windows * scaled_window;
	}

	/*
	 * Roll window_start back to current to process any remainder
	 * in current window.
	 */
	window_start += (u64)nr_full_windows * (u64)window_size;

	/* Process (wallclock - window_start) next */
	mark_start = window_start;
	runtime += add_to_task_demand(rq, p, wallclock - mark_start);

	return runtime;
}

u16 slim_walt_cpu_util(int cpu)
{
	u64 prev_runnable_sum;
	struct hmbird_sched_rq_stats *srq = &per_cpu(hmbird_sched_rq_stats, cpu);

	prev_runnable_sum = srq->prev_runnable_sum;
	do_div(prev_runnable_sum, srq->prev_window_size >> SCX_SCHED_CAPACITY_SHIFT);

	return (u16)prev_runnable_sum;
}

static DEFINE_PER_CPU(u16, prev_cpu_util);
static inline void cpu_util_update_systrace_c(int cpu)
{
	char buf[256];
	u16 cpu_util = slim_walt_cpu_util(cpu);

	if (cpu_util != per_cpu(prev_cpu_util, cpu)) {
		snprintf(buf, sizeof(buf), "C|9999|Cpu%d_util|%u\n",
						cpu, cpu_util);
		tracing_mark_write(buf);
		per_cpu(prev_cpu_util, cpu) = cpu_util;
	}
}


static inline int account_busy_for_cpu_time(struct rq *rq,
					    struct task_struct *p,
					    int event)
{
	return !is_idle_task(p) && (event == PUT_PREV_TASK
					|| event == TASK_UPDATE);
}


static void update_cpu_busy_time(struct task_struct *p, struct rq *rq,
				 int event, u64 wallclock)
{
	int new_window, full_window = 0;
	struct hmbird_sched_task_stats *sts = &(get_hmbird_ts(p)->sts);
	u64 mark_start = sts->mark_start;
	struct hmbird_sched_rq_stats *srq = &per_cpu(hmbird_sched_rq_stats, cpu_of(rq));
	u64 window_start = srq->window_start;
	u32 window_size = srq->prev_window_size;
	u64 delta;
	u64 *curr_runnable_sum = &srq->curr_runnable_sum;
	u64 *prev_runnable_sum = &srq->prev_runnable_sum;

	new_window = mark_start < window_start;
	if (new_window)
		full_window = (window_start - mark_start) >= window_size;


	if (!account_busy_for_cpu_time(rq, p, event))
		goto done;


	if (!new_window) {
		/*
		 * account_busy_for_cpu_time() = 1 so busy time needs
		 * to be accounted to the current window. No rollover
		 * since we didn't start a new window. An example of this is
		 * when a task starts execution and then sleeps within the
		 * same window.
		 */
		delta = wallclock - mark_start;

		delta = scale_exec_time(delta, rq);
		*curr_runnable_sum += delta;

		goto done;
	}

	/*
	 * situations below this need window rollover,
	 * Rollover of cpu counters (curr/prev_runnable_sum) should have already be done
	 * in update_window_start()
	 *
	 * For task counters curr/prev_window[_cpu] are rolled over in the early part of
	 * this function. If full_window(s) have expired and time since last update needs
	 * to be accounted as busy time, set the prev to a complete window size time, else
	 * add the prev window portion.
	 *
	 * For task curr counters a new window has begun, always assign
	 */

	/*
	 * account_busy_for_cpu_time() = 1 so busy time needs
	 * to be accounted to the current window. A new window
	 * must have been started in udpate_window_start()
	 * If any of these three above conditions are true
	 * then this busy time can't be accounted as irqtime.
	 *
	 * Busy time for the idle task need not be accounted.
	 *
	 * An example of this would be a task that starts execution
	 * and then sleeps once a new window has begun.
	 */

	/*
	 * A full window hasn't elapsed, account partial
	 * contribution to previous completed window.
	 */


	delta = full_window ? scale_exec_time(window_size, rq) :
				scale_exec_time(window_start - mark_start, rq);

	*prev_runnable_sum += delta;

	/* Account piece of busy time in the current window. */
	delta = scale_exec_time(wallclock - window_start, rq);
	*curr_runnable_sum += delta;

done:
	if (slim_walt_dump && new_window)
		cpu_util_update_systrace_c(rq->cpu);
}


void slim_walt_window_rollover_run_once(u64 old_window_start, struct rq *rq)
{
	u64 result;
	struct hmbird_sched_rq_stats *srq = &per_cpu(hmbird_sched_rq_stats, cpu_of(rq));
	u64 new_window_start = srq->window_start;

	if (old_window_start == new_window_start)
		return;

	result = atomic64_cmpxchg(&hmbird_irq_work_lastq_ws,
					old_window_start, new_window_start);
	if (result != old_window_start)
		return;

	if (likely(cpu_online(raw_smp_processor_id())))
		irq_work_queue(&hmbird_slim_walt_irq_work);
	else
		irq_work_queue_on(&hmbird_slim_walt_irq_work, cpumask_any(cpu_online_mask));
}

/*
 * In the core scheduler, most of the load update points update the rq_clock after
 * holding the rq lock. We can directly use rq_clock to reduce the overhead of
 * obtaining the time, but to prevent the subsequent migration of the load update
 * point to before the update of rq_clock, we wrap the judgment of the rq_clock
 * update here.
 */
void hmbird_update_task_ravg_rqclock_wrapper(struct task_struct *p,
				struct rq *rq, int event)
{
	if (!(rq->clock_update_flags & RQCF_UPDATED))
		update_rq_clock(rq);

	struct hmbird_sched_rq_stats *srq = &per_cpu(hmbird_sched_rq_stats, cpu_of(rq));

	hmbird_update_task_ravg(p, rq, event, max(rq_clock(rq), srq->latest_clock));
}

void hmbird_update_task_ravg(struct task_struct *p,
				struct rq *rq, int event, u64 wallclock)
{
	struct hmbird_sched_task_stats *sts = &(get_hmbird_ts(p)->sts);
	struct hmbird_sched_rq_stats *srq = &per_cpu(hmbird_sched_rq_stats, cpu_of(rq));
	u64 old_window_start;

	if (!slim_walt_ctrl)
		return;

	if (!srq->window_start || sts->mark_start == wallclock)
		return;

	old_window_start = update_window_start(rq, wallclock, event);

	if (!sts->window_start)
		sts->window_start = srq->window_start;

	if (!sts->mark_start)
		goto done;

	update_task_rq_cpu_cycles(p, rq, event, wallclock);
	update_task_demand(p, rq, event, wallclock);
	update_cpu_busy_time(p, rq, event, wallclock);

	sts->window_start = srq->window_start;

done:
	sts->mark_start = wallclock;
	slim_walt_window_rollover_run_once(old_window_start, rq);
}

static void slim_walt_irq_work(struct irq_work *irq_work)
{
	cpumask_t lock_cpus;
	struct hmbird_sched_rq_stats *srq;
	struct rq *rq;
	int cpu;
	int level = 0;
	u64 wc;
	unsigned long flags;

	cpumask_copy(&lock_cpus, cpu_possible_mask);

	for_each_cpu(cpu, &lock_cpus) {
		if (level == 0)
			raw_spin_lock(&cpu_rq(cpu)->__lock);
		else
			raw_spin_lock_nested(&cpu_rq(cpu)->__lock, level);
		level++;
	}

	wc = sched_clock();

	for_each_cpu(cpu, &lock_cpus) {
		rq = cpu_rq(cpu);
		hmbird_update_task_ravg(rq->curr, rq, TASK_UPDATE, wc);
	}

	cpufreq_update_util(cpu_rq(0), HMBIRD_CPUFREQ_WINDOW_ROLLOVER);
	spin_lock_irqsave(&new_sched_ravg_window_lock, flags);
	if (unlikely(new_hmbird_sched_ravg_window != hmbird_sched_ravg_window)) {
		srq = &per_cpu(hmbird_sched_rq_stats, smp_processor_id());
		if (wc < srq->window_start + new_hmbird_sched_ravg_window)
			hmbird_sched_ravg_window = new_hmbird_sched_ravg_window;
	}
	spin_unlock_irqrestore(&new_sched_ravg_window_lock, flags);

	for_each_cpu(cpu, &lock_cpus) {
		raw_spin_unlock(&cpu_rq(cpu)->__lock);
	}
}
static void hmbird_sched_init_rq(struct rq *rq)
{
	struct hmbird_sched_rq_stats *srq = &per_cpu(hmbird_sched_rq_stats, cpu_of(rq));

	srq->prev_window_size = hmbird_sched_ravg_window;
	srq->task_exec_scale = 1024;
	srq->window_start = 0;
}

void hmbird_sched_init_task(struct task_struct *p)
{
	struct hmbird_sched_task_stats *sts = &(get_hmbird_ts(p)->sts);

	memset(sts, 0, sizeof(struct hmbird_sched_task_stats));
}

static void hmbird_sched_stats_init(void)
{
	unsigned long flags;
	int cpu;

	for_each_possible_cpu(cpu) {
		struct rq *rq = cpu_rq(cpu);

		raw_spin_lock_irqsave(&rq->__lock, flags);
		hmbird_sched_init_rq(rq);
		raw_spin_unlock_irqrestore(&rq->__lock, flags);
	}
	slim_walt_policy = WINDOW_STATS_MAX_RECENT_AVG;

	if (false == init_irq_work_inited) {
		init_irq_work(&hmbird_slim_walt_irq_work, slim_walt_irq_work);
		init_irq_work_inited = true;
	}
}

void hmbird_scheduler_tick(void)
{
	int cpu = smp_processor_id();
	struct rq *rq = cpu_rq(cpu);

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
		atomic64_set(&hmbird_irq_work_lastq_ws, tick_sched_clock);
	}
}

void slim_walt_enable(int enable)
{
	if (1 == !!enable) {
		hmbird_sched_stats_init();
		WRITE_ONCE(tick_sched_clock, 0);
	} else
		slim_walt_ctrl = 0;
}

void slim_get_cpu_util(int cpu, u64 *util)
{
	if (cpu < 0)
		return;

	*util = slim_walt_cpu_util(cpu);
}

void slim_get_task_util(struct task_struct *p, u64 *util)
{
	if (p == NULL)
		return;

	*util = get_hmbird_ts(p)->sts.demand_scaled;
}

void sched_ravg_window_change(int frame_per_sec)
{
	unsigned long flags;

	spin_lock_irqsave(&new_sched_ravg_window_lock, flags);
	new_hmbird_sched_ravg_window = NSEC_PER_SEC / frame_per_sec;
	spin_unlock_irqrestore(&new_sched_ravg_window_lock, flags);
}
