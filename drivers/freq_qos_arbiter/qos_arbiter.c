// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Oplus. All rights reserved.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/pm_qos.h>
#include <linux/cpumask.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/cpufreq.h>
#include <linux/rwlock.h>
#include <linux/spinlock.h>
#include <trace/hooks/power.h>
#include <linux/plist.h>

#include <linux/qos_arbiter.h>
#include <linux/atomic.h>
#include <linux/seq_file.h>
#include <linux/topology.h>
#include <linux/device.h>
#include <linux/sched/cpufreq.h>


static struct kset *qos_kset;
static struct kobject *qos_param_kobj;
struct atomic_notifier_head sbe_notifier_chain;
s32 gDuration = DEFAULT_QOS_DURATION;
EXPORT_SYMBOL(gDuration);

static struct {
	s32 thermal_freq[DEFAULT_NR_CPUS];
	spinlock_t lock;
} thermal_freqs;

static DEFINE_MUTEX(freq_qos_mutex);

static ssize_t oplus_get_cpu_min_freq(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf);
static ssize_t oplus_set_cpu_min_freq(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count);
static ssize_t oplus_get_cpu_max_freq(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf);
static ssize_t oplus_set_cpu_max_freq(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count);
static ssize_t oplus_get_thermal_max_freq(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf);
static ssize_t oplus_set_thermal_max_freq(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count);

static struct kobj_attribute cpu_min_freq_attr =
	__ATTR(cpu_min_freq, 0644, oplus_get_cpu_min_freq, oplus_set_cpu_min_freq);
static struct kobj_attribute cpu_max_freq_attr =
	__ATTR(cpu_max_freq, 0644, oplus_get_cpu_max_freq, oplus_set_cpu_max_freq);
static struct kobj_attribute cpu_thermal_freq_attr =
	__ATTR(thermal_max_freq, 0644, oplus_get_thermal_max_freq, oplus_set_thermal_max_freq);


static struct attribute *param_attrs[] = {
	&cpu_min_freq_attr.attr,
	&cpu_max_freq_attr.attr,
	&cpu_thermal_freq_attr.attr,
	NULL,
};

static struct attribute_group param_attr_group = {
	.attrs = param_attrs,
};

static int add_module_params(void)
{
	int ret;
	struct kobject *module_obj;

	module_obj = &qos_kset->kobj;
	qos_param_kobj = kobject_create_and_add("parameters", module_obj);
	if (!qos_param_kobj) {
		pr_err("qos_arbiter: Failed to add param_kobj.\n");
		return -ENOMEM;
	}

	ret = sysfs_create_group(qos_param_kobj, &param_attr_group);
	if (ret)
		pr_err("qos_arbiter: Failed to create sysfs.\n");

	return ret;
}

static int __init thermal_freq_init(void)
{
	memset(&thermal_freqs, FREQ_QOS_MAX_DEFAULT_VALUE, sizeof(thermal_freqs));
	spin_lock_init(&thermal_freqs.lock);
	return 0;
}

/* Here for cpufreq min/max request */
static struct qos_manager global_qos_manager = {
	.min_list = PLIST_HEAD_INIT(global_qos_manager.min_list),
	.max_list = PLIST_HEAD_INIT(global_qos_manager.max_list),
	.min_max_val = INT_MAX,
	.max_min_val = 0,
	.lock = __SPIN_LOCK_UNLOCKED(global_qos_manager.lock),
};


struct oplus_cpu_status {
	unsigned int min;
	unsigned int max;
};


static DEFINE_PER_CPU(struct oplus_cpu_status, oplus_perf_cpu_stats);
static DEFINE_PER_CPU(struct freq_qos_request, oplus_qos_req_min);
static DEFINE_PER_CPU(struct freq_qos_request, oplus_qos_req_max);
static DEFINE_PER_CPU(struct freq_qos_request, oplus_sbe_qos_req_max);

static cpumask_var_t cpu_limit_mask_min;
static cpumask_var_t cpu_limit_mask_max;

struct qos_backup {
	struct list_head list;
	struct freq_qos_request *req;
	s32 value;
	enum freq_qos_req_type type;
	enum QOS_OWNER owner;
};
struct sbe_exclusive_context {
	struct list_head list;
	struct timer_list timer;
	struct list_head backup_list;
	bool is_active;
	spinlock_t lock;
};
static struct sbe_exclusive_context sbe_context = {
	.backup_list = LIST_HEAD_INIT(sbe_context.backup_list),
	.is_active = false,
	.lock = __SPIN_LOCK_UNLOCKED(sbe_context.lock),
};

