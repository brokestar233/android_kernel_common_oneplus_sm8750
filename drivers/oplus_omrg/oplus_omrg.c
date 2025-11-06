// SPDX-License-Identifier: GPL-2.0-only
/*
 * oplus_omrg.c
 *
 * function of oplus_omrg module
 *
 * Copyright (c) 2020-2022 Oplus. All rights reserved.
 *
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/notifier.h>
#include <linux/topology.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/version.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/devfreq.h>
#include <linux/cpu_pm.h>
#include <linux/platform_device.h>
#include <linux/kthread.h>
#include <linux/pm_qos.h>
#include <linux/irq_work.h>

#define CREATE_TRACE_POINTS
#include "oplus_omrg_trace.h"

#include <linux/oplus_omrg.h>
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_QOS_ARBITER)
#include <linux/qos_arbiter.h>
#endif


#define CPUFREQ_AMP	1000UL
#define DEVFREQ_AMP	1UL
#define KHZ		1000UL
#define INVALID_STATE	(-1)
#define IDLE_UPDATE_SLACK_TIME 20

struct omrg_freq_device {
	/* copy of cpufreq device's related cpus */
	cpumask_t cpus;
	/* cpufreq policy related with this device */
	struct cpufreq_policy *policy;
	/* driver pointer of devfreq device */
	struct devfreq *df;
	/* device node for binding omrg_device */
	struct device_node *np;
	/* work used to unbind omrg_device */
	struct kthread_work destroy_work;
	/* list of all binded master omrg_device */
	struct list_head master_list;
	/* list of all binded slave omrg_device */
	struct list_head slave_list;
	/* list node for g_omrg_cpufreq_list or g_omrg_devfreq_list */
	struct list_head node;
	/* amplifer used to convert device input freq to Hz */
	unsigned int amp;
	unsigned long cur_freq;
	unsigned long cpuinfo_min_freq;
	unsigned long cpuinfo_max_freq;
	unsigned long prev_max_freq;
	unsigned long prev_min_freq;
	/* used to send request to cpufreq pm qos */
	struct freq_qos_request min_qos_req;
	struct freq_qos_request max_qos_req;
	struct rcu_head rcu;
	struct notifier_block nb_max;
};

struct omrg_rule;

struct omrg_device {
	/* binded omrg_freq_device */
	struct omrg_freq_device *freq_dev;
	/* device node parsed from phandle for binding omrg freq device */
	struct device_node *np;
	/* the omrg rule this device belong to */
	struct omrg_rule *ruler;
	/* range divider freq, described as KHz */
	unsigned int *divider;
	bool *divider_dispense;
	/* list node for omrg freq device's master list or slave list */
	struct list_head node;
	/* minimum freq, set by omrg rule */
	unsigned long min_freq;
	/* maximum freq, set by omrg rule */
	unsigned long max_freq;
	/* thermal clip state for slave, cur state for master */
	int state;
	bool idle;
};

struct omrg_rule {
	struct kobject kobj;
	/* work used to notify slave device the freq range has changed */
	struct kthread_work exec_work;
	struct kthread_work *coop_work;
	bool work_in_progress;
	bool state_alter_pending;
	struct irq_work irq_work;
	/* spinlock to protect work_in_progress */
	spinlock_t wip_lock;
	/* list node for g_omrg_rule_list */
	struct list_head node;
	/* all master device belong to this rule */
	struct omrg_device *master_dev;
	/* all slave device belong to this rule */
	struct omrg_device *slave_dev;
	/* rule name */
	const char *name;
	/* divider number */
	int divider_num;
	/* master device number */
	int master_num;
	/* slave device number */
	int slave_num;
	/* current freq range index */
	int state_idx;
	/* limit slave's minimum freq */
	bool dn_limit;
	/* limit slave's maximum freq */
	bool up_limit;
	/* be aware of slave's thermal clamp */
	bool thermal_enable;
	/* if rule have multi master, use maximum state of these master */
	bool master_vote_max;
	/* dynamic switch setting in different scenario, default false */
	bool user_mode_enable;
	bool enable;
};

static LIST_HEAD(g_omrg_rule_list);
static LIST_HEAD(g_omrg_cpufreq_list);
static LIST_HEAD(g_omrg_devfreq_list);
static DEFINE_SPINLOCK(omrg_list_lock);
static struct kthread_worker g_omrg_worker;
static struct task_struct *g_omrg_work_thread;
static bool g_omrg_initialized;

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CEILING_FREE)
static atomic_t is_omrg_ceiling_free;
#endif

static void omrg_irq_work(struct irq_work *w)
{
	struct omrg_rule *ruler = container_of(w, struct omrg_rule, irq_work);
	kthread_queue_work(&g_omrg_worker, ruler->coop_work ?: &ruler->exec_work);
}

static struct omrg_freq_device *omrg_get_cpu_dev(unsigned int cpu)
{
	struct omrg_freq_device *freq_dev = NULL;

	/* find the freq device of cpu */
	list_for_each_entry_rcu(freq_dev, &g_omrg_cpufreq_list, node) {
		if (cpumask_test_cpu(cpu, &freq_dev->cpus))
			return freq_dev;
	}

	return NULL;
}

static struct omrg_freq_device *omrg_get_devfreq_dev(struct devfreq *df)
{
	struct omrg_freq_device *freq_dev = NULL;

	/* find the freq device of devfreq */
	list_for_each_entry_rcu(freq_dev, &g_omrg_devfreq_list, node) {
		if (freq_dev->df == df)
			return freq_dev;
	}

	return NULL;
}

static int omrg_freq2state(struct omrg_rule *ruler,
			  unsigned int *divider, unsigned long freq)
{
	int i;
	int state = 0;

	/* convert freq to freq range index according to divider */
	for (i = 0; i < ruler->divider_num; i++) {
		if (freq >= divider[i])
			state++;
	}

	return state;
}

