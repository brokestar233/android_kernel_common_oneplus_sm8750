/* SPDX-License-Identifier: GPL-2.0 */
/*
 * include/linux/sched/auth_ctrl.h
 *
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 */

#ifndef _AUTH_CTRL_H
#define _AUTH_CTRL_H

#include <linux/fs.h>

#define ROOT_UID   0
#define SYSTEM_UID 1000

#define SUPER_UID SYSTEM_UID
#define RESOURCE_SCHEDULE_SERVICE_UID 1096
#define super_uid(uid) (uid == ROOT_UID || uid == SYSTEM_UID || uid == RESOURCE_SCHEDULE_SERVICE_UID)

enum ioctl_abi_format_auth{
	AUTH_IOCTL_ABI_ARM32,
	AUTH_IOCTL_ABI_AARCH64,
};

enum auth_ctrl_cmdid {
	BASIC_AUTH_CTRL = 1,
	AUTH_CTRL_MAX_NR
};

#define AUTH_CTRL_IPC_MAGIG	0xCD

#define	BASIC_AUTH_CTRL_OPERATION \
	_IOWR(AUTH_CTRL_IPC_MAGIG, BASIC_AUTH_CTRL, struct auth_ctrl_data)

enum auth_flag_type {
#ifdef CONFIG_RTG_AUTHORITY
	RTG_AUTH_FLAG,
#endif
#ifdef CONFIG_QOS_AUTHORITY
	QOS_AUTH_FLAG,
#endif
};

#define INVALIED_AUTH_FLAG	0x00000000

struct auth_ctrl_data {
	unsigned int pid;

	/*
	 * type:  operation type, see auth_manipulate_type, valid range [1, AUTH_MAX_NR)
	 *
	 * rtg_ua_flag: authority flag for RTG, see AF_RTG_ALL
	 *
	 * qos_ua_flag: authority flag for QOS, see AF_QOS_ALL
	 *
	 * status: current status for uid, use to match qos policy, see auth_status and
	 * qos_policy_type, valid range [1, AUTH_STATUS_MAX_NR - 1)
	 *
	 */
	unsigned int type;
	unsigned int rtg_ua_flag;
	unsigned int qos_ua_flag;
	unsigned int status;
};

enum auth_err_no {
	ARG_INVALID = 1,
	THREAD_EXITING,
	DIRTY_QOS_POLICY,
	PID_NOT_AUTHORIZED,
	PID_NOT_FOUND,
	PID_DUPLICATE,
	PID_NOT_EXIST,
	INVALID_AUTH,
	ALREADY_RT_TASK,
	QOS_THREAD_NUM_EXCEED_LIMIT,
};

enum auth_manipulate_type {
	AUTH_ENABLE = 1,
	AUTH_DELETE,
	AUTH_GET,
	AUTH_SWITCH,
	AUTH_MAX_NR,
};

#ifndef CONFIG_QOS_POLICY_MAX_NR
#define QOS_STATUS_COUNT 5
#else
#define QOS_STATUS_COUNT CONFIG_QOS_POLICY_MAX_NR
#endif

/* keep match with qos_policy_type */
enum auth_status {
	/* reserved fo QOS_POLICY_DEFAULT, no qos supply in this status */
	AUTH_STATUS_DISABLED = 1,

	/* reserved for ROOT and SYSTEM */
	AUTH_STATUS_SYSTEM_SERVER = 2,

	/*
	 * these space for user specific status
	 * range (AUTH_STATUS_SYSTEM_SERVER, AUTH_STATUS_DEAD)
	 *
	 * initial the policy in matching index of qos_policy_array first before use
	 * see ctrl_qos_policy
	 */

	/* reserved for destorying auth_struct*/
	AUTH_STATUS_DEAD = QOS_STATUS_COUNT,

	AUTH_STATUS_MAX_NR = QOS_STATUS_COUNT + 1,
};

struct auth_struct;
long auth_ctrl_ioctl(int abi, struct file *file, unsigned int cmd, unsigned long arg);
void get_auth_struct(struct auth_struct *auth);
void put_auth_struct(struct auth_struct *auth);
struct auth_struct *get_authority(struct task_struct *p);
bool check_authorized(unsigned int func_id, unsigned int type);

#endif /* _AUTH_CTRL_H */

