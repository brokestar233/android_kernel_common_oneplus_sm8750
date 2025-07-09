// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/auth_ctl/auth_qos_debug.c
 *
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 *
 */
#include <linux/cred.h>
#include <linux/mutex.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched/auth_ctrl.h>
#include <linux/sched/rtg_auth.h>
#include <linux/sched/qos_ctrl.h>
#include <linux/sched/qos_auth.h>

#include "auth_ctrl.h"
#include "qos_ctrl.h"

#define seq_printf_auth(m, x...) \
do { \
	if (m) \
		seq_printf(m, x); \
	else \
		printk(x); \
} while (0)

static void print_auth_id(struct seq_file *file,
	const int tgid)
{
	seq_printf_auth(file, "AUTH_PID            :%d\n", tgid);
}

static void print_auth_info(struct seq_file *file,
	const struct auth_struct *auth)
{
	seq_printf_auth(file, "AUTH_STATUS        :%d\n", auth->status);
#ifdef CONFIG_RTG_AUTHORITY
	seq_printf_auth(file, "RTG_FLAG           :%04x\n", auth->rtg_auth_flag);
#endif
#ifdef CONFIG_QOS_AUTHORITY
	seq_printf_auth(file, "QOS_FLAG           :%04x\n", auth->qos_auth_flag);
#endif
}

static void print_qos_count(struct seq_file *file,
	const struct auth_struct *auth)
{
	int i;

	for (i = QOS_POLICY_MIN_LEVEL; i < NR_QOS; ++i)
		seq_printf_auth(file, "QOS level %d thread nr  :%d\n", i, auth->num[i]);
}

static void print_qos_thread(struct seq_file *file,
	const struct auth_struct *auth)
{
	struct qos_task_struct *tmp, *next;
	struct task_struct *p;
	int i;

	for (i = QOS_POLICY_MIN_LEVEL; i < NR_QOS; ++i) {
		seq_printf_auth(file, "QOS level %d threads:", i);
		list_for_each_entry_safe(tmp, next, &auth->tasks[i], qos_list) {
			p = container_of(tmp, struct task_struct, qts);
			seq_printf_auth(file, "%d ", p->pid);
		}
		seq_printf_auth(file, "\n");
	}

}

static inline void print_auth_struct(struct seq_file *file, struct auth_struct *auth)
{
	print_auth_info(file, auth);
	seq_printf_auth(file, "\n");
	print_qos_count(file, auth);
	seq_printf_auth(file, "\n");
#ifdef CONFIG_QOS_CTRL
	print_qos_thread(file, auth);
#endif
	seq_printf_auth(file, "---------------------------------------------------------\n");

}

int authority_printf_handler(int id, void *p, void *para)
{
	struct auth_struct *auth = (struct auth_struct *)p;
	struct seq_file *file = (struct seq_file *)para;

	/*
	 * data consistency is not that important here
	 */
	seq_printf_auth(file, "\n\n");
	print_auth_id(file, id);
	seq_printf_auth(file, "\n");

	/* no need to add refcount here, auth must alive in ua_idr_mutex */
	print_auth_struct(file, auth);

	return 0;
}

static int sched_auth_debug_show(struct seq_file *file, void *param)
{
	struct idr *ua_idr = get_auth_ctrl_idr();
	struct mutex *ua_idr_mutex = get_auth_idr_mutex();
	/*
	 * NOTICE:
	 * if mutex in authority_printf_handler, sleep may occur
	 * change ths spin_lock to mutex, or remove mutex in handler
	 */

	mutex_lock(ua_idr_mutex);
	/* will never return 0 here, auth in ua_idr must alive */
	idr_for_each(ua_idr, authority_printf_handler, file);
	mutex_unlock(ua_idr_mutex);

	return 0;
}

static int sched_auth_debug_release(struct inode *inode, struct file *file)
{
	seq_release(inode, file);
	return 0;
}

static int sched_auth_debug_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, sched_auth_debug_show, NULL);
}

static const struct proc_ops sched_auth_debug_fops = {
	.proc_open = sched_auth_debug_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = sched_auth_debug_release,
};

int __init init_sched_auth_debug_procfs(void)
{
	struct proc_dir_entry *pe = NULL;

	pe = proc_create("sched_auth_qos_debug",
		0400, NULL, &sched_auth_debug_fops);
	if (unlikely(!pe))
		return -ENOMEM;
	return 0;
}