static int find_proper_upbound(struct omrg_freq_device *freq_dev,
			       unsigned long *freq)
{
	/*struct device *dev = NULL;
	struct dev_pm_opp *opp = NULL;
	unsigned int cpu = cpumask_first(&freq_dev->cpus);
	struct cpufreq_policy *policy = NULL;
	int idx;

	policy = cpufreq_cpu_get(cpu);
	if (!policy)
		return -ESRCH;

	idx = cpufreq_frequency_table_target(policy, *freq / KHZ,
							CPUFREQ_RELATION_H);

	*freq = policy->freq_table[idx].frequency * KHZ;

	cpufreq_cpu_put(policy);*/

	/*if (cpu < (unsigned int)nr_cpu_ids)
		dev = get_cpu_device(cpu);
	else if (!IS_ERR_OR_NULL(freq_dev->df))
		dev = freq_dev->df->dev.parent;
	else
		dev = NULL;

	if (IS_ERR_OR_NULL(dev))
		return -ENODEV;

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0))
	rcu_read_lock();
	opp = dev_pm_opp_find_freq_floor(dev, freq);
	if (IS_ERR(opp)) {
		rcu_read_unlock();
		return -ENODEV;
	}
	rcu_read_unlock();
#else
	opp = dev_pm_opp_find_freq_floor(dev, freq);
	if (IS_ERR(opp))
		return -ENODEV;
	dev_pm_opp_put(opp);
#endif*/

	return 0;
}

static unsigned long omrg_check_thermal_limit(struct omrg_device *master,
					     unsigned long new_freq)
{
	struct omrg_rule *ruler = master->ruler;
	struct omrg_device *slave = NULL;
	unsigned long ret = new_freq;
	int i, state;
	int thermal_state = INT_MAX;
	int thermal_slave = 0;

	if (!ruler->thermal_enable)
		return ret;

	/* find minimum thermal clamp freq range index */
	for (i = 0; i < ruler->slave_num; i++) {
		slave = &ruler->slave_dev[i];
		if (slave->state >= 0 &&
		    slave->state < thermal_state) {
			thermal_state = slave->state;
			thermal_slave = i;
		}
	}

	/* get the freq range index determined by master freq */
	state = omrg_freq2state(ruler, master->divider, new_freq);

	/*
	 * if master's freq range index is higher than slave's thermal
	 * clamp freq range index, it means slave cannot sync freq range
	 * with master, if they have strict constraints, master's freq must
	 * be limited to ensure it will not break the rule.
	 */
	if (state > thermal_state) {
		/*
		 * find a proper freq for master according to
		 * thermal clamp freq range index
		 */
		ret = (master->divider[thermal_state] - 1) * KHZ;
		if (find_proper_upbound(master->freq_dev, &ret))
			ret /= KHZ;
		else
			ret = new_freq;

		trace_omrg_thermal_limit(ruler->name, thermal_slave,
					thermal_state, state, new_freq, ret);
	}

	return ret;
}

static int omrg_rule_get_master_state(const struct omrg_rule *ruler)
{
	int i, tmp, stt;
	int state = INVALID_STATE;

	if (!ruler->enable)
		return state;

	for (i = 0; i < ruler->master_num; i++) {
		stt = ruler->master_dev[i].state;
		tmp = ruler->master_dev[i].idle ?
		      0 : (stt < ruler->divider_num && ruler->master_dev[i].divider_dispense[stt]) ?
			ruler->divider_num : stt;
		if (tmp < 0)
			continue;

		if (state < 0 ||
		    (ruler->master_vote_max && tmp > state) ||
		    (!ruler->master_vote_max && tmp < state))
			state = tmp;
	}

	return state;
}

static void omrg_get_slave_freq_range(struct omrg_freq_device *freq_dev,
				     unsigned long *min_freq,
				     unsigned long *max_freq)
{
	struct omrg_rule *ruler = NULL;
	struct omrg_device *slave = NULL;
	unsigned long rule_min, rule_max;
	int state;

	*min_freq = 0;
	*max_freq = ULONG_MAX;

	list_for_each_entry(slave, &freq_dev->slave_list, node) {
		ruler = slave->ruler;
		state = omrg_rule_get_master_state(ruler);

		if (!ruler->dn_limit || state <= 0)
			rule_min = 0;
		else
			rule_min = slave->divider[state - 1] * KHZ;

		if (!ruler->up_limit || state == ruler->divider_num || state < 0)
			rule_max = ULONG_MAX;
		else
			rule_max = (slave->divider[state]) * KHZ;

		if (rule_min < freq_dev->cpuinfo_min_freq)
			rule_min = freq_dev->cpuinfo_min_freq;

		if (rule_max > freq_dev->cpuinfo_max_freq)
			rule_max = freq_dev->cpuinfo_max_freq;

		find_proper_upbound(freq_dev, &rule_max);

		slave->min_freq = rule_min;
		slave->max_freq = rule_max;
		trace_omrg_update_freq_range(ruler->name, 0, state, rule_min,
					    rule_max);

		/*
		 * the priority of the rule is determined by
		 * the order defined in dts, so the followed freq
		 * range cannot be large than pervious
		 */
		*max_freq = clamp(slave->max_freq, *min_freq, *max_freq);
		*min_freq = clamp(slave->min_freq, *min_freq, *max_freq);
	}
}

static void omrg_cpufreq_apply_limits(unsigned int cpu)
{
	struct omrg_freq_device *freq_dev = NULL;
	unsigned long max_freq = ULONG_MAX;
	unsigned long min_freq = 0;
	int ret;
	if (!g_omrg_initialized)
		return;

	rcu_read_lock();
	freq_dev = omrg_get_cpu_dev(cpu);
	if (IS_ERR_OR_NULL(freq_dev))
		goto unlock;

	omrg_get_slave_freq_range(freq_dev, &min_freq, &max_freq);

	max_freq /= freq_dev->amp;
	min_freq /= freq_dev->amp;

	if (max_freq == 0)
		goto unlock;

	if (max_freq == freq_dev->prev_max_freq &&
			min_freq == freq_dev->prev_min_freq)
		goto unlock;

	freq_dev->prev_max_freq = max_freq;
	freq_dev->prev_min_freq = min_freq;

	rcu_read_unlock();

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CEILING_FREE)
	if (atomic_read(&is_omrg_ceiling_free)) {
		min_freq = FREQ_QOS_MIN_DEFAULT_VALUE;
		max_freq = FREQ_QOS_MAX_DEFAULT_VALUE;
	}
#endif
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_QOS_ARBITER)
	ret = oplus_update_freq_qos_request(QOS_OWNER_OMRG, &freq_dev->min_qos_req, min_freq);
#else
	ret = freq_qos_update_request(&freq_dev->min_qos_req, min_freq);
