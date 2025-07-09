// SPDX-License-Identifier: GPL-2.0
/*
 * drivers/auth_ctl/auth_ctrl.c
 *
 * Copyright (c) 2022 Huawei Device Co., Ltd.
 *
 */

#include <linux/cred.h>
#include <linux/mutex.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sched/auth_ctrl.h>
#include <linux/sched/rtg_auth.h>
#include <linux/sched/qos_ctrl.h>
#include <linux/sched/qos_auth.h>

#include "auth_ctrl.h"
#ifdef CONFIG_QOS_CTRL
#include "qos_ctrl.h"
#endif

typedef long (*auth_ctrl_func)(int abi, void __user *arg);

static long ctrl_auth_basic_operation(int abi, void __user *uarg);

static auth_ctrl_func g_func_array[AUTH_CTRL_MAX_NR] = {
	NULL, /* reserved */
	ctrl_auth_basic_operation,
};

/*
 * uid-based authority idr table
 */
static struct idr *ua_idr;

struct idr *get_auth_ctrl_idr(void)
{
	return ua_idr;
}

static DEFINE_MUTEX(ua_idr_mutex);

struct mutex *get_auth_idr_mutex(void)
{
	return &ua_idr_mutex;
}

/*
 * change auth's status to SYSTEM and enable all feature access
 */
static void change_to_super(struct auth_struct *auth)
{
#ifdef CONFIG_RTG_AUTHORITY
	auth->rtg_auth_flag = AF_RTG_ALL;
#endif
#ifdef CONFIG_QOS_AUTHORITY
	auth->qos_auth_flag = AF_QOS_ALL;
#endif
	auth->status = AUTH_STATUS_SYSTEM_SERVER;
}

static void init_authority_record(struct auth_struct *auth)
{
#ifdef CONFIG_QOS_AUTHORITY
	int i;
#endif

#ifdef CONFIG_RTG_AUTHORITY
	auth->rtg_auth_flag = 0;
#endif
#ifdef CONFIG_QOS_AUTHORITY
	auth->qos_auth_flag = 0;
#endif
	auth->status = AUTH_STATUS_DISABLED;
	mutex_init(&auth->mutex);
	refcount_set(&auth->usage, 1);
#ifdef CONFIG_QOS_CTRL
	for (i = QOS_POLICY_MIN_LEVEL; i < NR_QOS; ++i) {
		INIT_LIST_HEAD(&auth->tasks[i]);
		auth->num[i] = 0;
	}
#endif
}

void get_auth_struct(struct auth_struct *auth)
{
	refcount_inc(&auth->usage);
}

static void __put_auth_struct(struct auth_struct *auth)

{
	WARN_ON(auth->status != AUTH_STATUS_DEAD);
	WARN_ON(refcount_read(&auth->usage));

#ifdef CONFIG_QOS_CTRL
	/* refcount is zero here, no contend, no lock. */
	remove_qos_tasks(auth);
#endif
	kfree(auth);
}

void put_auth_struct(struct auth_struct *auth)
{
	if (refcount_dec_and_test(&auth->usage))
		__put_auth_struct(auth);
}

static int init_ua_idr(void)
{
	ua_idr = kzalloc(sizeof(*ua_idr), GFP_ATOMIC);
	if (ua_idr == NULL) {
		pr_err("[AUTH_CTRL] auth idr init failed, no memory!\n");
		return -ENOMEM;
	}

	idr_init(ua_idr);
	
	return 0;
}

static int init_super_authority(unsigned int auth_tgid)
{
	int ret;
	struct auth_struct *auth_super;

	auth_super = kzalloc(sizeof(*auth_super), GFP_ATOMIC);
	if(auth_super == NULL) {
		pr_err("[AUTH_CTRL] auth struct alloc failed\n");
		return -ENOMEM;
	}
	init_authority_record(auth_super);
	change_to_super(auth_super);

	ret = idr_alloc(ua_idr, auth_super, auth_tgid, auth_tgid + 1, GFP_ATOMIC);
	if(ret != auth_tgid) {
		pr_err("[AUTH_CTRL] authority for super init failed! ret=%d\n", ret);
		kfree(auth_super);
		return ret;
	}

	return 0;
}

