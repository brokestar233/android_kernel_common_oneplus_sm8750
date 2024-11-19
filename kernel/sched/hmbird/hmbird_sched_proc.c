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

#include "slim.h"

#define HMBIRD_SCHED_PROC_DIR "hmbird_sched"
#define SLIM_FREQ_GOV_DIR       "slim_freq_gov"
#define LOAD_TRACK_DIR          "slim_walt"

int scx_enable;
int partial_enable;
int cpuctrl_high_ratio = 55;
int cpuctrl_low_ratio = 40;
int slim_stats;
int hmbirdcore_debug = DEBUG_INTERNAL;
int slim_for_app;
int misfit_ds = 40;
unsigned int highres_tick_ctrl;
unsigned int highres_tick_ctrl_dbg;
int cpu7_tl = 70;
int slim_walt_ctrl;
int slim_walt_dump;
int slim_walt_policy;
int slim_gov_debug;
int scx_gov_ctrl = 1;
int sched_ravg_window_frame_per_sec = 125;

static int set_proc_buf_val(struct file *file, const char __user *buf, size_t count, int *val)
{
	char kbuf[5] = {0};
	int err;

	if (count >= 5)
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

/* common ops begin */
static ssize_t hmbird_common_write(struct file *file,
				   const char __user *buf,
				   size_t count, loff_t *ppos)
{
	int *pval = (int *)pde_data(file_inode(file));

	if(set_proc_buf_val(file, buf, count, pval)) {
		return -EFAULT;
	}

	return count;
}

static int hmbird_common_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", *(int*) m->private);
	return 0;
}

static int hmbird_common_open(struct inode *inode, struct file *file)
{
	return single_open(file, hmbird_common_show, pde_data(inode));
}
HMBIRD_PROC_OPS(hmbird_common, hmbird_common_open, hmbird_common_write);
/* common ops end */

/* scx_enable ops begin */
static ssize_t scx_enable_proc_write(struct file *file, const char __user *buf,
								size_t count, loff_t *ppos)
{
	int *pval = (int *)pde_data(file_inode(file));

	if(set_proc_buf_val(file, buf, count, pval)) {
		return -EFAULT;
	}

	hmbird_ctrl(*pval);

	return count;
}
HMBIRD_PROC_OPS(scx_enable, hmbird_common_open, scx_enable_proc_write);
/* scx_enable ops end */

/* hmbird_stats ops begin */
#define MAX_STATS_BUF	(2000)
static int hmbird_stats_proc_show(struct seq_file *m, void *v)
{
	char buf[MAX_STATS_BUF] = {0};

	stats_print(buf, MAX_STATS_BUF);

	seq_printf(m, "%s\n", buf);
	return 0;
}

static int hmbird_stats_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, hmbird_stats_proc_show, inode);
}
HMBIRD_PROC_OPS(hmbird_stats, hmbird_stats_proc_open, NULL);
/* hmbird_stats ops end */

/* sched_ravg_window_frame_per_sec ops begin */
static ssize_t sched_ravg_window_frame_per_sec_proc_write(struct file *file, const char __user *buf,
								size_t count, loff_t *ppos)
{
	int *pval = (int *)pde_data(file_inode(file));

	if(set_proc_buf_val(file, buf, count, pval)) {
		return -EFAULT;
	}

#ifdef CONFIG_SCX_USE_UTIL_TRACK
	sched_ravg_window_change(*pval);
#endif

	return count;
}
HMBIRD_PROC_OPS(sched_ravg_window_frame_per_sec, hmbird_common_open, sched_ravg_window_frame_per_sec_proc_write);
/* sched_ravg_window_frame_per_sec ops end */