#endif /* CONFIG_OPLUS_FEATURE_CPU_QOS_ARBITER */
	if (ret < 0)
		pr_err("fail to update min freq for CPU%d ret=%d\n",
						cpu, ret);

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_QOS_ARBITER)
	ret = oplus_update_freq_qos_request(QOS_OWNER_OMRG, &freq_dev->max_qos_req, max_freq);
#else
	ret = freq_qos_update_request(&freq_dev->max_qos_req, max_freq);
#endif /* CONFIG_OPLUS_FEATURE_CPU_QOS_ARBITER */
	if (ret < 0)
		pr_err("fail to update max freq for CPU%d ret=%d\n",
						cpu, ret);

	trace_omrg_cpu_policy_adjust(cpu, MAX_OMRG_MARGIN,
						min_freq, max_freq);
	return;

unlock:
	rcu_read_unlock();
}

static void omrg_rule_update_work(struct kthread_work *work)
{
	struct omrg_rule *ruler = container_of(work, struct omrg_rule, exec_work);
	struct omrg_device *slave = NULL;
	unsigned int cpu;
	int i;
	unsigned long flags;

	cpus_read_lock();
	for (i = 0; i < ruler->slave_num; i++) {
		slave = &ruler->slave_dev[i];
		if (IS_ERR_OR_NULL(slave->freq_dev))
			continue;

		cpu = (unsigned int)cpumask_any_and(&slave->freq_dev->cpus,
						    cpu_online_mask);
		if (cpu < (unsigned int)nr_cpu_ids) {
			omrg_cpufreq_apply_limits(cpu);
			continue;
		}

		/*if (slave->freq_dev->df)
			devfreq_apply_limits(slave->freq_dev->df);*/
	}
	cpus_read_unlock();
	spin_lock_irqsave(&ruler->wip_lock, flags);
	ruler->work_in_progress = false;
	spin_unlock_irqrestore(&ruler->wip_lock, flags);
}

static inline void omrg_irq_work_queue(struct irq_work *work)
{
	if (likely(cpu_online(raw_smp_processor_id())))
		irq_work_queue(work);
	else
		irq_work_queue_on(work, cpumask_any(cpu_online_mask));
}

static void omrg_update_state(struct omrg_rule *ruler, bool force)
{
	unsigned long flags;
	ruler->state_idx = omrg_rule_get_master_state(ruler);

	if (!ruler->enable)
		return;

	if (!force && ruler->coop_work != NULL)
		return;

	spin_lock_irqsave(&ruler->wip_lock, flags);
	if (ruler->work_in_progress) {
		ruler->state_alter_pending = true;
		spin_unlock_irqrestore(&ruler->wip_lock, flags);
		return;
	}
	ruler->work_in_progress = true;
	ruler->state_alter_pending = false;
	/*
	 * omrg_rule_update_work will update other cpufreq/devfreq's freq,
	 * throw it to a global worker can avoid function reentrant and
	 * other potential race risk
	 */
	omrg_irq_work_queue(&ruler->irq_work);

	spin_unlock_irqrestore(&ruler->wip_lock, flags);
}

static void omrg_refresh_all_rule(void)
{
	struct omrg_rule *ruler = NULL;

	if (!g_omrg_initialized)
		return;

	list_for_each_entry(ruler, &g_omrg_rule_list, node)
		omrg_update_state(ruler, false);
}

static void omrg_coop_work_update(struct omrg_freq_device *freq_dev,
				 struct omrg_device *work_dev, unsigned long freq)
{
	struct omrg_device *master = NULL;
	int old_state;
	int update = 0;
	struct omrg_rule *ruler = work_dev->ruler;

	/* find all related coop work and check if they need an update */
	list_for_each_entry(master, &freq_dev->master_list, node) {
		if (master->ruler->coop_work == &work_dev->ruler->exec_work) {
			old_state = master->state;
			master->state = omrg_freq2state(master->ruler,
						       master->divider, freq);
			if (master->state != old_state)
				update = 1;
		}
	}

	old_state = work_dev->state;
	work_dev->state = omrg_freq2state(ruler, work_dev->divider, freq);

	if (ruler->state_idx != omrg_rule_get_master_state(ruler) ||
			ruler->state_alter_pending)
		update = 1;

	if (work_dev->state != old_state || update)
		omrg_update_state(ruler, true);
}

static unsigned long omrg_freq_check_limit(struct omrg_freq_device *freq_dev,
					  unsigned long target_freq)
{
	struct omrg_device *master = NULL;
	unsigned long ret;
	unsigned long min_freq = 0;
	unsigned long max_freq = ULONG_MAX;
	int cpu;

	if (!g_omrg_initialized) {
		freq_dev->cur_freq = target_freq * freq_dev->amp / KHZ;
		return target_freq;
	}

	/*
	 * first check if this device is slave of some rules, its freq
	 * must fit into the freq range decided by master
	 */
	omrg_get_slave_freq_range(freq_dev, &min_freq, &max_freq);

	ret = clamp(target_freq * freq_dev->amp, min_freq, max_freq) / KHZ;
	cpu = freq_dev->df ? 0 : cpumask_first(&freq_dev->cpus);

	/*
	 * second check if this device is master of some rules, its freq
	 * will not break the rule due to slave's thermal clamp
	 */
	list_for_each_entry(master, &freq_dev->master_list, node)
		ret = omrg_check_thermal_limit(master, ret);

	/* get current freq range index and notice all related slaves */
	list_for_each_entry(master, &freq_dev->master_list, node) {
		/* coop work checked by its related work */
		if (master->ruler->coop_work != NULL)
			continue;

		omrg_coop_work_update(freq_dev, master, ret);
	}

	freq_dev->cur_freq = ret;
	trace_omrg_check_limit(freq_dev->np->name, cpu, target_freq,
			      ret * KHZ / freq_dev->amp, min_freq, max_freq);

	return ret * KHZ / freq_dev->amp;
}

/*
 * omrg_cpufreq_check_limit() - Return freq clamped by omrg
 * @policy: the cpufreq policy of the freq domain
 * @target_freq: the target freq determined by cpufreq governor
 *
 * check this policy's freq device, if it is slave of any omrg rule, make sure
 * target_freq will never exceed omrg freq range, if it is master of any omrg
 * rule, notify other slave device of this rule if this master has changed
 * freq range.
 *
 * Return: frequency.
 *
 * Locking: This function must be called under policy->rwsem. cpufreq core
 * ensure this function will not be called when unregistering the freq device
 * of this policy.
 */
