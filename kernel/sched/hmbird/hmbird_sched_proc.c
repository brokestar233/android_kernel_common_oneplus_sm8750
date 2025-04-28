// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Oplus. All rights reserved.
 */

#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include "hmbird_sched_proc.h"
#include <linux/sched/hmbird_version.h>

#include "hmbird_util_track.h"
#include "slim.h"

#define HMBIRD_SCHED_PROC_DIR "hmbird_sched"
#define SLIM_FREQ_GOV_DIR       "slim_freq_gov"
#define LOAD_TRACK_DIR          "slim_walt"
#define HMBIRD_PROC_PERMISSION  0666

int scx_enable;
int partial_enable;
int cpuctrl_high_ratio = 55;
int cpuctrl_low_ratio = 40;
int slim_stats;
int hmbirdcore_debug;
int slim_for_app;
int misfit_ds = 90;
unsigned int highres_tick_ctrl;
unsigned int highres_tick_ctrl_dbg;
int cpu7_tl = 70;
int slim_walt_ctrl;
int slim_walt_dump;
int slim_walt_policy;
int slim_gov_debug;
int scx_gov_ctrl = 1;
int sched_ravg_window_frame_per_sec = 125;
int parctrl_high_ratio = 55;
int parctrl_low_ratio = 40;
int parctrl_high_ratio_l = 65;
int parctrl_low_ratio_l = 50;
int isoctrl_high_ratio = 75;
int isoctrl_low_ratio = 60;
int isolate_ctrl;
int iso_free_rescue;
int heartbeat;
int heartbeat_enable = 1;
int watchdog_enable;
int save_gov;
u64 cpu_cluster_masks;
int hmbird_preempt_policy;
int cluster_separate;

char saved_gov[NR_CPUS][MAX_GOV_LEN];

static int set_proc_buf_val(struct file *file, const char __user *buf, size_t count, int *val)
{
	char kbuf[32] = {0};
	int err;

	if (count >= 32)
		return -EFAULT;

	if (copy_from_user(kbuf, buf, count)) {
		pr_err("hmbird_sched : Failed to copy_from_user\n");
		return -EFAULT;
	}

	err = kstrtoint(strstrip(kbuf), 0, val);
	if (err < 0) {
		pr_err("hmbird_sched: Failed to exec kstrtoint\n");
		return -EFAULT;
	}

	return 0;
}

static int set_proc_buf_val_u64(struct file *file, const char __user *buf,
				size_t count, u64 *val)
{
	char kbuf[32] = {0};
	int err;

	if (count >= sizeof(kbuf))
		return -EFAULT;

	if (copy_from_user(kbuf, buf, count)) {
		pr_err("hmbird_sched : Failed to copy_from_user\n");
		return -EFAULT;
	}

	err = kstrtou64(strstrip(kbuf), 0, val);
	if (err < 0) {
		pr_err("hmbird_sched: Failed to exec kstrtoul\n");
	return -EFAULT;
	}

	return 0;
}

/* common ops begin */
static ssize_t hmbird_common_write(struct file *file,
				   const char __user *buf,
				   size_t count, loff_t *ppos)
{
	int *pval = (int *)pde_data(file_inode(file));

	if (set_proc_buf_val(file, buf, count, pval))
		return -EFAULT;

	return count;
}

static int hmbird_common_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", *(int *) m->private);
	return 0;
}

static int hmbird_common_open(struct inode *inode, struct file *file)
{
	return single_open(file, hmbird_common_show, pde_data(inode));
}

static int hmbird_common_ul_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%lu\n", *(unsigned long *) m->private);
	return 0;
}

static int hmbird_common_ul_open(struct inode *inode, struct file *file)
{
	return single_open(file, hmbird_common_ul_show, pde_data(inode));
}

HMBIRD_PROC_OPS(hmbird_common, hmbird_common_open, hmbird_common_write);
/* common ops end */

/* scx_enable ops begin */
static ssize_t scx_enable_proc_write(struct file *file, const char __user *buf,
								size_t count, loff_t *ppos)
{
	int *pval = (int *)pde_data(file_inode(file));

	if (set_proc_buf_val(file, buf, count, pval))
		return -EFAULT;

	WRITE_ONCE(sw_type, HMBIRD_SWITCH_PROC);
	if (hmbird_ctrl(*pval))
		return -EFAULT;

	return count;
}
HMBIRD_PROC_OPS(scx_enable, hmbird_common_open, scx_enable_proc_write);
/* scx_enable ops end */