static bool ready_for_qos_update;

/******** The following code is copied from lib/plist.c. ********/
#ifdef CONFIG_DEBUG_PLIST

static struct plist_head test_head;

static void plist_check_prev_next(struct list_head *t, struct list_head *p,
				  struct list_head *n)
{
	WARN(n->prev != p || p->next != n,
			"top: %p, n: %p, p: %p\n"
			"prev: %p, n: %p, p: %p\n"
			"next: %p, n: %p, p: %p\n",
			 t, t->next, t->prev,
			p, p->next, p->prev,
			n, n->next, n->prev);
}

static void plist_check_list(struct list_head *top)
{
	struct list_head *prev = top, *next = top->next;

	plist_check_prev_next(top, prev, next);
	while (next != top) {
		prev = next;
		next = prev->next;
		plist_check_prev_next(top, prev, next);
	}
}

static void plist_check_head(struct plist_head *head)
{
	if (!plist_head_empty(head))
		plist_check_list(&plist_first(head)->prio_list);
	plist_check_list(&head->node_list);
}

#else
# define plist_check_head(h)	do { } while (0)
#endif

/******** The code above is copied from lib/plist.c. ********/

static int oplus_qos_val_invalid(s32 value)
{
	return value < 0 && value != PM_QOS_DEFAULT_VALUE;
}

static inline int oplus_qos_request_active(struct freq_qos_request *req)
{
	return !IS_ERR_OR_NULL(req->qos);
}

static int freq_qos_req_init(void)
{
	unsigned int cpu;
	struct cpufreq_policy *policy;
	struct freq_qos_request *req;
	int ret;

	for_each_present_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy) {
			pr_err("%s: Failed to get cpufreq policy for cpu%d\n", __func__, cpu);
			ret = -EAGAIN;
			goto cleanup;
		}
		per_cpu(oplus_perf_cpu_stats, cpu).min = 0;
		req = &per_cpu(oplus_qos_req_min, cpu);
		ret = freq_qos_add_request(&policy->constraints, req,
										FREQ_QOS_MIN, FREQ_QOS_MIN_DEFAULT_VALUE);
		if (ret < 0) {
			pr_err("%s: Failed to add min freq constraint %d\n", __func__, ret);
			cpufreq_cpu_put(policy);
			goto cleanup;
		}

		per_cpu(oplus_perf_cpu_stats, cpu).max = FREQ_QOS_MAX_DEFAULT_VALUE;
		req = &per_cpu(oplus_qos_req_max, cpu);
		ret = oplus_add_freq_qos_request(QOS_OWNER_UAH, &policy->constraints, req,
										FREQ_QOS_MAX, FREQ_QOS_MAX_DEFAULT_VALUE);
		if (ret < 0) {
			pr_err("%s: Failed to add max freq constraint %d\n", __func__, ret);
			cpufreq_cpu_put(policy);
			goto cleanup;
		}

		req = &per_cpu(oplus_sbe_qos_req_max, cpu);
		ret = freq_qos_add_request(&policy->constraints, req,
									FREQ_QOS_MAX, FREQ_QOS_MAX_DEFAULT_VALUE);
		if (ret < 0) {
			pr_err("%s: Failed to add sbe max freq constraint %d\n", __func__, ret);
			cpufreq_cpu_put(policy);
			goto cleanup;
		}

		cpufreq_cpu_put(policy);
	}
	return 0;

cleanup:
	for_each_present_cpu(cpu) {
		req = &per_cpu(oplus_qos_req_min, cpu);
		if (req && freq_qos_request_active(req))
			freq_qos_remove_request(req);

		req = &per_cpu(oplus_qos_req_max, cpu);
		if (req && freq_qos_request_active(req))
			oplus_remove_freq_qos_request(QOS_OWNER_UAH, req);

		req = &per_cpu(oplus_sbe_qos_req_max, cpu);
		if (req && freq_qos_request_active(req))
			freq_qos_remove_request(req);

		per_cpu(oplus_perf_cpu_stats, cpu).min = 0;
		per_cpu(oplus_perf_cpu_stats, cpu).max = FREQ_QOS_MAX_DEFAULT_VALUE;
	}
	return ret;
}


static ssize_t oplus_get_cpu_min_freq(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	int cpu, cnt = 0;

	for_each_present_cpu(cpu) {
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "%d:%u ",
						cpu, per_cpu(oplus_perf_cpu_stats, cpu).min);
	}
	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\n");
	return cnt;
}