unsigned int omrg_cpufreq_check_limit(struct cpufreq_policy *policy,
				     unsigned int target_freq)
{
	struct omrg_freq_device *freq_dev = NULL;

	if (IS_ERR_OR_NULL(policy))
		return target_freq;

	rcu_read_lock();
	freq_dev = omrg_get_cpu_dev(policy->cpu);
	if (IS_ERR_OR_NULL(freq_dev))
		goto unlock;

	if (list_empty(&freq_dev->master_list) &&
		list_empty(&freq_dev->slave_list))
		goto unlock;

	target_freq = omrg_freq_check_limit(freq_dev, target_freq);

unlock:
	rcu_read_unlock();
	return target_freq;
}
EXPORT_SYMBOL_GPL(omrg_cpufreq_check_limit);

/*
 * omrg_devfreq_check_limit() - Return freq clamped by omrg
 * @df: the devfreq device
 * @target_freq: the target freq determined by devfreq governor
 *
 * check this devfreq's freq device, if it is slave of any omrg rule, make sure
 * target_freq will never exceed omrg freq range, if it is master of any omrg
 * rule, notify other slave device of this rule if this master has changed
 * freq range.
 *
 * Return: frequency.
 *
 * Locking: This function must be called under df->lock. This lock will
 * protect against unregistering the freq device of this devfreq.
 */
unsigned long omrg_devfreq_check_limit(struct devfreq *df,
				      unsigned long target_freq)
{
	struct omrg_freq_device *freq_dev = NULL;

	if (IS_ERR_OR_NULL(df))
		return target_freq;

	rcu_read_lock();
	freq_dev = omrg_get_devfreq_dev(df);
	if (IS_ERR_OR_NULL(freq_dev))
		goto unlock;

	target_freq = omrg_freq_check_limit(freq_dev, target_freq);

unlock:
	rcu_read_unlock();
	return target_freq;
}

static void omrg_update_clip_state(struct omrg_freq_device *freq_dev,
				  unsigned long target_freq)
{
	struct omrg_device *slave = NULL;

	if (!g_omrg_initialized)
		return;

	list_for_each_entry(slave, &freq_dev->slave_list, node)
		slave->state = omrg_freq2state(slave->ruler,
					      slave->divider, target_freq);
}

/*
 * omrg_cpufreq_cooling_update()
 * @cpu: cpu number
 * @clip_freq: the clip freq determined by cpu cooling device
 *
 * convert clip freq to freq range index for each related slave of any omrg
 * rule.
 *
 * Locking: cpufreq dirver should ensure cooling device will be unregistered
 * before omrg freq device being unregistered.
 */
void omrg_cpufreq_cooling_update(unsigned int cpu, unsigned int clip_freq)
{
	struct omrg_freq_device *freq_dev = NULL;

	/*
	 * the cooling device we used is not the one registered
	 * in cpufreq-dt, so this interface may be called when
	 * we hotplug cpu, in order to prevent freq_dev being
	 * accessed when cpufreq unregister this device in hotplug,
	 * hold hotplug lock in this interface
	 */
	cpus_read_lock();
	rcu_read_lock();
	freq_dev = omrg_get_cpu_dev(cpu);
	if (!IS_ERR_OR_NULL(freq_dev))
		omrg_update_clip_state(freq_dev, clip_freq);

	rcu_read_unlock();
	cpus_read_unlock();
}

/*
 * omrg_devfreq_cooling_update()
 * @df: devfreq device
 * @clip_freq: the clip freq determined by cpu cooling device
 *
 * convert clip freq to freq range index for each related slave of any omrg
 * rule.
 *
 * Locking: devfreq dirver should ensure cooling device will be unregistered
 * before omrg freq device being unregistered.
 */
void omrg_devfreq_cooling_update(struct devfreq *df, unsigned long clip_freq)
{
	struct omrg_freq_device *freq_dev = NULL;

	rcu_read_lock();
	freq_dev = omrg_get_devfreq_dev(df);
	if (!IS_ERR_OR_NULL(freq_dev))
		omrg_update_clip_state(freq_dev, clip_freq / KHZ);
	rcu_read_unlock();
}

static void _of_match_omrg_rule(struct omrg_freq_device *freq_dev,
			       struct omrg_rule *ruler)
{
	struct omrg_device *tmp_dev = NULL;
	int i;

	for (i = 0; i < ruler->master_num; i++) {
		if (ruler->master_dev[i].np == freq_dev->np) {
			tmp_dev = &ruler->master_dev[i];
			list_add_tail(&tmp_dev->node, &freq_dev->master_list);
			tmp_dev->freq_dev = freq_dev;
			tmp_dev->state = omrg_freq2state(ruler, tmp_dev->divider,
							freq_dev->cur_freq);
			pr_debug("%s:match master%d %s\n",
				 ruler->name, i, freq_dev->np->full_name);
			return;
		}
	}

	for (i = 0; i < ruler->slave_num; i++) {
		if (ruler->slave_dev[i].np == freq_dev->np) {
			list_add_tail(&ruler->slave_dev[i].node,
				      &freq_dev->slave_list);
			ruler->slave_dev[i].freq_dev = freq_dev;
			pr_debug("%s:match slave%d %s\n",
				 ruler->name, i, freq_dev->np->full_name);
			return;
		}
	}
}

static void of_match_omrg_rule(struct omrg_freq_device *freq_dev)
{
	struct omrg_rule *ruler = NULL;

	list_for_each_entry(ruler, &g_omrg_rule_list, node)
		_of_match_omrg_rule(freq_dev, ruler);
}

static void of_match_omrg_freq_device(struct omrg_rule *ruler)
{
	struct omrg_freq_device *freq_dev = NULL;

	rcu_read_lock();
	list_for_each_entry_rcu(freq_dev, &g_omrg_cpufreq_list, node)
		_of_match_omrg_rule(freq_dev, ruler);

	list_for_each_entry_rcu(freq_dev, &g_omrg_devfreq_list, node)
		_of_match_omrg_rule(freq_dev, ruler);
	rcu_read_unlock();
}

static int omrg_notifier_max(struct notifier_block *nb, unsigned long freq,
				void *data)
{
	struct omrg_freq_device *freq_dev = NULL;
	struct omrg_device *master = NULL;
	int i, ret = NOTIFY_DONE;

	rcu_read_lock();
	freq_dev = container_of(nb, struct omrg_freq_device, nb_max);
	if (!freq_dev || !freq_dev->policy)
		goto unlock;

	if (list_empty(&freq_dev->master_list))
		goto unlock;

	list_for_each_entry(master, &freq_dev->master_list, node) {
		for (i = 0; i < master->ruler->divider_num; i++) {
			if (freq <= master->divider[i])
				master->divider_dispense[i] = true;
			else
				master->divider_dispense[i] = false;
		}
	}
	ret = NOTIFY_OK;

unlock:
	rcu_read_unlock();

	return ret;
}

