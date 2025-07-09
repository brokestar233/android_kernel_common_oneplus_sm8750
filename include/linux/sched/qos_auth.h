/* SPDX-License-Identifier: GPL-2.0 */
/*
 * include/linux/sched/qos_auth.h
 *
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 */

#ifndef _QOS_AUTH_H
#define _QOS_AUTH_H

#include <linux/sched.h>
#include <linux/sched/auth_ctrl.h>

/*
 * QOS authority flags for SYSTEM or ROOT
 *
 * keep sync with qos_ctrl_cmdid
 * when add a new cmd to qos_ctrl_cmdid
 * keep new_flag = (old_flag << 1) + 1
 * up to now, next flag value is 0x0007
 */
#define AF_QOS_ALL		0x0003

/*
 * delegated authority for normal uid
 * trim access range for QOS
 */
#define AF_QOS_DELEGATED	0x0001

bool check_authorized(unsigned int func_id, unsigned int type);

#endif /* _QOS_AUTH_H */

