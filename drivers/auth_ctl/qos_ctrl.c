// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/auth_ctl/auth_ctrl.c
 *
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 *
 */
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/stop_machine.h>
#include <linux/sched/auth_ctrl.h>
#include <linux/sched/rtg_auth.h>
#include <linux/sched/qos_ctrl.h>
#include <linux/sched/qos_auth.h>
#include <uapi/linux/sched/types.h>

#include "auth_ctrl.h"
#include "qos_ctrl.h"

typedef long (*qos_ctrl_func)(int abi, void __user *uarg);

static long ctrl_qos_operation(int abi, void __user *uarg);
static long ctrl_qos_policy(int abi, void __user *uarg);

#define QOS_LEVEL_SET_MAX 5

static qos_ctrl_func g_func_array[QOS_CTRL_MAX_NR] = {
	NULL, /* reserved */
	ctrl_qos_operation,
	ctrl_qos_policy,
};

static struct qos_policy_map qos_policy_array[QOS_POLICY_MAX_NR];

void remove_qos_tasks(struct auth_struct *auth)
{
	int i;
	struct qos_task_struct *tmp, *next;
	struct task_struct *p;

	mutex_lock(&auth->mutex);
	for (i = QOS_POLICY_MIN_LEVEL; i < NR_QOS; ++i) {
		list_for_each_entry_safe(tmp, next, &auth->tasks[i], qos_list) {
			struct qos_task_struct **tmp_ptr = &tmp;
			p = container_of(tmp_ptr, struct task_struct, qts);
			if (!list_empty(&tmp->qos_list)) {
				list_del_init(&tmp->qos_list);
				tmp->in_qos = NO_QOS;
				put_task_struct(p);
			}
		}
	}
	mutex_unlock(&auth->mutex);
}

static void init_sched_attr(struct sched_attr *attr)
{
	memset(attr, 0, sizeof(struct sched_attr));
}

static inline bool is_system(unsigned int uid)
{
	return uid == SYSTEM_UID;
}

/* This function must be called when p is valid. That means the p's refcount must exist */
static int sched_set_task_qos_attr(struct task_struct *p, int level, int status)
{
	struct qos_policy_item *item;
	struct qos_policy_map *policy_map;
	struct sched_attr attr;

	read_lock(&qos_policy_array[status].lock);
	if (!qos_policy_array[status].initialized) {
		pr_err("[QOS_CTRL] dirty qos policy, pid=%d, uid=%d, status=%d\n",
		       p->pid, p->cred->uid.val, status);
		read_unlock(&qos_policy_array[status].lock);
		return -DIRTY_QOS_POLICY;
	}

	policy_map = &qos_policy_array[status];
	item = &policy_map->levels[level];

	init_sched_attr(&attr);
	attr.size			= sizeof(struct sched_attr);
	attr.sched_policy		= SCHED_NORMAL;

	if (policy_map->policy_flag & QOS_FLAG_NICE)
		attr.sched_nice = item->nice;


	if ((policy_map->policy_flag & QOS_FLAG_RT) && item->rt_sched_priority) {
		attr.sched_policy = SCHED_FIFO;
		attr.sched_flags |= SCHED_FLAG_RESET_ON_FORK;
		attr.sched_priority = item->rt_sched_priority;
	}

	read_unlock(&qos_policy_array[status].lock);

	if (unlikely(p->flags & PF_EXITING)) {
		pr_info("[QOS_CTRL] dying task, no need to set qos\n");
		return -THREAD_EXITING;
	}

	return sched_setattr_nocheck(p, &attr);
}

/*
 * Switch qos mode when status changed.
 * Lock auth before calling this function
 */
void qos_switch(struct auth_struct *auth, int target_status)
{
	int i;
	int ret;
	struct task_struct *task;
	struct qos_task_struct *qts;

	if (!auth) {
		pr_err("[QOS_CTRL] auth no exist, qos switch failed\n");
		return;
	}

	lockdep_assert_held(&auth->mutex);

	if (auth->status == target_status) {
		pr_info("[QOS_CTRL] same status, no need to switch qos\n");
		return;
	}

	for (i = QOS_POLICY_MIN_LEVEL; i < NR_QOS; ++i) {
		list_for_each_entry(qts, &auth->tasks[i], qos_list) {
			struct qos_task_struct **qts_ptr = &qts;
			task = container_of(qts_ptr, struct task_struct, qts);
			ret = sched_set_task_qos_attr(task, i, target_status);
			if (ret)
				pr_err("[QOS_CTRL] set qos attr failed, qos switch failed\n");
		}
	}
}