static void omrg_policy_create_callback(struct cpufreq_policy *policy)
{
	struct omrg_freq_device *freq_dev = NULL;
	struct device *cpu_dev = NULL;
	int ret;
	unsigned long flags;

	if (IS_ERR_OR_NULL(policy)) {
		pr_err("%s:null cpu policy\n", __func__);
		return;
	}

	cpu_dev = get_cpu_device(policy->cpu);
	if (IS_ERR_OR_NULL(cpu_dev)) {
		pr_err("%s:null cpu device\n", __func__);
		return;
	}

	spin_lock_irqsave(&omrg_list_lock, flags);

	list_for_each_entry_rcu(freq_dev, &g_omrg_cpufreq_list, node) {
		if (cpumask_subset(policy->related_cpus, &freq_dev->cpus)) {
			spin_unlock_irqrestore(&omrg_list_lock, flags);
			goto unlock;
		}
	}

	freq_dev = (struct omrg_freq_device *)
			kzalloc(sizeof(*freq_dev), GFP_ATOMIC);
	if (IS_ERR_OR_NULL(freq_dev)) {
		pr_err("alloc cpu%d freq dev err\n", policy->cpu);
		spin_unlock_irqrestore(&omrg_list_lock, flags);
		goto unlock;
	}

	cpumask_copy(&freq_dev->cpus, policy->related_cpus);
	freq_dev->np = cpu_dev->of_node;
	freq_dev->amp = CPUFREQ_AMP;
	freq_dev->cur_freq = policy->cur * CPUFREQ_AMP / KHZ;
	freq_dev->cpuinfo_min_freq = policy->cpuinfo.min_freq * KHZ;
	freq_dev->cpuinfo_max_freq = policy->cpuinfo.max_freq * KHZ;
	freq_dev->prev_max_freq = policy->max;
	freq_dev->prev_min_freq = policy->min;
	INIT_LIST_HEAD(&freq_dev->master_list);
	INIT_LIST_HEAD(&freq_dev->slave_list);

	freq_dev->policy = cpufreq_cpu_get(policy->cpu);

	if (g_omrg_initialized)
		of_match_omrg_rule(freq_dev);

	list_add_tail_rcu(&freq_dev->node, &g_omrg_cpufreq_list);

	spin_unlock_irqrestore(&omrg_list_lock, flags);

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_QOS_ARBITER)
	ret = oplus_add_freq_qos_request(QOS_OWNER_OMRG, &policy->constraints,
					&freq_dev->min_qos_req, FREQ_QOS_MIN, policy->cpuinfo.min_freq);
#else
	ret = freq_qos_add_request(&policy->constraints, &freq_dev->min_qos_req,
					FREQ_QOS_MIN, policy->cpuinfo.min_freq);
#endif /* CONFIG_OPLUS_FEATURE_CPU_QOS_ARBITER */
	if (ret < 0) {
		pr_err("%s: Failed to add min freq constraint (%d)\n", __func__,
				ret);
		goto free_freq_dev;
	}

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_QOS_ARBITER)
	ret = oplus_add_freq_qos_request(QOS_OWNER_OMRG, &policy->constraints,
					&freq_dev->max_qos_req, FREQ_QOS_MAX,
					policy->cpuinfo.max_freq);
#else
	ret = freq_qos_add_request(&policy->constraints,
					&freq_dev->max_qos_req, FREQ_QOS_MAX,
					policy->cpuinfo.max_freq);
#endif /* CONFIG_OPLUS_FEATURE_CPU_QOS_ARBITER */

	if (ret < 0) {
		pr_err("%s: Failed to add max freq constraint (%d)\n", __func__,
				ret);
		goto remove_min_qos;
	}


	freq_dev->nb_max.notifier_call = omrg_notifier_max;
	/* may sleep */
	ret = freq_qos_add_notifier(&policy->constraints,
					FREQ_QOS_MAX, &freq_dev->nb_max);
	if (ret) {
		pr_err("%s: Failed to add qos max notifier (%d)\n", __func__,
				ret);
		goto remove_max_qos;
	}

	omrg_refresh_all_rule();

	return;

remove_max_qos:
	freq_qos_remove_request(&freq_dev->max_qos_req);
remove_min_qos:
	freq_qos_remove_request(&freq_dev->min_qos_req);
free_freq_dev:
	spin_lock_irqsave(&omrg_list_lock, flags);
	list_del_rcu(&freq_dev->node);
	spin_unlock_irqrestore(&omrg_list_lock, flags);
	kfree(freq_dev);
unlock:
	omrg_refresh_all_rule();
}

static inline void omrg_freq_dev_detach(struct omrg_freq_device *freq_dev)
{
	struct omrg_device *omrg_dev, *tmp;

	list_for_each_entry_safe(omrg_dev, tmp, &freq_dev->slave_list, node) {
		omrg_dev->freq_dev = NULL;
		omrg_dev->state = INVALID_STATE;
		list_del(&omrg_dev->node);
	}

	list_for_each_entry_safe(omrg_dev, tmp, &freq_dev->master_list, node) {
		omrg_dev->freq_dev = NULL;
		omrg_dev->state = INVALID_STATE;
		list_del(&omrg_dev->node);
		omrg_update_state(omrg_dev->ruler, false);
	}
}

static void omrg_kfree_freq_dev(struct rcu_head *rcu)
{
	unsigned long flags;
	struct omrg_freq_device *freq_dev;

	spin_lock_irqsave(&omrg_list_lock, flags);
	freq_dev = container_of(rcu, struct omrg_freq_device, rcu);

	omrg_freq_dev_detach(freq_dev);
	if (freq_dev->policy) {
		cpufreq_cpu_put(freq_dev->policy);
		freq_dev->policy = NULL;
	}
	kfree(freq_dev);
	spin_unlock_irqrestore(&omrg_list_lock, flags);
}