static ssize_t oplus_set_cpu_min_freq(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	int i, nrtokens = 0;
	unsigned int cpu, val;
	const char *cpybuf = buf;
	struct oplus_cpu_status *i_cpu_stats;
	struct freq_qos_request *req;
	int ret = 0;

	mutex_lock(&freq_qos_mutex);
	if (!ready_for_qos_update) {
		ret = freq_qos_req_init();
		if (ret) {
			pr_err("%s: Failed to init qos requests policy, ret:%d\n", __func__, ret);
			mutex_unlock(&freq_qos_mutex);
			return ret;
		}
		pr_err("%s:qos isn't init.\n", __func__);
		ready_for_qos_update = true;
	}
	mutex_unlock(&freq_qos_mutex);

	/* 0:2016000 1:2016000 2:1824000 3:1824000 4:1824000 5:1824000 6:1824000 7:2284800 */
	while ((cpybuf = strpbrk(cpybuf + 1, " :")))
		nrtokens++;

	if (!(nrtokens % 2))
		return -EINVAL;

	cpybuf = buf;
	cpumask_clear(cpu_limit_mask_min);
	for (i = 0; i < nrtokens; i += 2) {
		if (sscanf(cpybuf, "%u:%u", &cpu, &val) != 2)
			return -EINVAL;
		if (cpu >= nr_cpu_ids)
			break;

		if (cpu_possible(cpu)) {
			i_cpu_stats = &per_cpu(oplus_perf_cpu_stats, cpu);
			i_cpu_stats->min = val;
			cpumask_set_cpu(cpu, cpu_limit_mask_min);
		}

		cpybuf = strnchr(cpybuf, strlen(cpybuf), ' ');
		cpybuf++;
	}

	cpus_read_lock();
	for_each_cpu(i, cpu_limit_mask_min) {
		i_cpu_stats = &per_cpu(oplus_perf_cpu_stats, i);
		req = &per_cpu(oplus_qos_req_min, i);
		if (oplus_update_freq_qos_request(QOS_OWNER_UAH, req, i_cpu_stats->min) < 0)
			continue;
	}
	cpus_read_unlock();

	return count;
}

static ssize_t oplus_get_cpu_max_freq(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	int cpu, cnt = 0;

	for_each_present_cpu(cpu) {
		cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "%d:%u ",
						cpu, per_cpu(oplus_perf_cpu_stats, cpu).max);
	}
	cnt += scnprintf(buf + cnt, PAGE_SIZE - cnt, "\n");
	return cnt;
}

static ssize_t oplus_set_cpu_max_freq(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	int i, nrtokens = 0;
	unsigned int cpu, val;
	const char *cpybuf = buf;
	struct oplus_cpu_status *i_cpu_stats;
	struct freq_qos_request *req;
	int ret = 0;

	mutex_lock(&freq_qos_mutex);
	if (!ready_for_qos_update) {
		ret = freq_qos_req_init();
		if (ret) {
			pr_err("%s: Failed to init qos requests policy, ret:%d\n", __func__, ret);
			mutex_unlock(&freq_qos_mutex);
			return ret;
		}
		pr_err("%s:qos isn't init.\n", __func__);
		ready_for_qos_update = true;
	}
	mutex_unlock(&freq_qos_mutex);

	/* 0:2016000 1:2016000 2:1824000 3:1824000 4:1824000 5:1824000 6:1824000 7:2284800 */
	while ((cpybuf = strpbrk(cpybuf + 1, " :")))
		nrtokens++;

	if (!(nrtokens % 2))
		return -EINVAL;

	cpybuf = buf;
	cpumask_clear(cpu_limit_mask_max);
	for (i = 0; i < nrtokens; i += 2) {
		if (sscanf(cpybuf, "%u:%u", &cpu, &val) != 2)
			return -EINVAL;
		if (cpu >= nr_cpu_ids)
			break;

		if (cpu_possible(cpu)) {
			i_cpu_stats = &per_cpu(oplus_perf_cpu_stats, cpu);
			i_cpu_stats->max = min_t(uint, val, (unsigned int)FREQ_QOS_MAX_DEFAULT_VALUE);
			cpumask_set_cpu(cpu, cpu_limit_mask_max);
		}

		cpybuf = strnchr(cpybuf, strlen(cpybuf), ' '); /* find next space */
		cpybuf++;
	}

	cpus_read_lock();
	for_each_cpu(i, cpu_limit_mask_max) {
		i_cpu_stats = &per_cpu(oplus_perf_cpu_stats, i);
		req = &per_cpu(oplus_qos_req_max, i);
		if (oplus_update_freq_qos_request(QOS_OWNER_UAH, req, i_cpu_stats->max) < 0)
			continue;
	}
	cpus_read_unlock();

	return count;
}