static int qos_insert_task(struct task_struct *p, struct list_head *head, unsigned int level)
{
	struct qos_task_struct *qts = p->qts;

	if (qts->in_qos > NO_QOS) {
		pr_err("[QOS_CTRL] qos apply still active, no duplicate add\n");
		return -PID_DUPLICATE;
	}

	if (likely(list_empty(&qts->qos_list))) {
		get_task_struct(p);
		list_add(&qts->qos_list, head);
		qts->in_qos = level;
	}

	return 0;
}

static int qos_remove_task(struct task_struct *p)
{
	struct qos_task_struct *qts = (struct qos_task_struct *) &p->qts;

	if (qts->in_qos == NO_QOS) {
		pr_err("[QOS_CTRL] task not in qos, no need to remove\n");
		return -PID_NOT_EXIST;
	}

	if (likely(!list_empty(&qts->qos_list))) {
		list_del_init(&qts->qos_list);
		qts->in_qos = NO_QOS;
		put_task_struct(p);
	}

	return 0;
}

static inline bool super_user(struct task_struct *p)
{
	return super_uid(task_uid(p).val);
}

/*
 * judge permission for changing tasks' qos
 */
static bool can_change_qos(struct task_struct *p, unsigned int qos_level)
{
	struct auth_struct *auth;
	auth = get_authority(p);
	/* just system & root user can set(be setted) high qos level */
	if (!auth || (auth && !super_user(p) && qos_level > QOS_LEVEL_SET_MAX)) {
		pr_err("[QOS_CTRL] %d have no permission to change qos\n", p->pid);
		return false;
	}

	return true;
}

int qos_apply(struct qos_ctrl_data *data)
{
	unsigned int level = data->level;
	struct auth_struct *auth;
	struct task_struct *p;
	struct qos_task_struct *qts;
	int pid = data->pid;
	int ret;

	if (level >= NR_QOS || level == NO_QOS) {
		pr_err("[QOS_CTRL] no this qos level, qos apply failed\n");
		ret = -ARG_INVALID;
		goto out;
	}

	p = find_get_task_by_vpid((pid_t)pid);
	if (unlikely(!p)) {
		pr_err("[QOS_CTRL] no matching task for this pid, qos apply failed\n");
		ret = -ESRCH;
		goto out;
	}

	if (unlikely(p->flags & PF_EXITING)) {
		pr_info("[QOS_CTRL] dying task, no need to set qos\n");
		ret = -THREAD_EXITING;
		goto out_put_task;
	}

	if (!can_change_qos(current, level)) {
		pr_err("[QOS_CTRL] QOS apply not permit\n");
		ret = -ARG_INVALID;
		goto out_put_task;
	}

	auth = get_authority(p);
	if (!auth) {
		pr_err("[QOS_CTRL] no auth data for pid=%d(%s), qos apply failed\n",
		       p->tgid, p->comm);
		ret = -PID_NOT_FOUND;
		goto out_put_task;
	}

	mutex_lock(&auth->mutex);
	if (auth->status == AUTH_STATUS_DEAD) {
		pr_err("[QOS_CTRL] this auth data has been deleted\n");
		ret = -INVALID_AUTH;
		goto out_unlock;
	}

	if (auth->num[level] >= QOS_NUM_MAX) {
		pr_err("[QOS_CTRL] qos num exceeds limit, cached only\n");
		ret = -QOS_THREAD_NUM_EXCEED_LIMIT;
		goto out_unlock;
	}

	qts = (struct qos_task_struct *) &p->qts;

	if (rt_task(p) && qts->in_qos == NO_QOS) {
		pr_err("[QOS_CTRL] can not apply qos for native rt task\n");
		ret = -ALREADY_RT_TASK;
		goto out_unlock;
	}

	/* effective qos must in range [NO_QOS, NR_QOS) */
	if (qts->in_qos != NO_QOS) {
		if (qts->in_qos == level) {
			ret = 0;
			goto out_unlock;
		}

		--auth->num[qts->in_qos];
		qos_remove_task(p);
	}

	ret = qos_insert_task(p, &auth->tasks[level], level);
	if (ret < 0) {
		pr_err("[QOS_CTRL] insert task to qos list %d failed\n", level);
		goto out_unlock;
	}

	++auth->num[level];

	ret = sched_set_task_qos_attr(p, level, auth->status);
	if (ret) {
		pr_err("[QOS_CTRL] set qos_level %d for thread %d on status %d failed\n",
		       level, p->pid, auth->status);
		--auth->num[level];
		qos_remove_task(p);
	}

out_unlock:
	mutex_unlock(&auth->mutex);
	put_auth_struct(auth);
out_put_task:
	put_task_struct(p);
out:
	return ret;
}

