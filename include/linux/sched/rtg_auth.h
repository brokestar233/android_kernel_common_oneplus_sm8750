/* SPDX-License-Identifier: GPL-2.0 */
/*
 * include/linux/sched/rtg_auth.h
 *
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 */

#ifndef _RTG_AUTH_H
#define _RTG_AUTH_H

#include <linux/sched.h>
#include <linux/sched/auth_ctrl.h>

/*
 * RTG authority flags for SYSTEM or ROOT
 *
 * keep sync with rtg_sched_cmdid
 * when add a new cmd to rtg_sched_cmdid
 * keep new_flag = (old_flag << 1) + 1
 * up to now, next flag value is 0x3fff
 */
#define AF_RTG_ALL		0x1fff

/*
 * delegated authority for normal uid
 * trim access range for RTG
 */
#define AF_RTG_DELEGATED	0x1fff

bool check_authorized(unsigned int func_id, unsigned int type);

#endif /* _RTG_AUTH_H */