int authority_remove_handler(int id, void *p, void *para)
{
	struct auth_struct *auth = (struct auth_struct *)p;

	mutex_lock(&auth->mutex);
#ifdef CONFIG_QOS_CTRL
	qos_switch(auth, AUTH_STATUS_DISABLED);
#endif
	auth->status = AUTH_STATUS_DEAD;
	mutex_unlock(&auth->mutex);
	put_auth_struct(auth);

	return 0;
}

void remove_authority_control(void)
{
	int ret;

	mutex_lock(&ua_idr_mutex);
	ret = idr_for_each(ua_idr, authority_remove_handler, NULL);
	if (ret < 0)
		pr_err("[AUTH_CTRL] authority item remove failed\n");

	idr_destroy(ua_idr);
	kfree(ua_idr);

	mutex_unlock(&ua_idr_mutex);
}

/*
 * constrain user assigned auth_flag to kernel accepted auth_flag
 */
static int generic_auth_trim(unsigned int orig_flag, unsigned int constrain)
{
	return orig_flag & constrain;
}

static inline void set_auth_flag(struct auth_ctrl_data *data, struct auth_struct *auth_to_enable)
{
#ifdef CONFIG_RTG_AUTHORITY
	auth_to_enable->rtg_auth_flag = generic_auth_trim(data->rtg_ua_flag, AF_RTG_DELEGATED);
#endif
#ifdef CONFIG_QOS_AUTHORITY
	auth_to_enable->qos_auth_flag = generic_auth_trim(data->qos_ua_flag, AF_QOS_ALL);
#endif
}

static int auth_enable(struct auth_ctrl_data *data)
{
	struct auth_struct *auth_to_enable;
	unsigned int tgid = data->pid;
	int status = data->status;
	int ret;

	mutex_lock(&ua_idr_mutex);
	auth_to_enable = idr_find(ua_idr, tgid);
	/* auth exist, just resume the task's qos request */
	if (auth_to_enable) {
		get_auth_struct(auth_to_enable);
		mutex_unlock(&ua_idr_mutex);

		mutex_lock(&auth_to_enable->mutex);
		if (auth_to_enable->status == AUTH_STATUS_DEAD) {
			mutex_unlock(&auth_to_enable->mutex);
			put_auth_struct(auth_to_enable);
			return -INVALID_AUTH;
		}

		set_auth_flag(data, auth_to_enable);
#ifdef CONFIG_QOS_CTRL
		qos_switch(auth_to_enable, status);
#endif
		auth_to_enable->status = status;
		mutex_unlock(&auth_to_enable->mutex);
		ret = 0;
		put_auth_struct(auth_to_enable);
		goto out;
	}

	/* auth not exist, build a new auth, then insert to idr */
	auth_to_enable = kzalloc(sizeof(*auth_to_enable), GFP_ATOMIC);
	if (!auth_to_enable) {
		mutex_unlock(&ua_idr_mutex);
		pr_err("[AUTH_CTRL] alloc auth data failed, no memory!\n");
		ret = -ENOMEM;
		goto out;
	}

	init_authority_record(auth_to_enable);

	/* no one could get the auth from idr now, no need to lock */
	set_auth_flag(data, auth_to_enable);
	auth_to_enable->status = status;

	ret = idr_alloc(ua_idr, auth_to_enable, tgid, tgid + 1, GFP_ATOMIC);
	if (ret < 0) {
		pr_err("[AUTH_CTRL] add auth to idr failed, no memory!\n");
		kfree(auth_to_enable);
	}

	mutex_unlock(&ua_idr_mutex);

out:
	return ret;
}