/* hmbird_stats ops begin */
#define MAX_STATS_BUF	(4096)
static int hmbird_stats_proc_show(struct seq_file *m, void *v)
{
	char *buf;

	buf = kmalloc(MAX_STATS_BUF, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	stats_print(buf, MAX_STATS_BUF);

	seq_printf(m, "%s\n", buf);

	kfree(buf);
	return 0;
}

static int hmbird_stats_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, hmbird_stats_proc_show, inode);
}
HMBIRD_PROC_OPS(hmbird_stats, hmbird_stats_proc_open, NULL);
/* hmbird_stats ops end */

/* sched_ravg_window_frame_per_sec ops begin */
static ssize_t sched_ravg_window_frame_per_sec_proc_write(struct file *file,
			const char __user *buf, size_t count, loff_t *ppos)
{
	int *pval = (int *)pde_data(file_inode(file));

	if (set_proc_buf_val(file, buf, count, pval))
		return -EFAULT;

	sched_ravg_window_change(*pval);

	return count;
}
HMBIRD_PROC_OPS(sched_ravg_window_frame_per_sec, hmbird_common_open,
			sched_ravg_window_frame_per_sec_proc_write);
/* sched_ravg_window_frame_per_sec ops end */

static ssize_t save_gov_str(struct file *file, const char __user *buf,
					size_t count, loff_t *ppos)
{
	int cpu;
	struct cpufreq_policy *policy;

	for_each_possible_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy || (cpu != policy->cpu))
			continue;
		WARN_ON(show_scaling_governor(policy, saved_gov[cpu]) <= 0);
		hmbird_info_systrace("<gov_restore>:save origin gov : %s\n", saved_gov[cpu]);
	}
	return count;
}
HMBIRD_PROC_OPS(save_gov, hmbird_common_open, save_gov_str);

static ssize_t cpu_cluster_proc_write(struct file *file, const char __user *buf,
							size_t count, loff_t *ppos)
{
	u64 *pval = (u64 *)pde_data(file_inode(file));

	if (set_proc_buf_val_u64(file, buf, count, pval))
		return -EFAULT;

	if (scx_enable == 0)
		set_cpu_cluster(*pval);

	return count;
}
HMBIRD_PROC_OPS(cpu_cluster_masks, hmbird_common_ul_open, cpu_cluster_proc_write);

static ssize_t slim_walt_ctrl_write(struct file *file, const char __user *buf,
					size_t count, loff_t *ppos)
{
	int *pval = (int *)pde_data(file_inode(file));
	int tmp_val;

	if (set_proc_buf_val(file, buf, count, &tmp_val))
		return -EFAULT;

	slim_walt_enable(tmp_val);
	*pval = tmp_val;

	return count;
}

HMBIRD_PROC_OPS(slim_walt_ctrl, hmbird_common_open, slim_walt_ctrl_write);

/* yield_opt ops begin */
static int yield_opt_show(struct seq_file *m, void *v)
{
	struct yield_opt_params *data = m->private;

	seq_printf(m, "yield_opt:{\"enable\":%d; \"frame_per_sec\":%d; \"headroom\":%d}\n",
				data->enable, data->frame_per_sec, data->yield_headroom);
	return 0;
}

static int yield_opt_open(struct inode *inode, struct file *file)
{
	return single_open(file, yield_opt_show, pde_data(inode));
}