static int __init hmbird_proc_init(void) {
	struct proc_dir_entry *hmbird_dir;
	struct proc_dir_entry *load_track_dir;
	struct proc_dir_entry *freq_gov_dir;

	/* mkdir /proc/hmbird_sched */
	hmbird_dir = proc_mkdir(HMBIRD_SCHED_PROC_DIR, NULL);
	if (!hmbird_dir) {
		pr_err("Error creating proc directory %s\n", HMBIRD_SCHED_PROC_DIR);
		return -ENOMEM;
	}

	/* /proc/hmbird_sched--begin */
	HMBIRD_CREATE_PROC_ENTRY_DATA("scx_enable", S_IRUGO | S_IWUGO, \
					hmbird_dir, \
					&scx_enable_proc_ops, \
					&scx_enable);

	HMBIRD_CREATE_PROC_ENTRY_DATA("partial_ctrl", S_IRUGO | S_IWUGO, \
					hmbird_dir, \
					&hmbird_common_proc_ops, \
					&partial_enable);

	HMBIRD_CREATE_PROC_ENTRY_DATA("cpuctrl_high", S_IRUGO | S_IWUGO, \
					hmbird_dir, \
					&hmbird_common_proc_ops, \
					&cpuctrl_high_ratio);

	HMBIRD_CREATE_PROC_ENTRY_DATA("cpuctrl_low", S_IRUGO | S_IWUGO, \
					hmbird_dir, \
					&hmbird_common_proc_ops, \
					&cpuctrl_low_ratio);

	HMBIRD_CREATE_PROC_ENTRY_DATA("slim_stats", S_IRUGO | S_IWUGO, \
					hmbird_dir, \
					&hmbird_common_proc_ops, \
					&slim_stats);

	HMBIRD_CREATE_PROC_ENTRY_DATA("hmbirdcore_debug", S_IRUGO | S_IWUGO, \
					hmbird_dir, \
					&hmbird_common_proc_ops, \
					&hmbirdcore_debug);

	HMBIRD_CREATE_PROC_ENTRY_DATA("slim_for_app", S_IRUGO | S_IWUGO, \
					hmbird_dir, \
					&hmbird_common_proc_ops, \
					&slim_for_app);

	HMBIRD_CREATE_PROC_ENTRY_DATA("misfit_ds", S_IRUGO | S_IWUGO, \
					hmbird_dir, \
					&hmbird_common_proc_ops, \
					&misfit_ds);

	HMBIRD_CREATE_PROC_ENTRY_DATA("scx_shadow_tick_enable", S_IRUGO | S_IWUGO, \
					hmbird_dir, \
					&hmbird_common_proc_ops, \
					&highres_tick_ctrl);

	HMBIRD_CREATE_PROC_ENTRY_DATA("highres_tick_ctrl_dbg", S_IRUGO | S_IWUGO, \
					hmbird_dir, \
					&hmbird_common_proc_ops, \
					&highres_tick_ctrl_dbg);

	HMBIRD_CREATE_PROC_ENTRY_DATA("cpu7_tl", S_IRUGO | S_IWUGO, \
					hmbird_dir, \
					&hmbird_common_proc_ops, \
					&cpu7_tl);

	HMBIRD_CREATE_PROC_ENTRY("hmbird_stats", S_IRUGO | S_IWUGO, \
					hmbird_dir, \
					&hmbird_stats_proc_ops);
	/* /proc/hmbird_sched--end */

	/* mkdir /proc/hmbird_sched/slim_walt */
	load_track_dir = proc_mkdir(LOAD_TRACK_DIR, hmbird_dir);
	if (!load_track_dir) {
		pr_err("Error creating proc directory %s\n", LOAD_TRACK_DIR);
		return -ENOMEM;
	}

	/* /proc/hmbird_sched/slim_walt--begin */
	HMBIRD_CREATE_PROC_ENTRY_DATA("slim_walt_ctrl", S_IRUGO | S_IWUGO, \
					load_track_dir, \
					&hmbird_common_proc_ops, \
					&slim_walt_ctrl);

	HMBIRD_CREATE_PROC_ENTRY_DATA("slim_walt_dump", S_IRUGO | S_IWUGO, \
					load_track_dir, \
					&hmbird_common_proc_ops, \
					&slim_walt_dump);

	HMBIRD_CREATE_PROC_ENTRY_DATA("slim_walt_policy", S_IRUGO | S_IWUGO, \
					load_track_dir, \
					&hmbird_common_proc_ops, \
					&slim_walt_policy);

	HMBIRD_CREATE_PROC_ENTRY_DATA("frame_per_sec", S_IRUGO | S_IWUGO, \
					load_track_dir, \
					&sched_ravg_window_frame_per_sec_proc_ops, \
					&sched_ravg_window_frame_per_sec);
	/* /proc/hmbird_sched/slim_walt--end */

	/* mkdir /proc/hmbird_sched/slim_freq_gov */
	freq_gov_dir = proc_mkdir(SLIM_FREQ_GOV_DIR, hmbird_dir);
	if (!freq_gov_dir) {
		pr_err("Error creating proc directory %s\n", SLIM_FREQ_GOV_DIR);
		return -ENOMEM;
	}

	/* /proc/hmbird_sched/slim_freq_gov--begin */
	HMBIRD_CREATE_PROC_ENTRY_DATA("slim_gov_debug", S_IRUGO | S_IWUGO, \
					freq_gov_dir, \
					&hmbird_common_proc_ops, \
					&slim_gov_debug);
	HMBIRD_CREATE_PROC_ENTRY_DATA("scx_gov_ctrl", S_IRUGO | S_IWUGO, \
					freq_gov_dir, \
					&hmbird_common_proc_ops, \
					&scx_gov_ctrl);
	/* /proc/hmbird_sched/slim_freq_gov--end */

	return 0;
}

device_initcall(hmbird_proc_init);
