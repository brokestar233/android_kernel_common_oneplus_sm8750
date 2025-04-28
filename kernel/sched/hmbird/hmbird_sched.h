/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024 Oplus. All rights reserved.
 */
#ifndef __HMBIRD_SCHED__
#define __HMBIRD_SCHED__

#include <linux/sched.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/smp.h>
#include <linux/tick.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/irq_work.h>
#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/sched/clock.h>
#include <uapi/linux/sched/types.h>
#include <../../kernel/time/tick-sched.h>
#include <../../kernel/sched/sched.h>
#include <trace/hooks/sched.h>
#include <linux/delay.h>
#include <linux/sched/hmbird.h>

#define REGISTER_TRACE_VH(vender_hook, handler) \
{ \
	ret = register_trace_##vender_hook(handler, NULL); \
	if (ret) { \
		pr_err("failed to register_trace_"#vender_hook", ret=%d\n", ret); \
	} \
}
#define REGISTER_TRACE(vendor_hook, handler, data, err)	\
do {								\
	ret = register_trace_##vendor_hook(handler, data);				\
	if (ret) {						\
		pr_err("sched_ext:failed to register_trace_"#vendor_hook", ret=%d\n", ret);	\
		goto err;					\
	}							\
} while (0)

#define UNREGISTER_TRACE(vendor_hook, handler, data)	\
	unregister_trace_##vendor_hook(handler, data)				\

extern unsigned int highres_tick_ctrl;
extern unsigned int highres_tick_ctrl_dbg;

extern int slim_walt_ctrl;
extern int slim_walt_dump;
extern int slim_walt_policy;
extern int sched_ravg_window_frame_per_sec;
extern int slim_gov_debug;
extern int cpu7_tl;
extern int scx_gov_ctrl;
extern spinlock_t new_sched_ravg_window_lock;
extern int cluster_separate;

#define HMBIRD_CPUFREQ_WINDOW_ROLLOVER	BIT(31)
#define MAX_YIELD_SLEEP		(2000000ULL)
#define MIN_YIELD_SLEEP		(200000ULL)
#define YIELD_DURATION		(5000ULL)
#define DEFAULT_YIELD_SLEEP_TH	(10)

struct sched_yield_state {
	raw_spinlock_t	lock;
	u64				last_yield_time;
	u64				last_update_time;
	u64				sleep_end;
	unsigned long	yield_cnt;
	unsigned long	yield_cnt_after_sleep;
	unsigned long	sleep;
	int sleep_times;
};

DECLARE_PER_CPU(struct sched_yield_state, ystate);

void hmbird_window_rollover_run_once(struct rq *rq);
void hmbird_yield_state_update_per_frame(void);
void hmbird_misc_init(void);

void hmbird_ops_init(struct hmbird_ops *hmbird_ops);
#endif /*__HMBIRD_SCHED__*/