int qos_leave(struct qos_ctrl_data *data)
{
	unsigned int level;
	struct auth_struct *auth;
	struct task_struct *p;
	struct qos_task_struct *qts;
	int pid = data->pid;
	int ret;

	p = find_get_task_by_vpid((pid_t)pid);
	if (!p) {
		pr_err("[QOS_CTRL] no matching task for this pid, qos apply failed\n");
		ret = -ESRCH;
		goto out;
	}

	if (unlikely(p->flags & PF_EXITING)) {
		pr_info("[QOS_CTRL] dying task, no need to set qos\n");
		ret = -THREAD_EXITING;
		goto out_put_task;
	}

	auth = get_authority(p);
	if (!auth) {
		pr_err("[QOS_CTRL] no auth data for pid=%d(%s), qos stop failed\n",
		       p->tgid, p->comm);
		ret = -PID_NOT_FOUND;
		goto out_put_task;
	}

	mutex_lock(&auth->mutex);

	qts = (struct qos_task_struct *) &p->qts;

	level = qts->in_qos;
	if (level == NO_QOS) {
		pr_err("[QOS_CTRL] task not in qos list, qos stop failed\n");
		ret = -ARG_INVALID;
		goto out_unlock;
	}

	if (!can_change_qos(current, 0)) {
		pr_err("[QOS_CTRL] apply for others not permit\n");
		ret = -ARG_INVALID;
		goto out_unlock;
	}

	if (auth->status == AUTH_STATUS_DEAD) {
		pr_err("[QOS_CTRL] this auth data has been deleted\n");
		ret = -INVALID_AUTH;
		goto out_unlock;
	}

	ret = qos_remove_task(p);
	if (ret < 0) {
		pr_err("[QOS_CTRL] remove task from qos list %d failed\n", level);
		goto out_unlock;
	}

	--auth->num[level];

	/*
	 * NO NEED to judge whether current status is AUTH_STATUS_DISABLE.
	 * In the auth destoring context, the removing of thread's sched attr was protected by
	 * auth->mutex, AUTH_STATUS_DISABLED will never appear here.
	 *
	 * The second param 3 means nothing, actually you can use any valid level here, cause the
	 * policy matching AUTH_STATUS_DISABLED has default parameters for all qos level, which can
	 * keep a powerful thread to behave like a ordinary thread.
	 */
	ret = sched_set_task_qos_attr(p, 3, AUTH_STATUS_DISABLED);
	if (ret)
		pr_err("[QOS_CTRL] set qos_level %d for thread %d on status %d to default failed\n",
		       level, p->pid, auth->status);

out_unlock:
	mutex_unlock(&auth->mutex);
	put_auth_struct(auth);
out_put_task:
	put_task_struct(p);
out:
	return ret;
}

int qos_get(struct qos_ctrl_data *data)
{
	struct task_struct *p;
	struct qos_task_struct *qts;
	int pid = data->pid;
	int ret = 0;

	p = find_get_task_by_vpid((pid_t)pid);
	if (unlikely(!p)) {
		pr_err("[QOS_CTRL] no matching task for this pid, qos get failed\n");
		ret = -ESRCH;
		goto out;
	}

	if (unlikely(p->flags & PF_EXITING)) {
		pr_info("[QOS_CTRL] dying task, no need to set qos\n");
		ret = -THREAD_EXITING;
		goto out_put_task;
	}

	qts = (struct qos_task_struct *) &p->qts;
	data->qos = qts->in_qos;

out_put_task:
	put_task_struct(p);
out:
	return ret;
}

void init_task_qos(struct task_struct *p)
{
    p->qts = kzalloc(sizeof(struct qos_task_struct), GFP_KERNEL);
    if (p->qts) {
        INIT_LIST_HEAD(&p->qts->qos_list);
        p->qts->in_qos = NO_QOS;
    }
}

/*
 * Remove statistic info in auth when task exit
 */
void sched_exit_qos_list(struct task_struct *p)
{
	struct auth_struct *auth;
	struct qos_task_struct *qts = (struct qos_task_struct *) &p->qts;

	/*
	 * For common tasks(the vast majority):
	 * skip get authority, fast return here.
	 *
	 * For qos tasks:
	 * If contend with auth_delete() happens,
	 * 1. function return here, auth_delete() will do the clean up
	 * 2. function go on, either no auth return, either do clean up here
	 * Both cases guarantee data synchronization
	 */
	if (likely(qts->in_qos == NO_QOS))
		return;

	auth = get_authority(p);
	if (!auth)
		goto out;

	mutex_lock(&auth->mutex);
	if (qts->in_qos == NO_QOS) {
		mutex_unlock(&auth->mutex);
		goto out_put_auth;
	}
	if (qts && qts->in_qos != NO_QOS) {
    	--auth->num[qts->in_qos];
    	list_del_init(&qts->qos_list);
    	qts->in_qos = NO_QOS;
    	put_task_struct(p);
	}
	mutex_unlock(&auth->mutex);
	kfree(p->qts); // 释放 qts 内存
	p->qts = NULL;

out_put_auth:
	put_auth_struct(auth);
out:
	return;
}

