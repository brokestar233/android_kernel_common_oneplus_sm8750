/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __SLIM_H
#define __SLIM_H

extern atomic_t __hmbird_ops_enabled;
extern atomic_t non_hmbird_task;
extern int cgroup_ids_table[NUMS_CGROUP_KINDS];

extern noinline int tracing_mark_write(const char *buf);
int task_top_id(struct task_struct *p);
void stats_print(char *buf, int len);
extern spinlock_t hmbird_tasks_lock;

#endif