static void omrg_policy_remove_callback(struct cpufreq_policy *policy)
{
	struct omrg_freq_device *freq_dev = NULL;
	bool found = false;
	unsigned long flags;

	spin_lock_irqsave(&omrg_list_lock, flags);
	list_for_each_entry_rcu(freq_dev, &g_omrg_cpufreq_list, node) {
		if (cpumask_test_cpu(policy->cpu, &freq_dev->cpus)) {
			found = true;
			list_del_rcu(&freq_dev->node);
			break;
		}
	}
	spin_unlock_irqrestore(&omrg_list_lock, flags);

	if (found) {
		/* may sleep */
		freq_qos_remove_request(&freq_dev->max_qos_req);
		freq_qos_remove_request(&freq_dev->min_qos_req);

		/* may sleep */
		freq_qos_remove_notifier(&freq_dev->policy->constraints,
			FREQ_QOS_MAX, &freq_dev->nb_max);
		call_rcu(&freq_dev->rcu, omrg_kfree_freq_dev);
	}
}


static int omrg_policy_nitifier_callback(struct notifier_block *nb,
						unsigned long val, void *data)
{
	struct cpufreq_policy *policy = (struct cpufreq_policy *)data;

	if (IS_ERR_OR_NULL(policy)) {
		pr_err("%s:null cpu policy\n", __func__);
		return NOTIFY_DONE;
	}

	switch (val) {
	case CPUFREQ_CREATE_POLICY:
		omrg_policy_create_callback(policy);
		break;
	case CPUFREQ_REMOVE_POLICY:
		omrg_policy_remove_callback(policy);
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block omrg_cpufreq_policy_notifier = {
	.notifier_call = omrg_policy_nitifier_callback,
};


/*
 * omrg_devfreq_register()
 * @df: devfreq device
 *
 * register omrg freq device for the devfreq device, match it with related omrg
 * device of all rules. omrg_refresh_all_rule will be called to make sure no
 * violation of rule will happen after adding this device.
 *
 * Locking: this function will hold omrg_list_lock during registration, devfreq
 * cooling device should be registered after omrg freq device registration.
 */
void omrg_devfreq_register(struct devfreq *df)
{
	struct omrg_freq_device *freq_dev = NULL;
	struct device *dev = NULL;

	if (IS_ERR_OR_NULL(df)) {
		pr_err("%s:null devfreq\n", __func__);
		return;
	}

	dev = df->dev.parent;
	if (IS_ERR_OR_NULL(dev)) {
		pr_err("%s:null devfreq parent dev\n", __func__);
		return;
	}

	spin_lock(&omrg_list_lock);
	list_for_each_entry_rcu(freq_dev, &g_omrg_devfreq_list, node) {
		if (freq_dev->df == df || freq_dev->np == dev->of_node)
			goto unlock;
	}

	freq_dev = (struct omrg_freq_device *)
			kzalloc(sizeof(*freq_dev), GFP_ATOMIC);
	if (IS_ERR_OR_NULL(freq_dev)) {
		pr_err("alloc freq dev err\n");
		goto unlock;
	}

	freq_dev->df = df;
	freq_dev->np = dev->of_node;
	freq_dev->amp = DEVFREQ_AMP;
	freq_dev->cur_freq = df->previous_freq * DEVFREQ_AMP / KHZ;
	INIT_LIST_HEAD(&freq_dev->master_list);
	INIT_LIST_HEAD(&freq_dev->slave_list);

	list_add_tail_rcu(&freq_dev->node, &g_omrg_devfreq_list);
	if (g_omrg_initialized)
		of_match_omrg_rule(freq_dev);

unlock:
	spin_unlock(&omrg_list_lock);
	omrg_refresh_all_rule();
}

/*
 * omrg_devfreq_unregister()
 * @df: devfreq device
 *
 * unregister omrg freq device for devfreq device, release the connection with
 * related omrg device of all rules, and if this device is master of the rule,
 * update this rule.
 *
 * Locking: this function will hold omrg_list_lock during unregistration,
 * cooling device should be unregistered before omrg freq device unregistration.
 * we will put destroy_work to worker thread and wait for completion of the work
 * to protect against omrg_rule_update_work.
 */
void omrg_devfreq_unregister(struct devfreq *df)
{
	struct omrg_freq_device *freq_dev = NULL;
	bool found = false;

	if (IS_ERR_OR_NULL(df)) {
		pr_err("%s:null devfreq\n", __func__);
		return;
	}

	/*
	 * prevent devfreq scaling freq when unregister freq device,
	 * cpufreq itself can prevent this from happening
	 */
	mutex_lock(&df->lock);
	spin_lock(&omrg_list_lock);

	list_for_each_entry_rcu(freq_dev, &g_omrg_devfreq_list, node) {
		if (freq_dev->df == df) {
			found = true;
			list_del_rcu(&freq_dev->node);
			break;
		}
	}

	spin_unlock(&omrg_list_lock);
	mutex_unlock(&df->lock);

	if (found)
		call_rcu(&freq_dev->rcu, omrg_kfree_freq_dev);
}

struct omrg_attr {
	struct attribute attr;
	ssize_t (*show)(const struct omrg_rule *, char *);
	ssize_t (*store)(struct omrg_rule *, const char *, size_t count);
};

#define omrg_attr_ro(_name) \
static struct omrg_attr _name = __ATTR(_name, 0440, show_##_name, NULL)

#define omrg_attr_rw(_name) \
static struct omrg_attr _name = __ATTR(_name, 0640, show_##_name, store_##_name)

static ssize_t show_ruler_enable(const struct omrg_rule *ruler, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n",
		ruler->enable ? "enabled" : "disabled");
}

static bool omrg_disable_ruler(struct omrg_rule *ruler)
{
	int i;
	bool ret = true;
	unsigned int cpu;
	struct omrg_device *slave = NULL;

	cpus_read_lock();
	for (i = 0; i < ruler->slave_num; i++) {
		slave = &ruler->slave_dev[i];
		if (IS_ERR_OR_NULL(slave->freq_dev))
			continue;

		cpu = (unsigned int)cpumask_any_and(
			&slave->freq_dev->cpus, cpu_online_mask);
		if (cpu >= (unsigned int)nr_cpu_ids) {
			ret = false;
			goto error_out;
		}

		if (
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_QOS_ARBITER)
			oplus_update_freq_qos_request(QOS_OWNER_OMRG, &slave->freq_dev->min_qos_req,
				slave->freq_dev->cpuinfo_min_freq / KHZ)
#else
			freq_qos_update_request(&slave->freq_dev->min_qos_req,
				slave->freq_dev->cpuinfo_min_freq / KHZ)
#endif /* CONFIG_OPLUS_FEATURE_CPU_QOS_ARBITER */
			< 0) {
			ret = false;
			goto error_out;
		}
		if (
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_QOS_ARBITER)
			oplus_update_freq_qos_request(QOS_OWNER_OMRG, &slave->freq_dev->max_qos_req,
				slave->freq_dev->cpuinfo_max_freq / KHZ)
#else
			freq_qos_update_request(&slave->freq_dev->max_qos_req,
				slave->freq_dev->cpuinfo_max_freq / KHZ)
#endif /* CONFIG_OPLUS_FEATURE_CPU_QOS_ARBITER */
			< 0) {
			ret = false;
			goto error_out;
		}
	}
error_out:
	cpus_read_unlock();
	return ret;
}

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CEILING_FREE)
void omrg_ceiling_free(bool ceiling_free_enable)
{
	struct omrg_rule *ruler = NULL;

	if (!g_omrg_initialized)
		return;

	if (atomic_read(&is_omrg_ceiling_free) == ceiling_free_enable)
		return;

	atomic_set(&is_omrg_ceiling_free, ceiling_free_enable);

	list_for_each_entry(ruler, &g_omrg_rule_list, node) {
		if (!ceiling_free_enable)
			omrg_irq_work_queue(&ruler->irq_work);
		else if (ceiling_free_enable && !omrg_disable_ruler(ruler))
			pr_err("omrg reset ceiling failed\n");
	}
}
EXPORT_SYMBOL(omrg_ceiling_free);
#endif

