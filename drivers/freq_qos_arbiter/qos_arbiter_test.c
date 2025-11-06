// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Oplus. All rights reserved.
 */

#define pr_fmt(fmt) "qos_arbiter_test: " fmt

#include <asm-generic/errno-base.h>
#include <linux/module.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/seq_file.h>
#include <linux/qos_arbiter.h>


struct proc_dir_entry *d_qos_entry;
static int g_enable_qos_test = 0;

static struct {
	int owner;
	int value;
	int target_cpu;
	bool valid;
	spinlock_t lock;
} last_qos_input = {
	.lock = __SPIN_LOCK_UNLOCKED(last_qos_input.lock),
	.valid = false,
};


struct qos_test_entry {
	struct list_head list;
	enum QOS_OWNER owner;
	struct freq_qos_request req;
	struct cpufreq_policy *policy;
	enum freq_qos_req_type type;
	s32 value;
	int target_cpu;
};

static LIST_HEAD(active_qos_entries);
static DEFINE_SPINLOCK(qos_list_lock);

#define OPLUS_QOS_TEST		"qos_test"


static int input_min_or_max_param(enum QOS_OWNER owner, int value, int target_cpu,
								enum freq_qos_req_type freq_qos_type, int duration)
{
	struct cpufreq_policy *policy;
	struct qos_test_entry *entry;
	int ret;
	unsigned long flags;

	policy = cpufreq_cpu_get(target_cpu);

	if (!policy) {
		return -ESRCH;
	}

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		cpufreq_cpu_put(policy);
		return -ENOMEM;
	}

	entry->owner = owner;
	entry->value = value;
	entry->target_cpu = target_cpu;
	entry->policy = policy;
	entry->type = freq_qos_type;

	ret = oplus_add_freq_qos_request(owner, &policy->constraints, &entry->req, freq_qos_type, value);

	if (ret < 0) {
		kfree(entry);
		cpufreq_cpu_put(policy);
		return ret;
	}

	ret = oplus_update_freq_qos_request(owner, &entry->req, value);

	if (ret < 0) {
		oplus_remove_freq_qos_request(owner, &entry->req);
		kfree(entry);
		cpufreq_cpu_put(policy);
		return ret;
	}


	spin_lock_irqsave(&qos_list_lock, flags);
	list_add_tail(&entry->list, &active_qos_entries);
	spin_unlock_irqrestore(&qos_list_lock, flags);
	pr_err("%s, add to active_qos_entries complete. owner:%d value:%d cpu:%d\n", __func__, owner, value, target_cpu);

	return 0;
}

static ssize_t qos_test_begin_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[8];
	int err, val;

	memset(buffer, 0, sizeof(buffer));

	if (count > sizeof(buffer) - 1)
		count = sizeof(buffer) - 1;

	if (copy_from_user(buffer, buf, count))
		return -EFAULT;

	buffer[count] = '\n';
	err = kstrtoint(strstrip(buffer), 10, &val);
	if (err)
		return err;

	g_enable_qos_test = val;

	return count;
}


static ssize_t qos_test_begin_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[30];
	size_t len = 0;

	len = snprintf(buffer, sizeof(buffer), "qos_test_begin=%d\n", g_enable_qos_test);

	return simple_read_from_buffer(buf, count, ppos, buffer, len);
}

