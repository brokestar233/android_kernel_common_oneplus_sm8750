/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __HMBIRD_SCHED_PROC_H__
#define __HMBIRD_SCHED_PROC_H__

#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#define HMBIRD_CREATE_PROC_ENTRY(name, mode, parent, proc_ops) \
	do { \
		if (!proc_create(name, mode, parent, proc_ops)) { \
			pr_err("Error creating proc entry %s\n", name); \
			return -ENOMEM; \
		} \
	} while (0)

#define HMBIRD_CREATE_PROC_ENTRY_DATA(name, mode, parent, proc_ops, data) \
	do { \
		if (!proc_create_data(name, mode, parent, proc_ops, data)) { \
			pr_err("Error creating proc entry with data %s\n", name); \
			return -ENOMEM; \
		} \
	} while (0)

#define HMBIRD_PROC_OPS(name, open_func, write_func) \
	static const struct proc_ops name##_proc_ops = { \
		.proc_open = open_func, \
		.proc_write = write_func, \
		.proc_read = seq_read, \
		.proc_lseek = seq_lseek, \
		.proc_release = single_release, \
	}

static ssize_t hmbird_common_write(struct file *file,
				   const char __user *buf,
				   size_t count, loff_t *ppos);
static int hmbird_common_show(struct seq_file *m, void *v);
static int hmbird_common_open(struct inode *inode, struct file *file);

#endif