typedef int (*qos_manipulate_func)(struct qos_ctrl_data *data);

static qos_manipulate_func qos_func_array[QOS_OPERATION_CMD_MAX_NR] = {
	NULL,
	qos_apply,  //1
	qos_leave,
	qos_get,
};

static long do_qos_manipulate(struct qos_ctrl_data *data)
{
	long ret = 0;
	unsigned int type = data->type;

	if (type <= 0 || type >= QOS_OPERATION_CMD_MAX_NR) {
		pr_err("[QOS_CTRL] CMD_ID_QOS_MANIPULATE type not valid\n");
		return -ARG_INVALID;
	}

	if (qos_func_array[type])
		ret = (long)(*qos_func_array[type])(data);

	return ret;
}

static long ctrl_qos_operation(int abi, void __user *uarg)
{
	struct qos_ctrl_data qos_data;
	int ret = -1;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-to-int-cast"

	switch (abi) {
	case QOS_IOCTL_ABI_ARM32:
		ret = copy_from_user(&qos_data,
				(void __user *)compat_ptr((compat_uptr_t)uarg),
				sizeof(struct qos_ctrl_data));
		break;
	case QOS_IOCTL_ABI_AARCH64:
		ret = copy_from_user(&qos_data, uarg, sizeof(struct qos_ctrl_data));
		break;
	default:
		pr_err("[QOS_CTRL] abi format error\n");
		break;
	}

#pragma GCC diagnostic pop

	if (ret) {
		pr_err("[QOS_CTRL] %s copy user data failed\n", __func__);
		return ret;
	}

	ret = do_qos_manipulate(&qos_data);
	if (ret < 0) {
		pr_err("[QOS_CTRL] CMD_ID_QOS_MANIPULATE failed\n");
		return ret;
	}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-to-int-cast"

	switch (abi) {
	case QOS_IOCTL_ABI_ARM32:
		ret = copy_to_user((void __user *)compat_ptr((compat_uptr_t)uarg),
				&qos_data, sizeof(struct qos_ctrl_data));
		break;
	case QOS_IOCTL_ABI_AARCH64:
		ret = copy_to_user(uarg, &qos_data, sizeof(struct qos_ctrl_data));
		break;
	default:
		pr_err("[QOS_CTRL] abi format error\n");
		break;
	}

#pragma GCC diagnostic pop

	if (ret) {
		pr_err("[QOS_CTRL] %s copy to user failed\n", __func__);
		return ret;
	}
	return 0;
}

#define MAX_LATENCY_NICE	19
#define MIN_LATENCY_NICE	-20

static inline bool valid_nice(int nice)
{
	return nice >= MIN_NICE && nice <= MAX_NICE;
}

static inline bool valid_latency_nice(int latency_nice)
{
	return latency_nice >= MIN_LATENCY_NICE && latency_nice <= MAX_LATENCY_NICE;
}

static inline bool valid_uclamp(int uclamp_min, int uclamp_max)
{
	if (uclamp_min > uclamp_max)
		return false;
	if (uclamp_max > SCHED_CAPACITY_SCALE)
		return false;

	return true;
}

static inline bool valid_rt(int sched_priority)
{
	if (sched_priority > MAX_USER_RT_PRIO - 1 || sched_priority < 0)
		return false;

	return true;
}

static bool valid_qos_flag(unsigned int qos_flag)
{
	if (qos_flag & ~QOS_FLAG_ALL)
		return false;

	return true;
}

