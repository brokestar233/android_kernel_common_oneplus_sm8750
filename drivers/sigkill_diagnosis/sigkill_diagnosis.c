// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2022-2030 Oplus. All rights reserved.
 * Description : assist driver for sigkill diagnosis, help userspace athena to
 * record kill reason category for sigkill
 * Version : 1.0
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": %s: %d: " fmt, __func__, __LINE__

#include <linux/types.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/cred.h>
#include <linux/spinlock.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <trace/hooks/signal.h>

#define SIGKILL_RECORD_SIZE 20
#define SYSTEM_UID 1000
#define APP_UID 10000
#define SIGKILL_REASON_BUF_LEN 1024
#define BUF_RESERVED_LEN 30

struct sigkill_record {
	u32 caller_pid;
	u32 caller_uid;
	u32 killer_pid;
	u32 killer_uid;
};

struct sigkill_records {
	struct sigkill_record array[SIGKILL_RECORD_SIZE]; /* use as ringbuffer */
	int index; /* write index */
	int count;
	spinlock_t lock;
};

static struct sigkill_records *g_sigkill_records;
static struct proc_dir_entry *g_sigkill_reason_entry;

static ssize_t sigkill_reason_read(struct file *file, char __user *buf,
	size_t count, loff_t *ppos)
{
	char kbuf[SIGKILL_REASON_BUF_LEN] = {0};
	int len = 0;
	int i, j;
	int start_index; /* read index */
	unsigned long flags;

	spin_lock_irqsave(&g_sigkill_records->lock, flags);
	start_index = g_sigkill_records->index - 1;
	for (i = 0; i < g_sigkill_records->count; i++) {
		if (len >= SIGKILL_REASON_BUF_LEN - BUF_RESERVED_LEN) {
			pr_err("kbuf overflow\n");
			break;
		}
		/* return with reverse order */
		j = start_index - i < 0 ? start_index - i + SIGKILL_RECORD_SIZE : start_index - i;
		len += snprintf(kbuf + len, SIGKILL_REASON_BUF_LEN - 1 - len, "%u,%u,%u,%u\n",
			g_sigkill_records->array[j].killer_pid, g_sigkill_records->array[j].killer_uid,
			g_sigkill_records->array[j].caller_pid, g_sigkill_records->array[j].caller_uid);
	}
	spin_unlock_irqrestore(&g_sigkill_records->lock, flags);

	return simple_read_from_buffer(buf, count, ppos, kbuf, len);
}

static struct proc_ops proc_sigkill_reason_ops = {
	.proc_read = sigkill_reason_read,
	.proc_lseek = default_llseek,
};

/*
 * self kill filter algorithm:
 * 1) for uid >= 10000, killerUid == callerUid or KillerPid == CallerPid
 * 2) for 1000 <= uid < 10000, killerPid == callerPid
 */
static void record_sigkill_reason(void *unused, int sig, struct task_struct *curr, struct task_struct *p)
{
	const struct cred *curr_cred;
	const struct cred *p_cred;
	uid_t caller_uid;
	uid_t killer_uid;
	u32 caller_pid;
	u32 killer_pid;
	int index;
	unsigned long flags;

	if (sig != SIGKILL)
		return;

	if (curr == NULL || p == NULL)
		return;

	rcu_read_lock();
	curr_cred = __task_cred(curr);
	p_cred = __task_cred(p);
	if (curr_cred == NULL || p_cred == NULL) {
		rcu_read_unlock();
		return;
	}

	caller_uid = __kuid_val(curr_cred->uid);
	killer_uid = __kuid_val(p_cred->uid);
	rcu_read_unlock();

	caller_pid = curr->tgid;
	killer_pid = p->tgid;

	if (killer_uid < SYSTEM_UID)
		return;

	if (killer_uid < APP_UID && killer_pid != caller_pid)
		return;

	if (killer_uid >= APP_UID && (killer_uid != caller_uid && killer_pid != caller_pid))
		return;

	/* add to sigkill record, use spinlock to protect exclusive */
	spin_lock_irqsave(&g_sigkill_records->lock, flags);

	index = g_sigkill_records->index++;
	g_sigkill_records->array[index].caller_pid = caller_pid;
	g_sigkill_records->array[index].caller_uid = caller_uid;
	g_sigkill_records->array[index].killer_pid = killer_pid;
	g_sigkill_records->array[index].killer_uid = killer_uid;

	if (g_sigkill_records->index >= SIGKILL_RECORD_SIZE)
		g_sigkill_records->index = 0;

	if (g_sigkill_records->count < SIGKILL_RECORD_SIZE)
		g_sigkill_records->count++;

	spin_unlock_irqrestore(&g_sigkill_records->lock, flags);
}

static int __init sigkill_diagnosis_init(void)
{
	struct proc_dir_entry *root_dir_entry;

	g_sigkill_records = kzalloc(sizeof(*g_sigkill_records), GFP_KERNEL);
	if (g_sigkill_records == NULL) {
		pr_err("kzalloc failed, no memory!\n");
		return -ENOMEM;
	}
	spin_lock_init(&g_sigkill_records->lock);

	root_dir_entry = proc_mkdir("oplus_mem", NULL);
	g_sigkill_reason_entry = proc_create((root_dir_entry ?
		"sigkill_reason" : "oplus_mem/sigkill_reason"),
		0444, root_dir_entry, &proc_sigkill_reason_ops);
	if (g_sigkill_reason_entry == NULL) {
		pr_err("proc_create failed, no memory!\n");
		kfree(g_sigkill_records);
		g_sigkill_records = NULL;
		return -ENOMEM;
	}

	register_trace_android_vh_do_send_sig_info(record_sigkill_reason, NULL);
	return 0;
}

static void __exit sigkill_diagnosis_exit(void)
{
	unregister_trace_android_vh_do_send_sig_info(record_sigkill_reason, NULL);

	proc_remove(g_sigkill_reason_entry);
	g_sigkill_reason_entry = NULL;

	kfree(g_sigkill_records);
	g_sigkill_records = NULL;
}

module_init(sigkill_diagnosis_init);
module_exit(sigkill_diagnosis_exit);

MODULE_DESCRIPTION("oplus_bsp_sigkill_diagnosis");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL v2");