static ssize_t qos_test_input_write(struct file *file, const char __user *buf,
									size_t count, loff_t *ppos)
{
	char buffer[128], owner_str[16];
	int value, target_cpu;
	int min_or_max = 0, duration;
	enum QOS_OWNER owner;
	unsigned long flags;
	int ret = 0;
	enum freq_qos_req_type type;

	if (copy_from_user(buffer, buf, min_t(size_t, count, sizeof(buffer) - 1)))
		return -EFAULT;
	buffer[count] = '\0';

	if (sscanf(buffer, "%15s %d %d %d %d", owner_str, &value, &target_cpu, &min_or_max, &duration) != 5)
		return -EINVAL;

	if (strcmp(owner_str, "OMRG") == 0) {
		owner = QOS_OWNER_OMRG;
	} else if (strcmp(owner_str, "CB") == 0) {
		owner = QOS_OWNER_CB;
	} else if (strcmp(owner_str, "UAH") == 0) {
		owner = QOS_OWNER_UAH;
	} else if (strcmp(owner_str, "SBE") == 0) {
		owner = QOS_OWNER_SBE;
	} else {
		pr_err("%s, invalid owner name, (OMRG, CB, UAH, SBE) only\n",
						__func__);
		pr_err("%s, invalid owner name, (OMRG, CB, UAH, SBE) only\n", __func__);
		return -EFAULT;
	}

	if (target_cpu < 0 || target_cpu >= nr_cpu_ids) {
		pr_err("%s, invalid cpu ids:%d\n", __func__, target_cpu);
		pr_err("%s, invalid cpu ids:%d\n", __func__, target_cpu);
	}

	spin_lock_irqsave(&last_qos_input.lock, flags);
	last_qos_input.owner = owner;
	last_qos_input.value = value;
	last_qos_input.target_cpu = target_cpu;
	last_qos_input.valid = true;
	spin_unlock_irqrestore(&last_qos_input.lock, flags);

	type = min_or_max == 0 ? FREQ_QOS_MIN : FREQ_QOS_MAX;

	if (g_enable_qos_test) {
		/* if (owner == QOS_OWNER_SBE)
		 *     sbe_event_active_handle(gDuration);
		 * else
		 */
			ret = input_min_or_max_param(owner, value, target_cpu, type, duration);
	}

	return count;
}

static ssize_t qos_test_input_read(struct file *file, char __user *buf,
		size_t count, loff_t *ppos)
{
	char output[128];
	unsigned long flags;
	enum QOS_OWNER owner;
	int value, target_cpu, len;
	bool valid;
	const char *owner_str;

	spin_lock_irqsave(&last_qos_input.lock, flags);
	owner = last_qos_input.owner;
	value = last_qos_input.value;
	target_cpu = last_qos_input.target_cpu;
	valid = last_qos_input.valid;
	spin_unlock_irqrestore(&last_qos_input.lock, flags);

	if (!valid) {
		const char *msg = "No data written yet\n";

		return simple_read_from_buffer(buf, count, ppos, msg, strlen(msg));
	}

	switch (owner) {
	case QOS_OWNER_OMRG:
		owner_str = "OMRG";
		break;
	case QOS_OWNER_CB:
		owner_str = "CB";
		break;
	case QOS_OWNER_UAH:
		owner_str = "UAH";
		break;
	case QOS_OWNER_SBE:
		owner_str = "SBE";
		break;
	default:
		owner_str = "UNKNOWN";
	}

	len = snprintf(output, sizeof(output),
					  "Last added: owner=%s, value=%d, cpu=%d\n",
					  owner_str, value, target_cpu);
	return simple_read_from_buffer(buf, count, ppos, output, len);
}

static ssize_t qos_test_remove_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[128], owner_str[16];
	int owner, value, target_cpu;
	int found = 0;
	struct qos_test_entry *entry, *tmp;
	unsigned long flags;

	if (copy_from_user(buffer, buf, min_t(size_t, count, sizeof(buffer) - 1))) {
		return -EFAULT;
	}
	buffer[count] = '\0';

	if (sscanf(buffer, "%15s %d %d", owner_str, &value, &target_cpu) != 3)
		return -EFAULT;

	if (strcmp(owner_str, "OMRG") == 0) {
		owner = QOS_OWNER_OMRG;
	} else if (strcmp(owner_str, "CB") == 0) {
		owner = QOS_OWNER_CB;
	} else if (strcmp(owner_str, "UAH") == 0) {
		owner = QOS_OWNER_UAH;
	} else if (strcmp(owner_str, "SBE") == 0) {
		owner = QOS_OWNER_SBE;
	} else {
		pr_err("%s, invalid owner name, (OMRG, CB, UAH, SBE) only\n",
						__func__);
		pr_err("%s, invalid owner name, (OMRG, CB, UAH, SBE) only\n", __func__);
		return -EFAULT;
	}

	spin_lock_irqsave(&qos_list_lock, flags);
	list_for_each_entry_safe(entry, tmp, &active_qos_entries, list) {
		if (owner == entry->owner && entry->value == value && entry->target_cpu == target_cpu) {
			oplus_remove_freq_qos_request(owner, &entry->req);
			list_del(&entry->list);
			cpufreq_cpu_put(entry->policy);
			kfree(entry);
			found = 1;
			pr_err("%s, remove entry(owner:%d, value:%d, cpu:%d)\n",
						__func__, owner, value, target_cpu);
			break;
		}
	}
	spin_unlock_irqrestore(&qos_list_lock, flags);

	if (!found) {
		pr_err("%s, entry not found(owner:%d, value:%d, cpu:%d)\n",
					__func__, owner, value, target_cpu);
		return -EFAULT;
	}

	return count;
}