static int auth_delete(struct auth_ctrl_data *data)
{
	struct auth_struct *auth_to_delete;
	unsigned int tgid = data->pid;

	mutex_lock(&ua_idr_mutex);
	auth_to_delete = (struct auth_struct *)idr_remove(ua_idr, tgid);
	if (!auth_to_delete) {
		mutex_unlock(&ua_idr_mutex);
		pr_err("[AUTH_CTRL] no auth data for this pid=%d, delete failed\n", tgid);
		return -PID_NOT_FOUND;
	}
	mutex_unlock(&ua_idr_mutex);

	mutex_lock(&auth_to_delete->mutex);
#ifdef CONFIG_QOS_CTRL
	qos_switch(auth_to_delete, AUTH_STATUS_DISABLED);
#endif
	auth_to_delete->status = AUTH_STATUS_DEAD;
	mutex_unlock(&auth_to_delete->mutex);

	put_auth_struct(auth_to_delete);

	return 0;
}

static int auth_get(struct auth_ctrl_data *data)
{
	struct auth_struct *auth_to_get;
	unsigned int tgid = data->pid;

	mutex_lock(&ua_idr_mutex);
	auth_to_get = idr_find(ua_idr, tgid);
	if (!auth_to_get) {
		mutex_unlock(&ua_idr_mutex);
		pr_err("[AUTH_CTRL] no auth data for this pid=%d to get\n", tgid);
		return -PID_NOT_FOUND;
	}
	get_auth_struct(auth_to_get);
	mutex_unlock(&ua_idr_mutex);

	mutex_lock(&auth_to_get->mutex);
	if (auth_to_get->status == AUTH_STATUS_DEAD) {
		mutex_unlock(&auth_to_get->mutex);
		put_auth_struct(auth_to_get);
		return -INVALID_AUTH;
	}
#ifdef CONFIG_RTG_AUTHORITY
	data->rtg_ua_flag = auth_to_get->rtg_auth_flag;
#endif
#ifdef CONFIG_QOS_AUTHORITY
	data->qos_ua_flag = auth_to_get->qos_auth_flag;
#endif
	data->status = auth_to_get->status;
	mutex_unlock(&auth_to_get->mutex);

	put_auth_struct(auth_to_get);

	return 0;
}

static int auth_switch(struct auth_ctrl_data *data)
{
	struct auth_struct *auth;
	unsigned int tgid = data->pid;
	unsigned int status = data->status;

	if (status == 0 || status >= AUTH_STATUS_MAX_NR) {
		pr_err("[AUTH_CTRL] not valied status %d\n", status);
		return -ARG_INVALID;
	}

	mutex_lock(&ua_idr_mutex);
	auth = idr_find(ua_idr, tgid);
	if (!auth) {
		mutex_unlock(&ua_idr_mutex);
		pr_err("[AUTH_CTRL] no auth data for this pid=%d to switch\n", tgid);
		return -PID_NOT_FOUND;
	}
	get_auth_struct(auth);
	mutex_unlock(&ua_idr_mutex);

	mutex_lock(&auth->mutex);
	if (auth->status == AUTH_STATUS_DEAD) {
		mutex_unlock(&auth->mutex);
		put_auth_struct(auth);
		return -INVALID_AUTH;
	}

	set_auth_flag(data, auth);
#ifdef CONFIG_QOS_CTRL
	qos_switch(auth, status);
#endif
	auth->status = status;
	mutex_unlock(&auth->mutex);

	put_auth_struct(auth);

	return 0;
}

typedef int (*auth_manipulate_func)(struct auth_ctrl_data *data);

static auth_manipulate_func auth_func_array[AUTH_MAX_NR] = {
	/*
	 * auth_enable: Start authority control for specific tgid.
	 * auth_delte:  End authroity control, remove statistic datas.
	 * auth_get:    Get auth info, deprecated.
	 * auth_switch: Change authority flag and status for specific tgid.
	 */
	NULL,
	auth_enable,
	auth_delete,
	auth_get,
	auth_switch,
};

