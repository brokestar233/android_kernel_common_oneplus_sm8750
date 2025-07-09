/* SPDX-License-Identifier: GPL-2.0 */
/*
 * drivers/auth_ctl/qos_ctrl.h
 *
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 *
 */

#ifndef __QOS_CTRL_H
#define __QOS_CTRL_H

#include "../../kernel/sched/sched.h"

#include <linux/sched/qos_ctrl.h>

/* min qos level used in kernel space, begin index for LOOP */
#define QOS_POLICY_MIN_LEVEL 0

#ifndef MAX_USER_RT_PRIO
#define MAX_USER_RT_PRIO 100
#endif

struct qos_policy_item {
	int nice;
	int latency_nice;
	int uclamp_min;
	int uclamp_max;
	int rt_sched_priority;
	int policy;
};

struct qos_policy_map {
	rwlock_t lock;
	bool initialized;
	unsigned int policy_flag;
	struct qos_policy_item levels[NR_QOS];
};

int __init init_qos_ctrl(void);

#endif /* __OQS_CTRL_H */