static ssize_t qos_test_update_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[128], owner_str[16];
	int owner, target_cpu, old_val, new_val;
	int found = 0;
	struct qos_test_entry *entry, *tmp;
	unsigned long flags;
	int ret = 0;

	if (copy_from_user(buffer, buf, min_t(size_t, count, sizeof(buffer) - 1))) {
		return -EFAULT;
	}
	buffer[count] = '\0';

	if (sscanf(buffer, "%15s %d %d %d", owner_str, &old_val, &new_val, &target_cpu) != 4)
		return -EFAULT;

	if (strcmp(owner_str, "OMRG") == 0) {
		owner = QOS_OWNER_OMRG;
	} else if (strcmp(owner_str, "CB") == 0) {
		owner = QOS_OWNER_CB;
	} else if (strcmp(owner_str, "UAH") == 0) {
		owner = QOS_OWNER_UAH;
	} else if (strcmp(owner_str, "SBE") == 0) {
		owner = QOS_OWNER_SBE;
	} else {
		pr_err("%s, invalid owner name, (OMRG, CB, UAH, SBE) only\n",
						__func__);
		pr_err("%s, invalid owner name, (OMRG, CB, UAH, SBE) only\n", __func__);
		return -EFAULT;
	}

	spin_lock_irqsave(&qos_list_lock, flags);
	list_for_each_entry_safe(entry, tmp, &active_qos_entries, list) {
		if (owner == entry->owner && entry->value == old_val && entry->target_cpu == target_cpu) {
			ret = oplus_update_freq_qos_request(owner, &entry->req, new_val);
			entry->value = new_val;
			found = 1;
			pr_err("%s, update entry(owner:%d, old_value:%d, new_value:%d, cpu:%d)\n",
						__func__, owner, old_val, new_val, target_cpu);
			break;
		}
	}
	spin_unlock_irqrestore(&qos_list_lock, flags);

	if (!found) {
		pr_err("%s, update entry not found(owner:%d, value:%d, cpu:%d)\n",
					__func__, owner, old_val, target_cpu);
		return -EFAULT;
	}

	return count;
}


static ssize_t qos_test_remove_all_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[128];
	int value;
	struct qos_test_entry *entry, *tmp;
	unsigned long flags;

	if (copy_from_user(buffer, buf, min_t(size_t, count, sizeof(buffer) - 1))) {
		return -EFAULT;
	}
	buffer[count] = '\0';

	if (sscanf(buffer, "%d", &value) != 1)
		return -EFAULT;

	if (value != 1)
		return -EFAULT;

	spin_lock_irqsave(&qos_list_lock, flags);
	list_for_each_entry_safe(entry, tmp, &active_qos_entries, list) {
		oplus_remove_freq_qos_request(entry->owner, &entry->req);
		list_del(&entry->list);
		cpufreq_cpu_put(entry->policy);
		kfree(entry);
		pr_err("%s, remove all entry end\n", __func__);
	}
	spin_unlock_irqrestore(&qos_list_lock, flags);

	return count;
}

static ssize_t qos_test_checklist_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[128];
	int value;

	if (copy_from_user(buffer, buf, min_t(size_t, count, sizeof(buffer) - 1))) {
		return -EFAULT;
	}
	buffer[count] = '\0';

	if (sscanf(buffer, "%d", &value) != 1)
		return -EFAULT;

	if (value != 1)
		return -EFAULT;

	/* dumplist(); */

	return count;
}

