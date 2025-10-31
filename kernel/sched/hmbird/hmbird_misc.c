// SPDX-License-Identifier: GPL-2.0-only

/*
 * @Description:
 * @Version:
 * @Author: wangrui8
 * Copyright (C) 2024 Oplus. All rights reserved.
 */

#include <linux/kprobes.h>
#include "hmbird_sched.h"
#include <linux/sched/hmbird.h>
#include <linux/kernel.h>
#include "slim.h"

noinline int tracing_mark_write(const char *buf)
{
	trace_printk(buf);
	return 0;
}

struct yield_opt_params yield_opt_params = {
	.enable = 0,
	.frame_per_sec = 120,
	.frame_time_ns = NSEC_PER_SEC / 120,
	.yield_headroom = 10,
};

DEFINE_PER_CPU(struct sched_yield_state, ystate);

static inline void hmbird_yield_state_update(struct sched_yield_state *ys)
{
	if (!raw_spin_is_locked(&ys->lock))
		return;
	int yield_headroom = yield_opt_params.yield_headroom;

	if (ys->yield_cnt >= DEFAULT_YIELD_SLEEP_TH || ys->sleep_times > 1
						|| ys->yield_cnt_after_sleep > yield_headroom) {
		ys->sleep = min(ys->sleep + yield_headroom * YIELD_DURATION, MAX_YIELD_SLEEP);
	} else if (!ys->yield_cnt && (ys->sleep_times == 1) && !ys->yield_cnt_after_sleep) {
		ys->sleep = max(ys->sleep - yield_headroom * YIELD_DURATION, MIN_YIELD_SLEEP);
	}
	ys->yield_cnt = 0;
	ys->sleep_times = 0;
	ys->yield_cnt_after_sleep = 0;
}

void hmbird_skip_yield(long *skip)
{
	if (!get_hmbird_ops_enabled() || !yield_opt_params.enable)
		return;
	unsigned long flags, sleep_now = 0;
	struct sched_yield_state *ys;
	int cpu = raw_smp_processor_id(), cont_yield, new_frame;
	int frame_time_ns = yield_opt_params.frame_time_ns;
	int yield_headroom = yield_opt_params.yield_headroom;
	u64 wc;

	if (!(*skip)) {
		wc = sched_clock();
		ys = &per_cpu(ystate, cpu);
		raw_spin_lock_irqsave(&ys->lock, flags);

		cont_yield = (wc - ys->last_yield_time) < MIN_YIELD_SLEEP;
		new_frame = (wc - ys->last_update_time) > (frame_time_ns >> 1);

		if (!cont_yield && new_frame) {
			hmbird_yield_state_update(ys);
			ys->last_update_time = wc;
			ys->sleep_end = ys->last_yield_time + frame_time_ns
						- yield_headroom * YIELD_DURATION;
		}

		if (ys->sleep > MIN_YIELD_SLEEP || ys->yield_cnt >= DEFAULT_YIELD_SLEEP_TH) {
			*skip = true;

			sleep_now = ys->sleep_times ?
				max(ys->sleep >> ys->sleep_times, MIN_YIELD_SLEEP):ys->sleep;
			if (wc + sleep_now > ys->sleep_end) {
				u64 delta = ys->sleep_end - wc;

				if (ys->sleep_end > wc && delta > 3 * YIELD_DURATION)
					sleep_now = delta;
				else
					sleep_now = 0;
			}
			raw_spin_unlock_irqrestore(&ys->lock, flags);
			if (sleep_now) {
				sleep_now = div64_u64(sleep_now, 1000);
				usleep_range_state(sleep_now, sleep_now, TASK_IDLE);
			}
			ys->sleep_times++;
			ys->last_yield_time = sched_clock();
			return;
		}
		if (ys->sleep_times)
			ys->yield_cnt_after_sleep++;
		else
			(ys->yield_cnt)++;
		ys->last_yield_time = wc;
		raw_spin_unlock_irqrestore(&ys->lock, flags);
	}
}

struct boost_policy_params boost_policy_params = {
	.enable = 0,
	.bottom_freq = 1200000,
	.boost_weight = 120,
};

int hmbird_get_boost_enable(void)
{
	return boost_policy_params.enable;
}

unsigned int hmbird_get_boost_bottom_freq(void)
{
	return boost_policy_params.bottom_freq;
}

int hmbird_get_boost_weight(void)
{
	return boost_policy_params.boost_weight;
}

void hmbird_ops_init(struct hmbird_ops *hmbird_ops)
{
	hmbird_ops->scx_enable = get_hmbird_ops_enabled;
	hmbird_ops->check_non_task = get_non_hmbird_task;
	hmbird_ops->hmbird_get_md_info = hmbird_get_md_info;
	hmbird_ops->hmbird_get_boost_enable = hmbird_get_boost_enable;
	hmbird_ops->hmbird_get_boost_bottom_freq = hmbird_get_boost_bottom_freq;
	hmbird_ops->hmbird_get_boost_weight = hmbird_get_boost_weight;
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