static ssize_t oplus_get_thermal_max_freq(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	ssize_t count = 0;
	unsigned long flags;
	int cpu;

	spin_lock_irqsave(&thermal_freqs.lock, flags);
	for (cpu = 0; cpu < DEFAULT_NR_CPUS; cpu++) {
		count += snprintf(buf + count, PAGE_SIZE - count, "%d:%d ",
						cpu, thermal_freqs.thermal_freq[cpu]);
	}
	spin_unlock_irqrestore(&thermal_freqs.lock, flags);

	count += scnprintf(buf + count, PAGE_SIZE - count, "\n");
	return count;
}

static ssize_t oplus_set_thermal_max_freq(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	int i, nrtokens = 0;
	unsigned int cpu, val;
	const char *cpybuf = buf;
	unsigned long flags;
	s32 old_val;
	bool value_changed = false;

	/* 0:2016000 1:2016000 2:1824000 3:1824000 4:1824000 5:1824000 6:1824000 7:2284800 */
	while ((cpybuf = strpbrk(cpybuf + 1, " :")))
		nrtokens++;

	if (!(nrtokens % 2))
		return -EINVAL;

	cpybuf = buf;

	spin_lock_irqsave(&thermal_freqs.lock, flags);
	for (i = 0; i < nrtokens; i += 2) {
		if (sscanf(cpybuf, "%u:%u", &cpu, &val) != 2)
			return -EINVAL;
		if (cpu >= nr_cpu_ids)
			break;

		old_val = thermal_freqs.thermal_freq[cpu];
		if (old_val != val) {
			thermal_freqs.thermal_freq[cpu] = val;
			value_changed = true;
		}

		cpybuf = strnchr(cpybuf, strlen(cpybuf), ' '); /* find next space */
		cpybuf++;
	}

	spin_unlock_irqrestore(&thermal_freqs.lock, flags);

	if (value_changed)
		oplus_update_sbe_val();

	return count;
}


static void __attribute__((unused)) print_qos_lists(void)
{
	struct plist_node *node;
	struct qos_record *rec;

	pr_err("=====QOS MIN List (Priority Order) =====\n");
	plist_for_each(node, &global_qos_manager.min_list) {
		rec = container_of(node, struct qos_record, node);
		pr_err("QOS Owner: %d, Type: %s, Value: %d\n",
			   rec->owner,
			   (rec->type == FREQ_QOS_MIN) ? "MIN" : "MAX",
			   node->prio);
	}

	pr_err("=====QOS MAX List (Priority Order) =====\n");
	plist_for_each(node, &global_qos_manager.max_list) {
		rec = container_of(node, struct qos_record, node);
		pr_err("QOS Owner: %d, Type: %s, Value: %d\n",
			   rec->owner,
			   (rec->type == FREQ_QOS_MIN) ? "MIN" : "MAX",
			   node->prio);
	}
}

static void __attribute__((unused)) print_qos_backup_lists(void)
{
	struct qos_backup *backup;

	pr_err("=====QOS Backup List =====\n");
	list_for_each_entry(backup, &sbe_context.backup_list, list) {
		pr_err("QOS Owner: %d, Value: %d, Type: %s\n",
			   backup->owner,
			   backup->value,
			   (backup->type == FREQ_QOS_MIN) ? "MIN" : "MAX");
	}
}

void __attribute__((unused)) dumplist(void)
{
	unsigned long flags = 0;
	spin_lock_irqsave(&global_qos_manager.lock, flags);
	print_qos_lists();
	spin_unlock_irqrestore(&global_qos_manager.lock, flags);

	spin_lock_irqsave(&sbe_context.lock, flags);
	print_qos_backup_lists();
	spin_unlock_irqrestore(&sbe_context.lock, flags);
}
EXPORT_SYMBOL_GPL(dumplist);