static ssize_t qos_set_duration_write(struct file *file, const char __user *buf,
		size_t count, loff_t *ppos)
{
	char buffer[128];
	int value;

	if (copy_from_user(buffer, buf, min_t(size_t, count, sizeof(buffer) - 1))) {
		return -EFAULT;
	}
	buffer[count] = '\0';

	if (sscanf(buffer, "%d", &value) != 1)
		return -EFAULT;

	if (value > 0)
		gDuration = value;

	return count;
}

static const struct proc_ops qos_test_input_fops = {
	.proc_write = qos_test_input_write,
	.proc_read = qos_test_input_read,
	.proc_lseek = default_llseek,
};

static const struct proc_ops qos_test_update_fops = {
	.proc_write = qos_test_update_write,
	.proc_lseek = default_llseek,
};

static const struct proc_ops qos_test_checklist_fops = {
	.proc_write = qos_test_checklist_write,
	.proc_lseek = default_llseek,
};

static const struct proc_ops qos_set_duration_fops = {
	.proc_write = qos_set_duration_write,
	.proc_lseek = default_llseek,
};

static const struct proc_ops qos_test_remove_fops = {
	.proc_write = qos_test_remove_write,
	.proc_lseek = default_llseek,
};

static const struct proc_ops qos_test_remove_all_fops = {
	.proc_write = qos_test_remove_all_write,
	.proc_lseek = default_llseek,
};

static const struct proc_ops qos_test_begin_fops = {
	.proc_write		= qos_test_begin_write,
	.proc_read		= qos_test_begin_read,
	.proc_lseek		= default_llseek,
};


int qos_test_proc_init(void)
{
	struct proc_dir_entry *proc_node;

	d_qos_entry = proc_mkdir(OPLUS_QOS_TEST, NULL);
	if (!d_qos_entry) {
		pr_err("failed to create proc dir qos_test\n");
	}

	proc_node = proc_create("qos_test_begin", 0666, d_qos_entry, &qos_test_begin_fops);
	if (!proc_node) {
		pr_err("failed to create proc node qos_test_begin\n");
		remove_proc_entry("qos_test_begin", d_qos_entry);
		return -ENOMEM;
	}

	proc_node = proc_create("qos_test_input", 0644, d_qos_entry, &qos_test_input_fops);
	if (!proc_node) {
		pr_err("Failed to create qos_test_input\n");
		remove_proc_entry("qos_test_input", d_qos_entry);
		return -ENOMEM;
	}

	proc_node = proc_create("qos_test_remove", 0644, d_qos_entry, &qos_test_remove_fops);
	if (!proc_node) {
		pr_err("Failed to create qos_test_remove\n");
		remove_proc_entry("qos_test_remove", d_qos_entry);
		return -ENOMEM;
	}

	proc_node = proc_create("qos_test_remove_all", 0644, d_qos_entry, &qos_test_remove_all_fops);
	if (!proc_node) {
		pr_err("Failed to create qos_test_remove_all\n");
		remove_proc_entry("qos_test_remove_all", d_qos_entry);
		return -ENOMEM;
	}

	proc_node = proc_create("qos_test_update", 0644, d_qos_entry, &qos_test_update_fops);
	if (!proc_node) {
		pr_err("Failed to create qos_test_update\n");
		remove_proc_entry("qos_test_update", d_qos_entry);
		return -ENOMEM;
	}

	proc_node = proc_create("qos_test_checklist", 0644, d_qos_entry, &qos_test_checklist_fops);
	if (!proc_node) {
		pr_err("Failed to create qos_test_checklist\n");
		remove_proc_entry("qos_test_checklist", d_qos_entry);
		return -ENOMEM;
	}

	proc_node = proc_create("qos_set_duration", 0644, d_qos_entry, &qos_set_duration_fops);
	if (!proc_node) {
		pr_err("Failed to create qos_set_duration\n");
		remove_proc_entry("qos_set_duration", d_qos_entry);
		return -ENOMEM;
	}

	pr_info("qos_test_proc_init done\n");

	return 0;
}

module_init(qos_test_proc_init);
MODULE_LICENSE("GPL v2");
