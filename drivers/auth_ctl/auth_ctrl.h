/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drivers/auth_ctl/auth_ctrl.h
 *
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 *
 */

#ifndef __AUTH_CTRL_H
#define __AUTH_CTRL_H

#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/refcount.h>

#include <linux/sched/qos_ctrl.h>

struct auth_struct {
	struct mutex mutex;
	refcount_t usage;
	unsigned int status;
#ifdef CONFIG_RTG_AUTHORITY
	unsigned int rtg_auth_flag;
#endif
#ifdef CONFIG_QOS_AUTHORITY
	unsigned int qos_auth_flag;
#endif
#ifdef CONFIG_QOS_CTRL
	unsigned int num[NR_QOS];
	struct list_head tasks[NR_QOS];
#endif
};

/*
 * for debug fs
 */
struct idr *get_auth_ctrl_idr(void);
struct mutex *get_auth_idr_mutex(void);

#ifdef CONFIG_AUTH_QOS_DEBUG
int __init init_sched_auth_debug_procfs(void);
#else
static inline int init_sched_auth_debug_procfs(void)
{
	return 0;
}
#endif

#endif /* __AUTH_CTRL_H */