static inline bool valid_qos_item(struct qos_policy_datas *datas)
{
	int i;
	int type = datas->policy_type;
	struct qos_policy_data *data;

	if (type <= 0 || type >= QOS_POLICY_MAX_NR) {
		pr_err("[QOS_CTRL] not valid qos policy type, policy change failed\n");
		goto out_failed;
	}

	if (!valid_qos_flag(datas->policy_flag)) {
		pr_err("[QOS_CTRL] not valid qos flag, policy change failed\n");
		goto out_failed;
	}

	/* check user space qos polcicy data, level 0 reserved */
	for (i = 0; i < NR_QOS; ++i) {
		data = &datas->policys[i];

		if (!valid_nice(data->nice)) {
			pr_err("[QOS_CTRL] invalid nice, policy change failed\n");
			goto out_failed;
		}

		if (!valid_latency_nice(data->latency_nice)) {
			pr_err("[QOS_CTRL] invalid latency_nice, policy change failed\n");
			goto out_failed;
		}

		if (!valid_uclamp(data->uclamp_min, data->uclamp_max)) {
			pr_err("[QOS_CTRL] invalid uclamp, policy change failed\n");
			goto out_failed;
		}

		if (!valid_rt(data->rt_sched_priority)) {
			pr_err("[QOS_CTRL] invalid rt, policy change failed\n");
			goto out_failed;
		}
	}

	return true;

out_failed:
	pr_err("[QOS_CTRL] not valid qos policy params\n");
	return false;
}

static long do_qos_policy_change(struct qos_policy_datas *datas)
{
	long ret = 0;
	int i;
	struct qos_policy_item *item;
	struct qos_policy_data *data;
	int type = datas->policy_type;

	if (type >= QOS_POLICY_MAX_NR) {
		pr_err("[QOS_CTRL] not valid policy type\n");
		goto out_failed;
	}

	if (!valid_qos_item(datas))
		goto out_failed;

	write_lock(&qos_policy_array[type].lock);
	for (i = QOS_POLICY_MIN_LEVEL; i < NR_QOS; ++i) {
		item = &qos_policy_array[type].levels[i];

		/* user space policy params */
		data = &datas->policys[i];

		item->nice = data->nice;
		item->latency_nice = data->latency_nice;
		item->uclamp_min = data->uclamp_min;
		item->uclamp_max = data->uclamp_max;
		/* only specific qos level could use SCHED_FIFO */
		item->rt_sched_priority = (i < MIN_RT_QOS_LEVEL) ? 0 :
					  data->rt_sched_priority;
	}
	qos_policy_array[type].policy_flag = datas->policy_flag;
	qos_policy_array[type].initialized = true;
	write_unlock(&qos_policy_array[type].lock);

	return ret;

out_failed:
	return -ARG_INVALID;
}

static long ctrl_qos_policy(int abi, void __user *uarg)
{
	struct qos_policy_datas policy_datas;
	long ret = -1;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-to-int-cast"

	switch (abi) {
	case QOS_IOCTL_ABI_ARM32:
		ret = copy_from_user(&policy_datas,
				(void __user *)compat_ptr((compat_uptr_t)uarg),
				sizeof(struct qos_policy_datas));
		break;
	case QOS_IOCTL_ABI_AARCH64:
		ret = copy_from_user(&policy_datas, uarg, sizeof(struct qos_policy_datas));
		break;
	default:
		pr_err("[QOS_CTRL] abi format error\n");
		break;
	}

#pragma GCC diagnostic pop

	if (ret) {
		pr_err("[QOS_RTG] %s copy user data failed\n", __func__);
		return ret;
	}

	return do_qos_policy_change(&policy_datas);
}

long do_qos_ctrl_ioctl(int abi, struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *uarg = (void __user *)arg;
	unsigned int func_cmd = _IOC_NR(cmd);

	if (uarg == NULL) {
		pr_err("%s: invalid user uarg\n", __func__);
		return -EINVAL;
	}

	if (_IOC_TYPE(cmd) != QOS_CTRL_IPC_MAGIG) {
		pr_err("%s: qos ctrl magic fail, TYPE=%d\n",
		       __func__, _IOC_TYPE(cmd));
		return -EINVAL;
	}

	if (func_cmd >= QOS_CTRL_MAX_NR) {
		pr_err("%s: qos ctrl cmd error, cmd:%d\n",
		       __func__, _IOC_TYPE(cmd));
		return -EINVAL;
	}

#ifdef CONFIG_QOS_AUTHORITY
	if (!check_authorized(func_cmd, QOS_AUTH_FLAG)) {
		pr_err("[QOS_CTRL] %s: pid not authorized\n", __func__);
		return -PID_NOT_AUTHORIZED;
	}
#endif

	if (g_func_array[func_cmd])
		return (*g_func_array[func_cmd])(abi, uarg);

	return -EINVAL;
}

static void init_qos_policy_array(void)
{
	int i;

	/* index 0 reserved */
	for (i = 1; i < QOS_POLICY_MAX_NR; ++i)
		rwlock_init(&qos_policy_array[i].lock);

	pr_info("[QOS_CTRL] lock in qos policy initialized\n");
}

int __init init_qos_ctrl(void)
{
	init_qos_policy_array();

	return 0;
}