static ssize_t store_ruler_enable(struct omrg_rule *ruler,
				 const char *buf, size_t count)
{
	int enable;

	if (sscanf(buf, "%d", &enable) != 1)
		return -EINVAL;

	ruler->enable = !!enable;

	if (ruler->enable) {
		omrg_update_state(ruler, true);
	} else {
		if (!omrg_disable_ruler(ruler)) {
			ruler->enable = true;
			return -EINVAL;
		}
	}

	return count;
}

static ssize_t show_ruler_freq_dev_list(const struct omrg_rule *ruler, char *buf)
{
	int i, ret = 0;
	struct omrg_device *dd = NULL;

	for (i = 0; i < ruler->slave_num; i++) {
		dd = &ruler->slave_dev[i];

		if (IS_ERR_OR_NULL(dd->freq_dev))
			continue;

		ret += scnprintf(buf + ret, PAGE_SIZE - ret, "slave%d:%s ",
				i, dd->freq_dev->np->full_name);
	}

	for (i = 0; i < ruler->master_num; i++) {
		dd = &ruler->master_dev[i];

		if (IS_ERR_OR_NULL(dd->freq_dev))
			continue;

		ret += scnprintf(buf + ret, PAGE_SIZE - ret, "master%d:%s ",
				i, dd->freq_dev->np->full_name);
	}

	return ret += scnprintf(buf + ret, PAGE_SIZE - ret, "\n");
}

omrg_attr_rw(ruler_enable);
omrg_attr_ro(ruler_freq_dev_list);

#if (LINUX_VERSION_CODE > KERNEL_VERSION(5, 15, 0))
static struct attribute *omrg_attrs[] = {
#else
static struct attribute *default_attrs[] = {
#endif /*LINUX_VERSION_CODE > KERNEL_VERSION(5, 15, 0)*/
	&ruler_enable.attr,
	&ruler_freq_dev_list.attr,
	NULL
};

#if (LINUX_VERSION_CODE > KERNEL_VERSION(5, 15, 0))
ATTRIBUTE_GROUPS(omrg);
#endif /*LINUX_VERSION_CODE > KERNEL_VERSION(5, 15, 0)*/

#define to_omrg_rule(k) container_of(k, struct omrg_rule, kobj)
#define to_attr(a) container_of(a, struct omrg_attr, attr)

static ssize_t show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct omrg_rule *data = to_omrg_rule(kobj);
	struct omrg_attr *cattr = to_attr(attr);
	ssize_t ret = -EIO;

	if (cattr->show != NULL)
		ret = cattr->show(data, buf);

	return ret;
}

static ssize_t store(struct kobject *kobj, struct attribute *attr,
		     const char *buf, size_t count)
{
	struct omrg_rule *data = to_omrg_rule(kobj);
	struct omrg_attr *cattr = to_attr(attr);
	ssize_t ret = -EIO;

	if (cattr->store != NULL)
		ret = cattr->store(data, buf, count);

	return ret;
}

static const struct sysfs_ops sysfs_ops = {
	.show = show,
	.store = store,
};

static struct kobj_type ktype_omrg = {
	.sysfs_ops = &sysfs_ops,
#if (LINUX_VERSION_CODE > KERNEL_VERSION(5, 15, 0))
	.default_groups = omrg_groups,
#else
	.default_attrs = default_attrs,
#endif /*LINUX_VERSION_CODE > KERNEL_VERSION(5, 15, 0)*/
};

static int omrg_device_init(struct device *dev, struct omrg_device **omrg_dev,
			   int *dev_num, struct omrg_rule *ruler,
			   struct device_node *np, const char *prop)
{
	struct of_phandle_args omrg_dev_spec;
	struct omrg_device *t_omrg_dev = NULL;
	int divider_num = ruler->divider_num;
	unsigned int divider_size;
	int i;

	i = of_property_count_u32_elems(np, prop);
	if (i <= 0 || (i % (divider_num + 1))) {
		dev_err(dev, "count %s err:%d\n", prop, i);
		return -ENOENT;
	}

	*dev_num = i / (divider_num + 1);

	*omrg_dev = (struct omrg_device *)
			devm_kzalloc(dev, sizeof(**omrg_dev) * (*dev_num),
				     GFP_KERNEL);
	if (IS_ERR_OR_NULL(*omrg_dev)) {
		dev_err(dev, "alloc %s fail\n", prop);
		return -ENOMEM;
	}

	divider_size = sizeof(*(*omrg_dev)->divider) * (unsigned int)divider_num;
	if (divider_size > sizeof(omrg_dev_spec.args)) {
		dev_err(dev, "divider size too big:%u\n", divider_size);
		return -ENOMEM;
	}

