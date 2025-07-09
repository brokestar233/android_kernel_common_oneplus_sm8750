/* SPDX-License-Identifier: GPL-2.0 */
/*
 * include/linux/sched/qos_ctrl.h
 *
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 */

#ifndef _QOS_CTRL_H
#define _QOS_CTRL_H

#include <linux/sched.h>
#include <linux/fs.h>

enum ioctl_abi_format_qos{
	QOS_IOCTL_ABI_ARM32,
	QOS_IOCTL_ABI_AARCH64,
};

enum qos_ctrl_cmdid {
	QOS_CTRL = 1,
	QOS_POLICY,
	QOS_CTRL_MAX_NR
};

#define QOS_CTRL_IPC_MAGIG	0xCC

#define QOS_CTRL_BASIC_OPERATION \
	_IOWR(QOS_CTRL_IPC_MAGIG, QOS_CTRL, struct qos_ctrl_data)
#define QOS_CTRL_POLICY_OPERATION \
	_IOWR(QOS_CTRL_IPC_MAGIG, QOS_POLICY, struct qos_policy_datas)

#define NO_QOS -1
#define NR_QOS 7
#define NR_RT_QOS 2
#define MIN_RT_QOS_LEVEL (NR_QOS - NR_RT_QOS)

#define QOS_NUM_MAX 2000

enum qos_manipulate_type {
	QOS_APPLY = 1,
	QOS_LEAVE,
	QOS_GET,
	QOS_OPERATION_CMD_MAX_NR,
};

#ifndef CONFIG_QOS_POLICY_MAX_NR
#define QOS_POLICYS_COUNT 5
#else
#define QOS_POLICYS_COUNT CONFIG_QOS_POLICY_MAX_NR
#endif

/*
 * keep match with auth_status
 *
 * range (QOS_POLICY_SYSTEM, QOS_POLICY_MAX_NR) could defined by user
 * use ctrl_qos_policy
 */
enum qos_policy_type {
	QOS_POLICY_DEFAULT = 1,    /* reserved for "NO QOS" */
	QOS_POLICY_SYSTEM  = 2,    /* reserved for ROOT and SYSTEM */
	QOS_POLICY_MAX_NR = QOS_POLICYS_COUNT,
};

struct qos_ctrl_data {
	int pid;

	/*
	 * type:  operation type, see qos_manipulate_type
	 * level: valid from 1 to NR_QOS. Larger value, more aggressive supply
	 */
	unsigned int type;

	/*
	 * user space level, range from [1, NR_QOS]
	 *
	 * NOTICE!!!:
	 * minus 1 before use in kernel, so the kernel range is [0, NR_QOS)
	 */
	unsigned int level;

	int qos;
};

struct qos_policy_data {
	int nice;
	int latency_nice;
	int uclamp_min;
	int uclamp_max;
	int rt_sched_priority;
	int policy;
};

#define QOS_FLAG_NICE			0x01
#define QOS_FLAG_LATENCY_NICE		0x02
#define QOS_FLAG_UCLAMP			0x04
#define QOS_FLAG_RT			0x08

#define QOS_FLAG_ALL	(QOS_FLAG_NICE			| \
			 QOS_FLAG_LATENCY_NICE		| \
			 QOS_FLAG_UCLAMP		| \
			 QOS_FLAG_RT)

struct qos_policy_datas {
	/*
	 * policy_type: id for qos policy, valid from [1, QOS_POLICY_MAX_NR)
	 * policy_flag: control valid sched attr for policy, QOS_FLAG_ALL for whole access
	 * policys:     sched params for specific level qos, minus 1 for matching struct in kerenl
	 */
	int policy_type;
	unsigned int policy_flag;
	struct qos_policy_data policys[NR_QOS];
};

struct auth_struct;

int qos_apply(struct qos_ctrl_data *data);
int qos_leave(struct qos_ctrl_data *data);
int qos_get(struct qos_ctrl_data *data);

void qos_switch(struct auth_struct *auth, int target_status);

void init_task_qos(struct task_struct *p);
void sched_exit_qos_list(struct task_struct *p);
void remove_qos_tasks(struct auth_struct *auth);

long do_qos_ctrl_ioctl(int abi, struct file *file, unsigned int cmd, unsigned long arg);

#endif /* _QOS_CTRL_H */