ssize_t oplus_update_freq_qos_request(enum QOS_OWNER owner, struct freq_qos_request *req, int val)
{
	struct qos_record *old_record = NULL;
	struct qos_record *new_record = NULL;
	struct plist_node *node, *temp;
	struct qos_backup *backup, *existing_backup = NULL;
	unsigned long flags = 0, sbe_flags = 0;
	bool sbe_skip_insert = false;
	int ret = 0;

	if (!req || oplus_qos_val_invalid(val))
		return -EINVAL;

	if (WARN(!oplus_qos_request_active(req), "%s() called for unknown object\n", __func__))
		return -EINVAL;

	if (owner < QOS_OWNER_UAH || owner >= QOS_OWNER_MAX) {
		pr_err("%s: Invalid QOS_OWNER: %d\n", __func__, owner);
		return -EINVAL;
	}

	if (req->type == FREQ_QOS_MIN) {
		ret = freq_qos_update_request(req, val);
		return ret;
	}

	spin_lock_irqsave(&global_qos_manager.lock, flags);
	/* find old */
	switch (req->type) {
	case FREQ_QOS_MIN:
		break;
	case FREQ_QOS_MAX:
		plist_for_each_safe(node, temp, &global_qos_manager.max_list) {
			struct qos_record *rec = container_of(node, struct qos_record, node);

			if (rec->req == req) {
				old_record = rec;
				plist_del(node, &global_qos_manager.max_list);
				break;
			}
		}
		break;
	default:
		ret = -EINVAL;
		goto unlock;
	}

	if (old_record)
		kfree(old_record);

	/* if a non-SBE request is updated during sbe exclusivity */
	spin_lock_irqsave(&sbe_context.lock, sbe_flags);
	if (sbe_context.is_active && owner != QOS_OWNER_SBE && req->type == FREQ_QOS_MAX) {
		list_for_each_entry(backup, &sbe_context.backup_list, list) {
			if (backup->req == req) {
				existing_backup = backup;
				break;
			}
		}

		if (existing_backup) {
			existing_backup->value = val;
		} else {
			backup = kmalloc(sizeof(*backup), GFP_ATOMIC);
			if (backup) {
				backup->owner = owner;
				backup->req = req;
				backup->type = req->type;
				backup->value = val;
				list_add_tail(&backup->list, &sbe_context.backup_list);
			}
		}
		sbe_skip_insert = true;
	}

	spin_unlock_irqrestore(&sbe_context.lock, sbe_flags);
	if (sbe_skip_insert)
		goto unlock;

	/* insert new */
	new_record = kzalloc(sizeof(*new_record), GFP_ATOMIC);
	if (!new_record) {
		ret = -ENOMEM;
		goto unlock;
	}

	new_record->owner = owner;
	new_record->req = req;
	new_record->type = req->type;

	plist_node_init(&new_record->node, val);

	switch (req->type) {
	case FREQ_QOS_MIN:
		break;
	case FREQ_QOS_MAX:
		plist_add(&new_record->node, &global_qos_manager.max_list);
		break;
	default:
		kfree(new_record);
		ret = -EINVAL;
	}

	ret = freq_qos_update_request(req, val);

unlock:
	spin_unlock_irqrestore(&global_qos_manager.lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(oplus_update_freq_qos_request);

ssize_t oplus_add_freq_qos_request(enum QOS_OWNER owner,
	struct freq_constraints *qos, struct freq_qos_request *req, enum freq_qos_req_type type, s32 value)
{
	int ret = 0;
	struct qos_record *new_record;
	unsigned long flags;

	if (owner < QOS_OWNER_UAH || owner >= QOS_OWNER_MAX) {
		return -EINVAL;
	}

	value = (type == FREQ_QOS_MIN ?
			FREQ_QOS_MIN_DEFAULT_VALUE : FREQ_QOS_MAX_DEFAULT_VALUE);

	ret = freq_qos_add_request(qos, req, type, value);
	if (ret < 0) {
		pr_err("%s: Failed to add freq constraint, owner:%d\n", __func__, owner);
		return -EINVAL;
	}

	if (type == FREQ_QOS_MIN) {
		return ret;
	}

	new_record = kmalloc(sizeof(*new_record), GFP_KERNEL);
	if (!new_record) {
		freq_qos_remove_request(req);
		pr_err("%s: failed to add req, owner:%d\n", __func__, owner);
		return -ENOMEM;
	}

	new_record->owner = owner;
	new_record->req = req;
	new_record->type = type;

	spin_lock_irqsave(&global_qos_manager.lock, flags);

	plist_node_init(&new_record->node, value);

	switch (type) {
	case(FREQ_QOS_MIN):
		break;
	case(FREQ_QOS_MAX):
		plist_add(&new_record->node, &global_qos_manager.max_list);
		break;
	default:
		kfree(new_record);
		freq_qos_remove_request(req);
		ret = -EINVAL;
	}

	spin_unlock_irqrestore(&global_qos_manager.lock, flags);

	return ret;
}
EXPORT_SYMBOL_GPL(oplus_add_freq_qos_request);


/* remove request from min/max list*/
ssize_t oplus_remove_freq_qos_request(enum QOS_OWNER owner, struct freq_qos_request *req)
{
	struct plist_node *node, *tmp;
	struct qos_record *rec;
	struct qos_backup *entry, *temp;
	enum freq_qos_req_type type = FREQ_QOS_MIN;
	unsigned long flags, sbe_flags;
	int found = 0;

	if (!req)
		return -EINVAL;

	if (WARN(!oplus_qos_request_active(req), "%s() called for unknown object\n", __func__))
		return -EINVAL;

	if (owner < QOS_OWNER_UAH || owner >= QOS_OWNER_MAX) {
		pr_err("%s: Invalid QOS_OWNER: %d\n", __func__, owner);
		return -EINVAL;
	}

	if (req->type == FREQ_QOS_MIN) {
		freq_qos_remove_request(req);
		return 0;
	}

	spin_lock_irqsave(&sbe_context.lock, sbe_flags);
	/* remove other qos req, during sbe active */
	if (sbe_context.is_active && owner != QOS_OWNER_SBE && req->type == FREQ_QOS_MAX) {
		/* for backup list */
		list_for_each_entry_safe(entry, temp, &sbe_context.backup_list, list) {
			if (entry->req == req && entry->owner == owner) {
				list_del(&entry->list);
				kfree(entry);
			}
		}
	}
	spin_unlock_irqrestore(&sbe_context.lock, sbe_flags);

	spin_lock_irqsave(&global_qos_manager.lock, flags);
	/* for max */
	plist_for_each_safe(node, tmp, &global_qos_manager.max_list) {
		rec = container_of(node, struct qos_record, node);
		if (rec->req == req && rec->owner == owner) {
			type = FREQ_QOS_MAX;
			plist_del(node, &global_qos_manager.max_list);
			kfree(rec);
			found = 1;
			goto update_val;
		}
	}

	if (!sbe_context.is_active && !found) {
		spin_unlock_irqrestore(&global_qos_manager.lock, flags);
		return -ENOENT;
	}

update_val:
	spin_unlock_irqrestore(&global_qos_manager.lock, flags);

	freq_qos_remove_request(req);
	return 0;
}
EXPORT_SYMBOL_GPL(oplus_remove_freq_qos_request);


static s32 __attribute__((unused)) oplus_get_min_max_value(void)
{
	unsigned long flags;
	s32 val = 100;
	struct plist_node *node;

	spin_lock_irqsave(&global_qos_manager.lock, flags);
	if (plist_head_empty(&global_qos_manager.max_list))
		goto unlock;

	plist_for_each(node, &global_qos_manager.max_list) {
		struct qos_record *rec = container_of(node, struct qos_record, node);

		if (rec->owner != QOS_OWNER_SBE) {
			val = node->prio;
			break;
		}
	}

unlock:
	spin_unlock_irqrestore(&global_qos_manager.lock, flags);
	return val;
}

ssize_t oplus_restore_freq_qos_request(enum QOS_OWNER owner, struct freq_qos_request *req, int val)
{
	struct qos_record *new_record;
	int ret = 0;

	if (!req || oplus_qos_val_invalid(val) ||
		owner < QOS_OWNER_UAH || owner >= QOS_OWNER_MAX) {
		return -EINVAL;
	}

	new_record = kzalloc(sizeof(*new_record), GFP_ATOMIC);
	if (!new_record)
		return -ENOMEM;

	new_record->owner = owner;
	new_record->req = req;
	new_record->type = req->type;
	plist_node_init(&new_record->node, val);

	switch (req->type) {
	case FREQ_QOS_MIN:
		break;
	case FREQ_QOS_MAX:
		plist_add(&new_record->node, &global_qos_manager.max_list);
		break;
	default:
		kfree(new_record);
		ret = -EINVAL;
		goto unlock;
	}

	ret = freq_qos_update_request(req, val);
	if (ret < 0) {
		plist_del(&new_record->node, &global_qos_manager.max_list);
		kfree(new_record);
	}

unlock:
	return ret;
}
EXPORT_SYMBOL_GPL(oplus_restore_freq_qos_request);

static void restore_backup_requests(struct timer_list *tl)
{
	struct sbe_exclusive_context *ctx = from_timer(ctx, tl, timer);
	struct qos_backup *entry, *temp;
	struct plist_node *node, *tmp;
	struct freq_qos_request *thermal_req;
	int cpu;
	int ret = 0;
	unsigned long flags, sbe_flags;

	spin_lock_irqsave(&global_qos_manager.lock, flags);
	spin_lock_irqsave(&ctx->lock, sbe_flags);

	if (ctx->is_active == false)
		goto out;

	ctx->is_active = false;

	for_each_possible_cpu(cpu) {
		thermal_req = &per_cpu(oplus_sbe_qos_req_max, cpu);
		ret = freq_qos_update_request(thermal_req, FREQ_QOS_MAX_DEFAULT_VALUE);
		if (ret < 0)
			pr_err("Failed to restore SBE QoS for CPU%d: %d\n", cpu, ret);
	}

	plist_for_each_safe(node, tmp, &global_qos_manager.max_list) {
		struct qos_record *rec = container_of(node, struct qos_record, node);
		plist_del(node, &global_qos_manager.max_list);
		kfree(rec);
	}

	list_for_each_entry_safe(entry, temp, &ctx->backup_list, list) {
		oplus_restore_freq_qos_request(entry->owner, entry->req, entry->value);
		list_del(&entry->list);
		kfree(entry);
	}

out:
	spin_unlock_irqrestore(&ctx->lock, sbe_flags);
	spin_unlock_irqrestore(&global_qos_manager.lock, flags);
}

/* already inside lock
 * move other request to backuplist
 */
static void process_req_to_backup(struct plist_head *list_head,
								int default_val, struct list_head *backup_head)
{
	struct plist_node *node, *tmp;

	plist_for_each_safe(node, tmp, list_head) {
		struct qos_record *rec = container_of(node, struct qos_record, node);
		struct qos_backup *backup = kmalloc(sizeof(*backup), GFP_ATOMIC);

		if (backup) {
			backup->owner = rec->owner;
			backup->req = rec->req;
			backup->type = rec->type;
			backup->value = rec->node.prio;
			list_add_tail(&backup->list, backup_head);
		}
		freq_qos_update_request(rec->req, default_val);

		plist_del(node, list_head);
		kfree(rec);
	}
}

/* apply new thermal_freq to qos */
ssize_t oplus_update_sbe_val(void)
{
	int ret = 0;
	int cpu;
	struct freq_qos_request *sbe_req;
	unsigned long flags, sbe_flags;
	s32 temp_freqs[DEFAULT_NR_CPUS];

	/* update thermal limit during sbe or q2q*/
	spin_lock_irqsave(&sbe_context.lock, sbe_flags);
	if (!sbe_context.is_active)
		goto out;

	spin_lock_irqsave(&thermal_freqs.lock, flags);
	memcpy(temp_freqs, thermal_freqs.thermal_freq, sizeof(temp_freqs));
	spin_unlock_irqrestore(&thermal_freqs.lock, flags);

	for_each_possible_cpu(cpu) {
		sbe_req = &per_cpu(oplus_sbe_qos_req_max, cpu);
		freq_qos_update_request(sbe_req, temp_freqs[cpu]);
	}

out:
	spin_unlock_irqrestore(&sbe_context.lock, sbe_flags);
	return ret;
}


/* for break max */
static void sbe_event_active_handle(int duration)
{
	unsigned long flags, sbe_flags;
	int cpu;
	int ret = 0;
	s32 temp_freqs[DEFAULT_NR_CPUS];
	struct freq_qos_request *thermal_req;

	spin_lock_irqsave(&thermal_freqs.lock, flags);
	memcpy(temp_freqs, thermal_freqs.thermal_freq, sizeof(temp_freqs));
	spin_unlock_irqrestore(&thermal_freqs.lock, flags);

	spin_lock_irqsave(&global_qos_manager.lock, flags);
	spin_lock_irqsave(&sbe_context.lock, sbe_flags);

	/* limit max to MAX2 */
	for_each_possible_cpu(cpu) {
		thermal_req = &per_cpu(oplus_sbe_qos_req_max, cpu);

		ret = freq_qos_update_request(thermal_req, temp_freqs[cpu]);
		if (ret < 0)
			pr_err("Failed to restore SBE QoS for CPU%d: %d\n", cpu, ret);
	}

	process_req_to_backup(&global_qos_manager.max_list,
						  FREQ_QOS_MAX_DEFAULT_VALUE,
						  &sbe_context.backup_list);

	if (!sbe_context.is_active) {
		sbe_context.is_active = true;
		timer_setup(&sbe_context.timer, restore_backup_requests, 0);

		if (duration > 0)
			mod_timer(&sbe_context.timer, jiffies + msecs_to_jiffies(duration));
		else if (duration == 0)
			mod_timer(&sbe_context.timer, jiffies + msecs_to_jiffies(gDuration));
	}

	spin_unlock_irqrestore(&sbe_context.lock, sbe_flags);
	spin_unlock_irqrestore(&global_qos_manager.lock, flags);
}


int sbe_register_notifier(struct notifier_block *nb)
{
	return atomic_notifier_chain_register(&sbe_notifier_chain, nb);
}

int sbe_unregister_notifier(struct notifier_block *nb)
{
	return atomic_notifier_chain_unregister(&sbe_notifier_chain, nb);
}

void sbe_notify_event(enum sbe_event event, struct oplus_qos_event_data *data)
{
	atomic_notifier_call_chain(&sbe_notifier_chain, (unsigned long)event, data);
}
EXPORT_SYMBOL(sbe_notify_event);


static void sbe_release_early(void)
{
	unsigned long flags;

	spin_lock_irqsave(&sbe_context.lock, flags);
	if (!sbe_context.is_active) {
		spin_unlock_irqrestore(&sbe_context.lock, flags);
		return;
	}
	spin_unlock_irqrestore(&sbe_context.lock, flags);

	/* sync is safe */
	del_timer_sync(&sbe_context.timer);
	restore_backup_requests(&sbe_context.timer);
}

static int sbe_notifier_call(struct notifier_block *nb, unsigned long action, void *data)
{
	enum sbe_event event = (enum sbe_event)action;
	struct oplus_qos_event_data *event_data = (struct oplus_qos_event_data *)data;

	if (unlikely(!ready_for_qos_update)) {
		pr_err("qos_arbiter: initialization has not been completed yet.\n");
		return NOTIFY_DONE;
	}

	switch (event) {
	case SBE_EVENT_ACTIVATE:
		if (event_data)
			sbe_event_active_handle(event_data->duration);
		else
			sbe_event_active_handle(gDuration);
		break;
	case SBE_EVENT_DEACTIVE:
		sbe_release_early();
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block sbe_nb = {
	.notifier_call = sbe_notifier_call,
};

static void cleanup_sysfs(void)
{
	if (qos_param_kobj) {
		sysfs_remove_file(qos_param_kobj, &cpu_min_freq_attr.attr);
		sysfs_remove_file(qos_param_kobj, &cpu_max_freq_attr.attr);
		sysfs_remove_file(qos_param_kobj, &cpu_thermal_freq_attr.attr);

		sysfs_remove_group(qos_param_kobj, &param_attr_group);
		kobject_put(qos_param_kobj);
		qos_param_kobj = NULL;
	}

	if (qos_kset) {
		kset_unregister(qos_kset);
		qos_kset = NULL;
	}
}


static int __init qos_arbiter_init(void)
{
	if (!alloc_cpumask_var(&cpu_limit_mask_min, GFP_KERNEL))
		return -ENOMEM;

	if (!alloc_cpumask_var(&cpu_limit_mask_max, GFP_KERNEL)) {
		free_cpumask_var(cpu_limit_mask_min);
		return -ENOMEM;
	}

	qos_kset = kset_create_and_add("qos_arbiter", NULL, kernel_kobj);
	if (!qos_kset) {
		free_cpumask_var(cpu_limit_mask_min);
		free_cpumask_var(cpu_limit_mask_max);
		return -ENOMEM;
	}

	ATOMIC_INIT_NOTIFIER_HEAD(&sbe_notifier_chain);
	sbe_register_notifier(&sbe_nb);

	thermal_freq_init();
	add_module_params();

	return 0;
}

static void __exit qos_arbiter_exit(void)
{
	struct plist_node *node, *tmp;
	struct qos_record *rec;
	unsigned long flags, sbe_flags;

	sbe_unregister_notifier(&sbe_nb);

	spin_lock_irqsave(&global_qos_manager.lock, flags);
	spin_lock_irqsave(&sbe_context.lock, sbe_flags);
	if (sbe_context.is_active) {
		sbe_context.is_active = false;
		sbe_release_early();
	}
	plist_for_each_safe(node, tmp, &global_qos_manager.max_list) {
		rec = container_of(node, struct qos_record, node);
		plist_del(node, &global_qos_manager.max_list);
		kfree(rec);
	}

	spin_unlock_irqrestore(&sbe_context.lock, sbe_flags);
	spin_unlock_irqrestore(&global_qos_manager.lock, flags);

	cleanup_sysfs();

	return;
}

module_init(qos_arbiter_init);
module_exit(qos_arbiter_exit);
MODULE_LICENSE("GPL v2");