static ssize_t yield_opt_write(struct file *file, const char __user *buf,
							size_t count, loff_t *ppos)
{
	char *data;
	int enable_tmp, frame_per_sec_tmp, yield_headroom_tmp, cpu;
	unsigned long flags;

	data = kmalloc(count + 1, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	if (copy_from_user(data, buf, count)) {
		kfree(data);
		return -EFAULT;
	}

	data[count] = '\0';

	if (sscanf(data, "%d %d %d", &enable_tmp, &frame_per_sec_tmp, &yield_headroom_tmp) != 3) {
		kfree(data);
		return -EINVAL;
	}

	if ((enable_tmp != 0 && enable_tmp != 1) || (frame_per_sec_tmp != 30 && frame_per_sec_tmp
			!= 60 && frame_per_sec_tmp != 90 && frame_per_sec_tmp != 120) ||
			(yield_headroom_tmp < 1 || yield_headroom_tmp > 20)) {
		kfree(data);
		return -EINVAL;
	}

	yield_opt_params.frame_time_ns = NSEC_PER_SEC / frame_per_sec_tmp;
	yield_opt_params.frame_per_sec = frame_per_sec_tmp;
	yield_opt_params.yield_headroom = yield_headroom_tmp;
	yield_opt_params.enable = enable_tmp;

	for_each_possible_cpu(cpu) {
		struct sched_yield_state *ys = &per_cpu(ystate, cpu);

		raw_spin_lock_irqsave(&ys->lock, flags);
		ys->last_yield_time = 0;
		ys->last_update_time = 0;
		ys->sleep_end = 0;
		ys->yield_cnt = 0;
		ys->yield_cnt_after_sleep = 0;
		ys->sleep = 0;
		ys->sleep_times = 0;
		raw_spin_unlock_irqrestore(&ys->lock, flags);
	}

	kfree(data);
	return count;
}

HMBIRD_PROC_OPS(yield_opt, yield_opt_open, yield_opt_write);


static int __init hmbird_proc_init(void)
{
	struct proc_dir_entry *hmbird_dir;
	struct proc_dir_entry *load_track_dir;
	struct proc_dir_entry *freq_gov_dir;

	if (get_hmbird_version_type() != HMBIRD_OGKI_VERSION)
		return 0;

	/* mkdir /proc/hmbird_sched */
	hmbird_dir = proc_mkdir(HMBIRD_SCHED_PROC_DIR, NULL);
	if (!hmbird_dir) {
		pr_err("Error creating proc directory %s\n", HMBIRD_SCHED_PROC_DIR);
		return -ENOMEM;
	}

	/* /proc/hmbird_sched--begin */
	HMBIRD_CREATE_PROC_ENTRY_DATA("scx_enable", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&scx_enable_proc_ops,
					&scx_enable);

	HMBIRD_CREATE_PROC_ENTRY_DATA("partial_ctrl", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_common_proc_ops,
					&partial_enable);

	HMBIRD_CREATE_PROC_ENTRY_DATA("cpuctrl_high", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_common_proc_ops,
					&cpuctrl_high_ratio);

	HMBIRD_CREATE_PROC_ENTRY_DATA("cpuctrl_low", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_common_proc_ops,
					&cpuctrl_low_ratio);

	HMBIRD_CREATE_PROC_ENTRY_DATA("slim_stats", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_common_proc_ops,
					&slim_stats);

	HMBIRD_CREATE_PROC_ENTRY_DATA("hmbirdcore_debug", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_common_proc_ops,
					&hmbirdcore_debug);

	HMBIRD_CREATE_PROC_ENTRY_DATA("slim_for_app", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_common_proc_ops,
					&slim_for_app);

	HMBIRD_CREATE_PROC_ENTRY_DATA("misfit_ds", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_common_proc_ops,
					&misfit_ds);

	HMBIRD_CREATE_PROC_ENTRY_DATA("scx_shadow_tick_enable", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_common_proc_ops,
					&highres_tick_ctrl);

	HMBIRD_CREATE_PROC_ENTRY_DATA("highres_tick_ctrl_dbg", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_common_proc_ops,
					&highres_tick_ctrl_dbg);

	HMBIRD_CREATE_PROC_ENTRY_DATA("cpu7_tl", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_common_proc_ops,
					&cpu7_tl);

	HMBIRD_CREATE_PROC_ENTRY_DATA("cpu_cluster_masks", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&cpu_cluster_masks_proc_ops,
					&cpu_cluster_masks);

	HMBIRD_CREATE_PROC_ENTRY_DATA("save_gov", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&save_gov_proc_ops,
					&save_gov);

	HMBIRD_CREATE_PROC_ENTRY_DATA("heartbeat", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_common_proc_ops,
					&heartbeat);

	HMBIRD_CREATE_PROC_ENTRY_DATA("heartbeat_enable", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_common_proc_ops,
					&heartbeat_enable);

	HMBIRD_CREATE_PROC_ENTRY_DATA("watchdog_enable", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_common_proc_ops,
					&watchdog_enable);

	HMBIRD_CREATE_PROC_ENTRY_DATA("isolate_ctrl", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_common_proc_ops,
					&isolate_ctrl);

	HMBIRD_CREATE_PROC_ENTRY_DATA("parctrl_high_ratio", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_common_proc_ops,
					&parctrl_high_ratio);

	HMBIRD_CREATE_PROC_ENTRY_DATA("parctrl_low_ratio", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_common_proc_ops,
					&parctrl_low_ratio);

	HMBIRD_CREATE_PROC_ENTRY_DATA("isoctrl_high_ratio", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_common_proc_ops,
					&isoctrl_high_ratio);

	HMBIRD_CREATE_PROC_ENTRY_DATA("isoctrl_low_ratio", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_common_proc_ops,
					&isoctrl_low_ratio);

	HMBIRD_CREATE_PROC_ENTRY_DATA("iso_free_rescue", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_common_proc_ops,
					&iso_free_rescue);

	HMBIRD_CREATE_PROC_ENTRY_DATA("parctrl_high_ratio_l", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_common_proc_ops,
					&parctrl_high_ratio_l);

	HMBIRD_CREATE_PROC_ENTRY_DATA("parctrl_low_ratio_l", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_common_proc_ops,
					&parctrl_low_ratio_l);

	HMBIRD_CREATE_PROC_ENTRY("hmbird_stats", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_stats_proc_ops);

	HMBIRD_CREATE_PROC_ENTRY_DATA("yield_opt", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&yield_opt_proc_ops,
					&yield_opt_params);

	HMBIRD_CREATE_PROC_ENTRY_DATA("hmbird_preempt_policy", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_common_proc_ops,
					&hmbird_preempt_policy);
	/* /proc/hmbird_sched--end */

	/* mkdir /proc/hmbird_sched/slim_walt */
	load_track_dir = proc_mkdir(LOAD_TRACK_DIR, hmbird_dir);
	if (!load_track_dir) {
		pr_err("Error creating proc directory %s\n", LOAD_TRACK_DIR);
		return -ENOMEM;
	}

	/* /proc/hmbird_sched/slim_walt--begin */
	HMBIRD_CREATE_PROC_ENTRY_DATA("slim_walt_ctrl", HMBIRD_PROC_PERMISSION,
					load_track_dir,
					&slim_walt_ctrl_proc_ops,
					&slim_walt_ctrl);

	HMBIRD_CREATE_PROC_ENTRY_DATA("slim_walt_dump", HMBIRD_PROC_PERMISSION,
					load_track_dir,
					&hmbird_common_proc_ops,
					&slim_walt_dump);

	HMBIRD_CREATE_PROC_ENTRY_DATA("slim_walt_policy", HMBIRD_PROC_PERMISSION,
					load_track_dir,
					&hmbird_common_proc_ops,
					&slim_walt_policy);

	HMBIRD_CREATE_PROC_ENTRY_DATA("frame_per_sec", HMBIRD_PROC_PERMISSION,
					load_track_dir,
					&sched_ravg_window_frame_per_sec_proc_ops,
					&sched_ravg_window_frame_per_sec);
	/* /proc/hmbird_sched/slim_walt--end */

	/* mkdir /proc/hmbird_sched/slim_freq_gov */
	freq_gov_dir = proc_mkdir(SLIM_FREQ_GOV_DIR, hmbird_dir);
	if (!freq_gov_dir) {
		pr_err("Error creating proc directory %s\n", SLIM_FREQ_GOV_DIR);
		return -ENOMEM;
	}

	/* /proc/hmbird_sched/slim_freq_gov--begin */
	HMBIRD_CREATE_PROC_ENTRY_DATA("slim_gov_debug", HMBIRD_PROC_PERMISSION,
					freq_gov_dir,
					&hmbird_common_proc_ops,
					&slim_gov_debug);
	HMBIRD_CREATE_PROC_ENTRY_DATA("scx_gov_ctrl", HMBIRD_PROC_PERMISSION,
					freq_gov_dir,
					&hmbird_common_proc_ops,
					&scx_gov_ctrl);
	/* /proc/hmbird_sched/slim_freq_gov--end */

	HMBIRD_CREATE_PROC_ENTRY_DATA("cluster_separate", HMBIRD_PROC_PERMISSION,
					hmbird_dir,
					&hmbird_common_proc_ops,
					&cluster_separate);

	return 0;
}

device_initcall(hmbird_proc_init);