static long do_auth_manipulate(struct auth_ctrl_data *data)
{
	long ret = 0;
	unsigned int type = data->type;

	if (type >= AUTH_MAX_NR) {
		pr_err("[AUTH_CTRL] BASIC_AUTH_CTRL_OPERATION type not valid\n");
		return -ARG_INVALID;
	}

	if (auth_func_array[type])
		ret = (long)(*auth_func_array[type])(data);

	return ret;
}

static long ctrl_auth_basic_operation(int abi, void __user *uarg)
{
	struct auth_ctrl_data auth_data;
	long ret = -1;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-to-int-cast"

	switch (abi) {
	case AUTH_IOCTL_ABI_ARM32:
		ret = copy_from_user(&auth_data,
				(void __user *)compat_ptr((compat_uptr_t)uarg),
				sizeof(struct auth_ctrl_data));
		break;
	case AUTH_IOCTL_ABI_AARCH64:
		ret = copy_from_user(&auth_data, uarg, sizeof(struct auth_ctrl_data));
		break;
	default:
		pr_err("[AUTH_CTRL] abi format error\n");
		break;
	}

#pragma GCC diagnostic pop

	if (ret) {
		pr_err("[AUTH_RTG] %s copy user data failed\n", __func__);
		return ret;
	}

	ret = do_auth_manipulate(&auth_data);
	if (ret < 0) {
		pr_err("[AUTH_CTRL] BASIC_AUTH_CTRL_OPERATION failed\n");
		return ret;
	}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-to-int-cast"

	switch (abi) {
	case AUTH_IOCTL_ABI_ARM32:
		ret = copy_to_user((void __user *)compat_ptr((compat_uptr_t)uarg),
				&auth_data,
				sizeof(struct auth_ctrl_data));
		break;
	case AUTH_IOCTL_ABI_AARCH64:
		ret = copy_to_user(uarg, &auth_data, sizeof(struct auth_ctrl_data));
		break;
	default:
		pr_err("[AUTH_CTRL] abi format error\n");
		break;
	}

#pragma GCC diagnostic pop

	if (ret) {
		pr_err("[AUTH_RTG] %s copy user data failed\n", __func__);
		return ret;
	}

	return 0;
}

long do_auth_ctrl_ioctl(int abi, struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *uarg = (void __user *)arg;
	unsigned int func_cmd = _IOC_NR(cmd);

	if (uarg == NULL) {
		pr_err("%s: invalid user uarg\n", __func__);
		return -EINVAL;
	}

	if (_IOC_TYPE(cmd) != AUTH_CTRL_IPC_MAGIG) {
		pr_err("%s: authority ctrl magic fail, TYPE=%d\n",
		       __func__, _IOC_TYPE(cmd));
		return -EINVAL;
	}

	if (func_cmd >= AUTH_CTRL_MAX_NR) {
		pr_err("%s: authority ctrl cmd error, cmd:%d\n",
		       __func__, _IOC_TYPE(cmd));
		return -EINVAL;
	}

	if (g_func_array[func_cmd])
		return (*g_func_array[func_cmd])(abi, uarg);

	return -EINVAL;
}

#define get_authority_flag(func_id)	(1 << (func_id - 1))

static inline unsigned int get_true_uid(struct task_struct *p)
{
	if (!p)
		return get_uid(current_user())->uid.val;

	return task_uid(p).val;
}

/*
 * Return 1000 for both SYSTEM and ROOT
 * Return current's uid if p is NULL
 */
static inline unsigned int get_authority_uid(struct task_struct *p)
{
	unsigned int uid = get_true_uid(p);

	if (super_uid(uid))
		uid = SUPER_UID;

	return uid;
}

static unsigned int auth_flag(struct auth_struct *auth, unsigned int type)
{
	switch (type) {
#ifdef CONFIG_RTG_AUTHORITY
	case RTG_AUTH_FLAG:
		return auth->rtg_auth_flag;
#endif
#ifdef CONFIG_QOS_AUTHORITY
	case QOS_AUTH_FLAG:
		return auth->qos_auth_flag;
#endif
	default:
		pr_err("[AUTH_CTRL] not valid auth type\n");
		return INVALIED_AUTH_FLAG;
	}
}

