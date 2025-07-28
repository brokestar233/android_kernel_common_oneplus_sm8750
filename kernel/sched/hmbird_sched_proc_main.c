// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2024 Oplus. All rights reserved.
 */

#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/cpufreq.h>
#include "hmbird_sched_proc.h"
#include "ext.h"

#define HMBIRD_SCHED_PROC_DIR "hmbird_sched"
#define SLIM_FREQ_GOV_DIR       "slim_freq_gov"
#define LOAD_TRACK_DIR          "slim_walt"
#define HMBIRD_PROC_PERMISSION  0666

int scx_enable;
int partial_enable;
int cpuctrl_high_ratio = 55;
int cpuctrl_low_ratio = 40;
int slim_stats;
int hmbirdcore_debug = 0;
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
int heartbeat_enable;
int watchdog_enable;
int save_gov;
unsigned int cpu_cluster_masks;

char saved_gov[NR_CPUS][16];

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
HMBIRD_PROC_OPS(hmbird_common, hmbird_common_open, hmbird_common_write);
/* common ops end */

/* scx_enable ops begin */
static ssize_t scx_enable_proc_write(struct file *file, const char __user *buf,
								size_t count, loff_t *ppos)
{
	int *pval = (int *)pde_data(file_inode(file));

	if (set_proc_buf_val(file, buf, count, pval))
		return -EFAULT;

	return count;
}
HMBIRD_PROC_OPS(scx_enable, hmbird_common_open, scx_enable_proc_write);
/* scx_enable ops end */

/* hmbird_stats ops begin */
#define MAX_STATS_BUF	(2000)
static int hmbird_stats_proc_show(struct seq_file *m, void *v)
{
    	seq_puts(m, "global stat:0, 0\n");
    	seq_puts(m, "cpu_allow_fail:0, 0\n");
    	seq_puts(m, "rt_cnt:0, 0\n");
    	seq_puts(m, "key_task_cnt:0, 0\n");
    	seq_puts(m, "switch_idx:0, 0\n");
    	seq_puts(m, "timeout_cnt:0, 0\n");
    	seq_puts(m, "total_dsp_cnt:0, 0\n");
    	seq_puts(m, "move_rq_cnt:0, 0\n");
	seq_puts(m, "select_cpu:0, 0\n");
    
    	// Output gdsq_cnt array
    	for (int i = 0; i < 10; i++) {
        	seq_printf(m, "gdsq_cnt[%d]:0, 0\n", i);
    	}
    
    	seq_puts(m, "err_idx:0, 0, 0, 0, 0\n");
    
    	// 扩展 pcp_timeout_cnt 输出（增加索引6-7）
    	for (int i = 0; i < 8; i++) {  // 从 6 扩大到 8
        	seq_printf(m, "pcp_timeout_cnt[%d]:0\n", i);
    	}
    
    	// 添加缺失的 pcp_ldsq_cnt 数组输出（索引0-7）
    	for (int i = 0; i < 8; i++) {
        	seq_printf(m, "pcp_ldsq_cnt[%d]:0, 0\n", i);
    	}
    
    	// 添加缺失的 pcp_enql_cnt 数组输出（索引0-7）
    	for (int i = 0; i < 8; i++) {
        	seq_printf(m, "pcp_enql_cnt[%d]:0\n", i);
    	}
    
    	// 原有的调度器状态变量输出
    	seq_printf(m, "SCX Enabled: %d\n", scx_enable);
    	seq_printf(m, "Partial Enable: %d\n", partial_enable);
    	seq_printf(m, "Slim Stats: %d\n", slim_stats);
    	seq_printf(m, "Heartbeat: %d\n", heartbeat);
    	seq_printf(m, "Misfit DS: %d\n", misfit_ds);
    	seq_printf(m, "Highres Tick Ctrl: %u\n", highres_tick_ctrl);
    	seq_printf(m, "Watchdog Enable: %d\n", watchdog_enable);
    
    	// SCX 特定统计信息
    	seq_printf(m, "SCX Exit Type: %d\n", atomic_read(&scx_exit_type));
    	seq_printf(m, "SCX Rejected Tasks: %lld\n", atomic64_read(&scx_nr_rejected));
    
    	// 调度频率信息
    	seq_printf(m, "Sched Ravg Window Frame Per Sec: %d\n", 
               sched_ravg_window_frame_per_sec);
    
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

	for_each_present_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (cpu != policy->cpu)
			continue;
	}
	return count;
}
HMBIRD_PROC_OPS(save_gov, hmbird_common_open, save_gov_str);

static ssize_t cpu_cluster_proc_write(struct file *file, const char __user *buf,
								size_t count, loff_t *ppos)
{
	int *pval = (int *)pde_data(file_inode(file));

	if (set_proc_buf_val(file, buf, count, pval))
		return -EFAULT;

	return count;
}
HMBIRD_PROC_OPS(cpu_cluster_masks, hmbird_common_open, cpu_cluster_proc_write);

/* slim_walt_ctrl ops begin */
static ssize_t slim_walt_ctrl_write(struct file *file, const char __user *buf,
					size_t count, loff_t *ppos)
{
	int *pval = (int *)pde_data(file_inode(file));

	if (set_proc_buf_val(file, buf, count, pval))
		return -EFAULT;

	return count;
}
HMBIRD_PROC_OPS(slim_walt_ctrl, hmbird_common_open,
                        slim_walt_ctrl_write);
/* slim_walt_ctrl ops end */

static int hmbird_proc_init(void)
{
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

	return 0;
}

static int __init hmbird_common_init(void)
{
	return hmbird_proc_init();
}

static void __exit hmbird_common_exit(void)
{
}

module_init(hmbird_common_init);
module_exit(hmbird_common_exit);
MODULE_LICENSE("GPL v2");