	for (i = 0; i < *dev_num; i++) {
		t_omrg_dev = &(*omrg_dev)[i];
		t_omrg_dev->divider = (unsigned int *)
					devm_kzalloc(dev,
						     divider_size, GFP_KERNEL);
		if (IS_ERR_OR_NULL(t_omrg_dev->divider)) {
			dev_err(dev, "alloc %s%d fail\n", prop, i);
			return -ENOMEM;
		}

		if (of_parse_phandle_with_fixed_args(np, prop, divider_num,
						     i, &omrg_dev_spec)) {
			dev_err(dev, "parse %s%d err\n", prop, i);
			return -ENOENT;
		}

		t_omrg_dev->divider_dispense = (bool *)
					devm_kzalloc(dev,
						     sizeof(bool) * divider_num,
							GFP_KERNEL);
		if (IS_ERR_OR_NULL(t_omrg_dev->divider_dispense)) {
			dev_err(dev, "alloc divider_dispense fail\n");
			return -ENOMEM;
		}

		t_omrg_dev->np = omrg_dev_spec.np;
		t_omrg_dev->ruler = ruler;
		t_omrg_dev->state = INVALID_STATE;
		t_omrg_dev->max_freq = ULONG_MAX;
		memcpy((void *)t_omrg_dev->divider,
			       (const void *)omrg_dev_spec.args, (size_t)divider_size);
	}

	return 0;
}

static bool omrg_is_subset_device(struct omrg_device *dev, int dev_num,
				 struct omrg_device *ex_dev, int ex_dev_num)
{
	int i, j;

	for (i = 0; i < dev_num; i++) {
		for (j = 0; j < ex_dev_num; j++) {
			if (dev[i].np == ex_dev[j].np)
				break;
		}
		if (j == ex_dev_num)
			break;
	}

	return (i == dev_num);
}

static void omrg_init_ruler_work(struct omrg_rule *ruler)
{
	struct omrg_rule *ex_ruler = NULL;

	list_for_each_entry(ex_ruler, &g_omrg_rule_list, node) {
		if (ex_ruler->master_num < ruler->master_num ||
		    ex_ruler->slave_num < ruler->slave_num ||
		    ex_ruler->coop_work)
			continue;

		if (!omrg_is_subset_device(ruler->master_dev, ruler->master_num,
					  ex_ruler->master_dev,
					  ex_ruler->master_num))
			continue;

		if (!omrg_is_subset_device(ruler->slave_dev, ruler->slave_num,
					  ex_ruler->slave_dev,
					  ex_ruler->slave_num))
			continue;
		/*
		 * the master and slave of ruler is a subset of ex_ruler,
		 * share the ex_ruler's work
		 */
		ruler->coop_work = &ex_ruler->exec_work;
		return;
	}

	kthread_init_work(&ruler->exec_work, omrg_rule_update_work);
}

static int omrg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	struct device_node *child = NULL;
	struct omrg_rule *ruler = NULL;
	int ret;

	for_each_child_of_node(np, child) {
		ruler = (struct omrg_rule *)
				devm_kzalloc(&pdev->dev, sizeof(*ruler),
					     GFP_KERNEL);
		if (IS_ERR_OR_NULL(ruler)) {
			dev_err(dev, "alloc ruler fail\n");
			ret = -ENOMEM;
			goto err_probe;
		}

		if (of_property_read_u32(child, "oplus,divider-cells",
					 &ruler->divider_num)) {
			dev_err(dev, "read divider cells fail\n");
			ret = -ENOENT;
			goto err_probe;
		}

		ruler->up_limit =
			of_property_read_bool(child, "up_limit_enable");
		ruler->dn_limit =
			of_property_read_bool(child, "down_limit_enable");
		ruler->thermal_enable =
			of_property_read_bool(child, "thermal_enable");
		ruler->master_vote_max =
			of_property_read_bool(child, "master_vote_max");
		ruler->user_mode_enable =
			of_property_read_bool(child, "user_mode_enable");

		ret = omrg_device_init(&pdev->dev, &ruler->master_dev,
				      &ruler->master_num, ruler,
				      child, "oplus,omrg-master");
		if (ret)
			goto err_probe;

		ret = omrg_device_init(&pdev->dev, &ruler->slave_dev,
				      &ruler->slave_num, ruler,
				      child, "oplus,omrg-slave");
		if (ret)
			goto err_probe;

		ruler->name = child->name;
		/* disable ruler by default */
		ruler->enable = false;

		if (ruler->user_mode_enable)
			ruler->enable = false;

		ret = kobject_init_and_add(&ruler->kobj, &ktype_omrg,
					   &dev->kobj, "%s", ruler->name);
		if (ret) {
			dev_err(dev, "create kobj err %d\n", ret);
			goto err_probe;
		}

		omrg_init_ruler_work(ruler);
		ruler->work_in_progress = false;
		ruler->state_alter_pending = false;
		spin_lock_init(&ruler->wip_lock);
		init_irq_work(&ruler->irq_work, omrg_irq_work);

		list_add_tail(&ruler->node, &g_omrg_rule_list);
	}

	kthread_init_worker(&g_omrg_worker);
	g_omrg_work_thread = kthread_run(kthread_worker_fn,
					(void *)&g_omrg_worker, "oplus-omrg");
	if (IS_ERR(g_omrg_work_thread)) {
		dev_err(dev, "create omrg thread fail\n");
		ret = PTR_ERR(g_omrg_work_thread);
		goto err_probe;
	}

	ret = cpufreq_register_notifier(&omrg_cpufreq_policy_notifier,
					CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		dev_err(dev, "register cpufreq nb fail\n");
		goto err_probe;
	}

	list_for_each_entry(ruler, &g_omrg_rule_list, node)
		of_match_omrg_freq_device(ruler);

	g_omrg_initialized = true;

	omrg_refresh_all_rule();

	return 0;

err_probe:
	list_for_each_entry(ruler, &g_omrg_rule_list, node)
		kobject_del(&ruler->kobj);

	INIT_LIST_HEAD(&g_omrg_rule_list);

	return ret;
}

static const struct of_device_id omrg_of_match[] = {
	{
		.compatible = "oplus,oplus-omrg",
	},
	{ /* end */ }
};

static struct platform_driver omrg_driver = {
	.probe = omrg_probe,
	.driver = {
		.name = "oplus-omrg",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(omrg_of_match),
	},
};

module_platform_driver(omrg_driver);
MODULE_DESCRIPTION("OPLUS OMRG Driver");
MODULE_LICENSE("GPL v2");