bool check_authorized(unsigned int func_id, unsigned int type)
{
	bool authorized = false;
	struct auth_struct *auth;
	unsigned int af = get_authority_flag(func_id);
	unsigned int uid = get_authority_uid(NULL);
	unsigned int tgid = task_tgid_nr(current);

	mutex_lock(&ua_idr_mutex);
	if (!ua_idr) {
		mutex_unlock(&ua_idr_mutex);
		pr_err("[AUTH_CTRL] authority idr table missed, auth failed\n");
		return authorized;
	}

	auth = (struct auth_struct *)idr_find(ua_idr, tgid);
	if (!auth) {
		if (uid != SUPER_UID) {
			mutex_unlock(&ua_idr_mutex);
			pr_err("[AUTH_CTRL] no auth data for this pid = %d\n", tgid);
			return authorized;
		} else if (init_super_authority(tgid)) {
			mutex_unlock(&ua_idr_mutex);
			pr_err("[AUTH_CTRL] init super authority failed\n");
			return authorized;
		}

		//the auth must exist
		auth = (struct auth_struct *)idr_find(ua_idr, tgid);
		if (!auth)
			return authorized;
	}

	get_auth_struct(auth);
	mutex_unlock(&ua_idr_mutex);

	mutex_lock(&auth->mutex);
	if (auth->status == AUTH_STATUS_DEAD) {
		mutex_unlock(&auth->mutex);
		pr_info("[AUTH_CTRL] not valid auth for pid %d\n", tgid);
		put_auth_struct(auth);
		return authorized;
	}
	if (auth && (auth_flag(auth, type) & af))
		authorized = true;

	mutex_unlock(&auth->mutex);

	put_auth_struct(auth);

	return authorized;
}

/*
 * Return authority info for given task
 * return current's auth if p is NULL
 * refcount will inc if this call return the valid auth
 * make sure to call put_auth_struct before the calling end
 */
struct auth_struct *get_authority(struct task_struct *p)
{
	unsigned int tgid;
	struct auth_struct *auth;

	tgid = (p == NULL ? current->tgid : p->tgid);

	mutex_lock(&ua_idr_mutex);
	auth = idr_find(ua_idr, tgid);
	if (auth)
		get_auth_struct(auth);

	mutex_unlock(&ua_idr_mutex);

	return auth;
}

long proc_auth_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return do_auth_ctrl_ioctl(AUTH_IOCTL_ABI_AARCH64, file, cmd, arg);
}

#ifdef CONFIG_COMPAT
long proc_auth_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	return do_auth_ctrl_ioctl(AUTH_IOCTL_ABI_ARM32, file, cmd,
				(unsigned long)(compat_ptr((compat_uptr_t)arg)));
}
#endif

static const struct file_operations auth_ctrl_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl = proc_auth_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = proc_auth_compat_ioctl,
#endif
};

static struct miscdevice auth_ctrl_device = {
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= "auth_ctrl",
	.fops		= &auth_ctrl_fops,
};

static __init int auth_ctrl_init_module(void)
{
	int err;

	err = misc_register(&auth_ctrl_device);
	if (err < 0) {
		pr_err("auth_ctrl register failed\n");
		return err;
	}

	pr_info("auth_ctrl init success\n");

	BUG_ON(init_ua_idr());

#ifdef CONFIG_QOS_CTRL
	init_qos_ctrl();
#endif

	init_sched_auth_debug_procfs();

	return 0;
}

static void auth_ctrl_exit_module(void)
{
	remove_authority_control();
	misc_deregister(&auth_ctrl_device);
}

/* module entry points */
module_init(auth_ctrl_init_module);
module_exit(auth_ctrl_exit_module);

MODULE_LICENSE("GPL v2");

