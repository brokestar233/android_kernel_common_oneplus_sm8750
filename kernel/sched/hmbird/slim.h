/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __SLIM_H
#define __SLIM_H

extern atomic_t __hmbird_ops_enabled;
extern atomic_t non_hmbird_task;
extern int cgroup_ids_table[NUMS_CGROUP_KINDS];
extern int heartbeat;
extern int heartbeat_enable;
extern int watchdog_enable;
extern int isolate_ctrl;
extern int parctrl_high_ratio;
extern int parctrl_low_ratio;
extern int isoctrl_high_ratio;
extern int isoctrl_low_ratio;
extern int iso_free_rescue;
extern int yield_opt;

extern enum hmbird_switch_type sw_type;
extern noinline int tracing_mark_write(const char *buf);
int task_top_id(struct task_struct *p);
void stats_print(char *buf, int len);
void hmbird_skip_yield(long *skip);
extern spinlock_t hmbird_tasks_lock;

struct yield_opt_params {
	int enable;
	int frame_per_sec;
	u64 frame_time_ns;
	int yield_headroom;
};

extern struct yield_opt_params yield_opt_params;

#define MAX_GOV_LEN     (16)
extern char saved_gov[NR_CPUS][MAX_GOV_LEN];

#endif
