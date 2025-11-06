/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Copyright (C) 2019-2021 Oplus. All rights reserved.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/topology.h>
#include <linux/pm_opp.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>
#include <linux/sched/sysctl.h>
#include <linux/version.h>
#include <linux/freezer.h>
#include <linux/workqueue.h>
#include <linux/cpufreq_bouncing.h>

#define NSEC_TO_MSEC(val) (val / NSEC_PER_MSEC)
#define MSEC_TO_NSEC(val) (val * NSEC_PER_MSEC)
#define MSEC_TO_USEC(val) (val * USEC_PER_MSEC)

#define NR_FREQ 48
/* New Qcom SOC with 4 cpufreq cluster, use kernel6.1 version */
#define NR_CLUS_MAX 4
#define NR_CORE_MAX 8

enum QOS_OWNER {
	QOS_OWNER_UAH = 0,
	QOS_OWNER_OMRG,
	QOS_OWNER_CB,
	QOS_OWNER_SBE,

	QOS_OWNER_MAX
};

/* cluster based */
struct cpufreq_bouncing {
	int first_cpu;

	/* statistics */
	u64 last_ts;
	u64 last_freq_update_ts;
	u64 acc;

	/* restriction */
	int limit_freq;
	int limit_level;
	u64 limit_thres;

	/* freqs */
	int freq_sorting;
	int freq_levels;
	unsigned int freqs[NR_FREQ]; /* quick mapping */

	/* trace info */
	long long freqs_resident[NR_FREQ]; /* for record how long freqs stay */

	/* config */
	bool enable;
	int cur_level;

	/* config: ceil */
	int max_level;
	int down_speed;
	s64 down_limit_ns;
	unsigned int max_freq;

	/* config: floor */
	int min_level;
	int up_speed;
	s64 up_limit_ns;
	unsigned int min_freq;

	/*
	 * check current limitation
	 * if limitation higher than target, not count
	 */

	/* control freq boundary */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
	struct freq_qos_request qos_req;
	struct work_struct qos_work;
#endif
	bool ctl_inited;
} cb_stuff[NR_CLUS_MAX] = {
	/* silver */
	{
		.first_cpu     = -1,
		.enable        = false,
		.freqs_resident[0 ... NR_FREQ - 1] = -1,
		.freqs[0 ... NR_FREQ - 1] = UINT_MAX,
	},
	/* gold 3-core */
	{
		.first_cpu     = -1,
		.enable        = false,
		.down_limit_ns = 50 * NSEC_PER_MSEC,
		.up_limit_ns   = 50 * NSEC_PER_MSEC,
		.limit_thres   = 30 * NSEC_PER_MSEC,
		.limit_freq    = 0,
		.limit_level   = 0,
		.down_speed    = 0,
		.up_speed      = 0,
		.freqs_resident[0 ... NR_FREQ - 1] = -1,
		.freqs[0 ... NR_FREQ - 1] = UINT_MAX,
	},
	/* gold 2-core */
	{
		.first_cpu     = -1,
		.enable        = false,
		.down_limit_ns = 50 * NSEC_PER_MSEC,
		.up_limit_ns   = 50 * NSEC_PER_MSEC,
		.limit_thres   = 30 * NSEC_PER_MSEC,
		.limit_freq    = 0,
		.limit_level   = 0,
		.down_speed    = 0,
		.up_speed      = 0,
		.freqs_resident[0 ... NR_FREQ - 1] = -1,
		.freqs[0 ... NR_FREQ - 1] = UINT_MAX,
	},
	/* gold prime */
	{
		.first_cpu     = -1,
		.enable        = false,
		.down_limit_ns = 50 * NSEC_PER_MSEC,
		.up_limit_ns   = 50 * NSEC_PER_MSEC,
		.limit_thres   = 30 * NSEC_PER_MSEC,
		.limit_freq    = 0,
		.limit_level   = 0,
		.down_speed    = 0,
		.up_speed      = 0,
		.freqs_resident[0 ... NR_FREQ - 1] = -1,
		.freqs[0 ... NR_FREQ - 1] = UINT_MAX,
	}
};

ssize_t oplus_update_freq_qos_request(enum QOS_OWNER owner, struct freq_qos_request *req, int val);
ssize_t oplus_add_freq_qos_request(enum QOS_OWNER owner, struct freq_constraints *qos,
									struct freq_qos_request *req, enum freq_qos_req_type type, s32 value);

/* main thread */
static struct task_struct *cb_task;

/* sleep range */
static unsigned long sleep_range_ms[2] = {20, 30};
module_param_array(sleep_range_ms, ulong, NULL, 0664);

/* core boost */
static u64 last_core_boost_ts;
static bool last_core_boost;
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CEILING_FREE)
static atomic_t is_cb_ceiling_free;
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
static bool freq_qos_check = true;
module_param_named(freq_qos_check, freq_qos_check, bool, 0664);
static struct workqueue_struct *cb_qos_wq;
#endif

static unsigned int core_boost_lat_ns = MSEC_TO_NSEC(50);
module_param_named(core_boost_lat_ns, core_boost_lat_ns, uint, 0664);

static bool core_ctl_check;
module_param_named(core_ctl_check, core_ctl_check, bool, 0664);

static void cb_reset_qos(int cpu)
{
	struct cpufreq_bouncing *cb = &cb_stuff[cpu];

	if (!cb->ctl_inited)
		return;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
	/* reset qos */
	if (
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_QOS_ARBITER)
	oplus_update_freq_qos_request(QOS_OWNER_CB, &cb->qos_req, FREQ_QOS_MAX_DEFAULT_VALUE)
#else
	freq_qos_update_request(&cb->qos_req, FREQ_QOS_MAX_DEFAULT_VALUE)
#endif /* CONFIG_OPLUS_FEATURE_CPU_QOS_ARBITER */
		< 0)
		pr_warn("cluster %d reset qos req failed.\n", cpu);
#endif
}

/* init config */
static bool self_activate;

static int self_activate_show(char *buf, const struct kernel_param *kp)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", self_activate);
}

static int self_activate_store(const char *buf, const struct kernel_param *kp)
{
	unsigned int val;
	int i;

	if (sscanf(buf, "%u\n", &val) == 0)
		return -EINVAL;

	self_activate = !!val;

	if (self_activate)
		wake_up_process(cb_task);
	else {
		for (i = 0; i < NR_CLUS_MAX; ++i)
			cb_reset_qos(i);
	}

	return 0;
}

static struct kernel_param_ops self_activate_ops = {
	.get = self_activate_show,
	.set = self_activate_store,
};
module_param_cb(self_activate, &self_activate_ops, NULL, 0664);

static bool enable = true;

static int enable_show(char *buf, const struct kernel_param *kp)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", enable);
}

static int enable_store(const char *buf, const struct kernel_param *kp)
{
	unsigned int val;
	int i;

	if (sscanf(buf, "%u\n", &val) == 0) {
		pr_warn("cb input invalid (%s)\n", buf);
		return -EINVAL;
	}

	enable = (bool) val;

	/* reset all */
	for (i = 0; i < NR_CLUS_MAX; ++i) {
		struct cpufreq_bouncing *cb = &cb_stuff[i];

		cb_reset_qos(i);

		/* reset core boost */
		last_core_boost = false;
		last_core_boost_ts = 0;
		/* cb_do_core_boost_work(); */

		/* reset acc */
		cb->last_ts = 0;
		cb->last_freq_update_ts = 0;
		cb->acc = 0;
		cb->cur_level = cb->max_level ? cb->max_level : NR_FREQ - 1;
	}

	if (self_activate)
		wake_up_process(cb_task);

	pr_info("cb %s\n", enable ? "enabled" : "disabled");

	return 0;
}

static struct kernel_param_ops enable_ops = {
	.get = enable_show,
	.set = enable_store,
};
module_param_cb(enable, &enable_ops, NULL, 0664);

static bool debug, init_complete;
module_param_named(debug, debug, bool, 0664);

static unsigned int decay = 80;
module_param_named(decay, decay, uint, 0664);

static DEFINE_PER_CPU(struct cpufreq_bouncing*, cbs);

static int cb_pol_idx;

static inline struct cpufreq_bouncing *cb_get(int cpu)
{
	struct cpufreq_bouncing *cb;

	if (cpu < 0 || cpu >= NR_CORE_MAX)
		return NULL;

	cb = per_cpu(cbs, cpu);

	if (unlikely(!cb))
		return NULL;

	if (unlikely(!cb->ctl_inited))
		return NULL;

	if (!enable || !cb->enable)
		return NULL;

	return cb;
}

/* module parameters */
static int cb_config_store(const char *buf, const struct kernel_param *kp)
{
	/*
	 * for limit_thres, down/up limit will use ms as unit
	 * format:
	 * echo "2,0,5,3000,3,2000,3,2000" > config
	 */
	struct pack {
		int clus;

		int limit_level;
		u64 limit_thres_ms;

		int down_speed;
		s64 down_limit_ms;

		int up_speed;
		s64 up_limit_ms;
	} v;
	int v_enable;
	struct cpufreq_bouncing *cb;

	if (debug)
		pr_info("%s\n", buf);

	if (sscanf(buf, "%d,%d,%d,%llu,%d,%lld,%d,%lld\n",
		&v.clus,
		&v_enable,
		&v.limit_level,
		&v.limit_thres_ms,
		&v.down_speed,
		&v.down_limit_ms,
		&v.up_speed,
		&v.up_limit_ms) != 8)
		goto out;

	if (v.clus < 0 || v.clus >= cb_pol_idx)
		goto out;

	cb = &cb_stuff[v.clus];

	if (v.limit_level < 0 || v.limit_level > cb->max_level)
		goto out;

	if (v.down_speed < 0 || v.down_speed > cb->freq_levels)
		goto out;

	if (v.up_speed < 0 || v.up_speed > cb->freq_levels)
		goto out;

	if (v.down_limit_ms < 0 || v.up_limit_ms < 0 ||
		v.limit_thres_ms < 0)
		goto out;

	/* begin update config */
	cb->enable = false;
	cb->last_ts = 0;
	cb->last_freq_update_ts = 0;
	cb->acc = 0;
	cb->limit_level = v.limit_level;
	cb->limit_freq = cb->freqs[cb->limit_level];
	cb->limit_thres = MSEC_TO_NSEC(v.limit_thres_ms);
	cb->down_speed = v.down_speed;
	cb->down_limit_ns = MSEC_TO_NSEC(v.down_limit_ms);
	cb->up_speed = v.up_speed;
	cb->up_limit_ns = MSEC_TO_NSEC(v.up_limit_ms);
	cb->enable = v_enable;

	return 0;
out:
	pr_warn("config: invalid:%s\n", buf);
	return 0;
}

static int cb_config_show(char *buf, const struct kernel_param *kp)
{
	struct cpufreq_bouncing *cb;
	int i, j, cnt = 0;

	for (i = 0; i < min(NR_CLUS_MAX, cb_pol_idx); ++i) {
		cb = &cb_stuff[i];
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "=====\n");
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "clus %d first_cpu %d ctl %d\n", i, cb->first_cpu, cb->ctl_inited);
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "last_ts: %llu\n", cb->last_ts);
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "last_freq_update_ts: %llu\n", cb->last_freq_update_ts);
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "acc: %llu\n", cb->acc);

		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "limit_freq: %d\n", cb->limit_freq);
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "limit_level: %d\n", cb->limit_level);
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "limit_thres: %llu ms\n", NSEC_TO_MSEC(cb->limit_thres));

		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "enable: %d\n", cb->enable);
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "cur_level: %d\n", cb->cur_level);

		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "max_freq: %u %d\n", cb->max_freq, cb->max_level);
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "down_speed: %d\n", cb->down_speed);
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "down_limit_lat: %lld ms\n", NSEC_TO_MSEC(cb->down_limit_ns));

		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "min_freq: %u %d\n", cb->min_freq, cb->min_level);
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "up_speed: %d\n", cb->up_speed);
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "up_limit_lat: %lld ms\n", NSEC_TO_MSEC(cb->up_limit_ns));

		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "freq_sorting: %d\n", cb->freq_sorting);
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "freq_levels: %d\n", cb->freq_levels);
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "freq idx:");
		for (j = 0; j < cb->freq_levels; ++j)
			cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "\t%u", j);
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "\n");
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "freq clk:");
		for (j = 0; j < cb->freq_levels; ++j)
			cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "\t%u", cb->freqs[j]);
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "\n");
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "last_core_boost: %d\n", last_core_boost);
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "last_core_boost_ts: %llu\n", last_core_boost_ts);
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "\n");
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "freq resident:");
		for (j = 0; j < cb->freq_levels; ++j)
			cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "\t%lld", cb->freqs_resident[j]);
		cnt += snprintf(buf + cnt, PAGE_SIZE - cnt, "\n");
	}

	return cnt;
}

static struct kernel_param_ops cb_config_ops = {
	.set = cb_config_store,
	.get = cb_config_show,
};
module_param_cb(config, &cb_config_ops, NULL, 0664);

/* for info collection */
static int cb_trace_show(char *buf, const struct kernel_param *kp)
{
	struct cpufreq_bouncing *cb;
	int ret = 0;
	int i, j;

	for (i = 0; i < min(NR_CLUS_MAX, cb_pol_idx); ++i) {
		long long total = 0;
		long long freq_avg = 0;
		long long freq_max = 0;
		long long freq_min = UINT_MAX;
		long long freqs_resident[NR_FREQ] = {0};

		cb = &cb_stuff[i];

		for (j = cb->limit_level; j < NR_FREQ; ++j) {
			if (cb->freqs[j] == UINT_MAX)
				break;

			if (cb->freqs_resident[j] > 0) {
				total += NSEC_TO_MSEC(cb->freqs_resident[j]);
				freqs_resident[j] = NSEC_TO_MSEC(cb->freqs_resident[j]);
				cb->freqs_resident[j] = 0;
			}
		}

		for (j = cb->limit_level; j < NR_FREQ; ++j) {
			if (cb->freqs[j] == UINT_MAX)
				break;

			if (freqs_resident[j]) {
				freq_avg += cb->freqs[j] * freqs_resident[j] / total;
				freq_max = max_t(long long, freq_max, cb->freqs[j]);
				freq_min = min_t(long long, freq_min, cb->freqs[j]);
				cb->freqs_resident[j] = 0;
			}
		}

		/* output, format: clus_id, status, min, max, avg */
		ret += snprintf(buf + ret, PAGE_SIZE - ret, "%d,%d,%lld,%lld,%lld,",
			i, cb->enable, freq_min, freq_max, freq_avg);
	}
	ret += snprintf(buf + ret, PAGE_SIZE - ret, "\n");

	return ret;
}

static struct kernel_param_ops cb_trace_ops = {
	.get = cb_trace_show,
};
module_param_cb(trace, &cb_trace_ops, NULL, 0444);

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CEILING_FREE)
void cb_ceiling_free(bool ceiling_free_enable)
{
	if (!enable)
		return;

	if (atomic_read(&is_cb_ceiling_free) == ceiling_free_enable)
		return;

	atomic_set(&is_cb_ceiling_free, ceiling_free_enable);

	for (int i = 0; i < NR_CLUS_MAX; ++i) {
		struct cpufreq_bouncing *cb = &cb_stuff[i];
		cb = cb_get(cb->first_cpu);
		if (!cb)
			continue;

		if (ceiling_free_enable)
			cb_reset_qos(i);
		else if (likely(cb_qos_wq))
			queue_work(cb_qos_wq, &cb->qos_work);
	}
}
EXPORT_SYMBOL(cb_ceiling_free);
#endif

static inline bool clus_isolated(struct cpufreq_policy *pol)
{
	cpumask_t active;

	cpumask_and(&active, pol->related_cpus, cpu_active_mask);
	return !cpumask_weight(&active);
}

unsigned int cb_cap(struct cpufreq_policy *pol, unsigned int freq)
{
	struct cpufreq_bouncing *cb;
	unsigned int capped = freq;
	int cpu;

	if (!enable)
		return freq;

	/* can remove since we're calling from sugov_fast_switch path */
	if (!pol->fast_switch_enabled)
		return freq;

	if (freq_qos_check)
		return freq;

	cpu = cpumask_first(pol->related_cpus);
	cb = cb_get(cpu);

	if (unlikely(!cb))
		return freq;

	if (!cb->enable)
		return freq;

	capped = min(freq, cb->freqs[cb->cur_level]);

	if (debug)
		pr_info("cpu %d, orig %u, capped %d\n", cpu, freq, capped);

	return capped;
}

void cb_reset(int cpu, u64 time)
{
	struct cpufreq_bouncing *cb;

	if (!enable)
		return;

	cb = cb_get(cpu);
	if (!cb)
		return;

	/* reset only when cluster has no active cpu */
	if (!clus_isolated(cpufreq_cpu_get_raw(cpu)))
		return;

	cb->last_ts = time;
	cb->last_freq_update_ts = time;
	cb->acc = 0;
	cb->cur_level = cb->max_level;
}

void cb_update(struct cpufreq_policy *pol, u64 time)
{
	struct cpufreq_bouncing *cb;
	u64 delta, update_delta;
	int cpu, prev_level;
	bool min_over_target_freq, isolated;

	if (unlikely(!pol->fast_switch_enabled))
		return;

	if (!enable || !init_complete)
		return;

	cpu = cpumask_first(pol->related_cpus);
	cb = cb_get(cpu);

	if (unlikely(!cb))
		return;

	/* check isolated status */
	isolated = clus_isolated(pol);

	/* check current min_freq and limit target freq, if min_freq large than limited, reset acc */
	min_over_target_freq = pol->min >= cb->limit_freq;
	if (min_over_target_freq || isolated)
		cb->acc = 0;

	/* for first update */
	if (unlikely(!cb->last_ts))
		cb->last_ts = cb->last_freq_update_ts = time;

	/*
	 * not count flag only affects to delta.
	 * keep update_delta to let limit_freq has time to restore
	 */
	delta = ((min_over_target_freq || isolated) ? 0 : (time - cb->last_ts));
	update_delta = time - cb->last_freq_update_ts;

	/* check cpufreq */
	if (pol->cur >= cb->limit_freq) {
		/* accumulate delta time */
		cb->acc += delta;
	} else {
		/* decay accumulate time */
		cb->acc = cb->acc * decay / 100;
	}

	/* check if need to update limitation */
	prev_level = cb->cur_level;
	if (cb->acc >= cb->limit_thres) {
		/* check last update */
		if (update_delta >= cb->down_limit_ns) {
			cb->cur_level = max(prev_level - cb->down_speed, cb->limit_level);
			cb->last_freq_update_ts = time;
			cb->freqs_resident[prev_level] += update_delta;
		}
	} else {
		/* check last update */
		if (update_delta >= cb->up_limit_ns) {
			cb->cur_level = min(prev_level + cb->up_speed, cb->max_level);
			cb->last_freq_update_ts = time;
			cb->freqs_resident[prev_level] += update_delta;
		}
	}

	/* when min bar is higher than cb limit, unlock immediately */
	if (min_over_target_freq || isolated)
		cb->cur_level = cb->max_level;

	/* update core_ctl boost status */
	/* cb_core_boost(time); */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
	/* queue qos request */
	if (freq_qos_check &&
			cb->qos_req.pnode.prio != cb->freqs[cb->cur_level] &&
			update_delta >= max(cb->down_limit_ns, cb->up_limit_ns) &&
			likely(cb_qos_wq))
		queue_work(cb_qos_wq, &cb->qos_work);
#endif

	if (debug)
		pr_info("cpu %d update: ts now %llu last %llu last_update %llu delta %llu update_d %llu cur %u acc %llu cur_level %id "
			"min_over_target_freq %d isolated %d last_core_boost %d\n",
			cpu,
			NSEC_TO_MSEC(time),
			NSEC_TO_MSEC(cb->last_ts),
			NSEC_TO_MSEC(cb->last_freq_update_ts),
			NSEC_TO_MSEC(delta),
			NSEC_TO_MSEC(update_delta),
			pol->cur,
			NSEC_TO_MSEC(cb->acc),
			cb->cur_level,
			min_over_target_freq,
			isolated,
			last_core_boost);

	cb->last_ts = time;
}
EXPORT_SYMBOL(cb_update);

static int cb_parse_cpufreq(struct cpufreq_policy *pol)
{
	struct cpufreq_frequency_table *table, *pos;
	struct device *cpu_dev;
	struct cpufreq_bouncing *cb;

	unsigned int freq = 0, max_freq = 0, min_freq = UINT_MAX;
	int i, idx, cb_idx, first_cpu, freq_levels = 0;

	first_cpu = cpumask_first(pol->related_cpus);
	cpu_dev = get_cpu_device(first_cpu);
	if (cpu_dev == NULL) {
		pr_err("cb failed to get cpu device, return\n");
		return -1;
	}

	cb_idx = topology_cluster_id(cpu_dev->id);
	if (cb_idx >= NR_CLUS_MAX) {
		pr_err("cb get error cluster id, return\n");
		return -1;
	}

	cb = &cb_stuff[cb_idx];
	if (cb->first_cpu == -1)
		cb->first_cpu = first_cpu;

	/* get & setup freq_levels */
	table = pol->freq_table;
	cb->freq_sorting = pol->freq_table_sorted;
	cpufreq_for_each_valid_entry_idx(pos, table, idx) {
		++freq_levels;
		freq = pos->frequency;
		cb->freqs[idx] = freq;
		if (freq > max_freq) {
			max_freq = freq;
			cb->max_level = idx;
			cb->cur_level = idx;
			cb->max_freq = max_freq;
		}
		if (freq < min_freq) {
			min_freq = freq;
			cb->min_level = idx;
			cb->min_freq = min_freq;
		}
	}
	cb->freq_levels = freq_levels;
	++cb_pol_idx;

	for_each_cpu(i, pol->cpus) {
		per_cpu(cbs, i) = &cb_stuff[cb_idx];
	}

	pr_info("cb_parse_cpufreq first_cpu %d, cb_idx %d cb_pol_idx %d freq_levels %d max_level %d idx %d \n",
			first_cpu, cb_idx, cb_pol_idx, cb->freq_levels, cb->max_level, idx);

	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
static void cb_do_boundary_change_work(struct work_struct *qos_work)
{
	struct cpufreq_bouncing *cb =
		container_of(qos_work, struct cpufreq_bouncing, qos_work);
	struct cpufreq_policy *pol = cpufreq_cpu_get_raw(cb->first_cpu);
	unsigned int target;

	if (!pol)
		return;

	target = max(cb->freqs[cb->cur_level], pol->min);

	if (debug)
		pr_info("processing work cpu %d min %u max %u target %u req max %u\n",
			cb->first_cpu, pol->min, pol->max, target, cb->qos_req.pnode.prio);

#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CEILING_FREE)
	if (atomic_read(&is_cb_ceiling_free))
		target = FREQ_QOS_MAX_DEFAULT_VALUE;
#endif

	if (
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_QOS_ARBITER)
		oplus_update_freq_qos_request(QOS_OWNER_CB, &cb->qos_req, target)
#else
		freq_qos_update_request(&cb->qos_req, target)
#endif /* CONFIG_OPLUS_FEATURE_CPU_QOS_ARBITER */
		< 0)
		pr_err("failed to update freq constraint. cpu %d cb_limit %u\n",
			cb->first_cpu, target);
}
#endif

static int cb_init_ctl(struct cpufreq_policy *pol)
{
	struct cpufreq_bouncing *cb;
	int i, first_cpu;

	first_cpu = cpumask_first(pol->related_cpus);

	for_each_cpu(i, pol->related_cpus) {
		cb = per_cpu(cbs, i);
		if (unlikely(!cb))
			return -EFAULT;

		if (!cb->ctl_inited) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
			if (!cb_qos_wq) {
				cb_qos_wq = alloc_workqueue("cb_qos_wq", WQ_HIGHPRI, 0);
				if (!cb_qos_wq)
					return -EFAULT;
			}
			INIT_WORK(&cb->qos_work, cb_do_boundary_change_work);
			if (
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CPU_QOS_ARBITER)
				oplus_add_freq_qos_request(QOS_OWNER_CB, &pol->constraints, &cb->qos_req,
					FREQ_QOS_MAX, FREQ_QOS_MAX_DEFAULT_VALUE)
#else
				freq_qos_add_request(&pol->constraints, &cb->qos_req, FREQ_QOS_MAX,
					FREQ_QOS_MAX_DEFAULT_VALUE)
#endif /* CONFIG_OPLUS_FEATURE_CPU_QOS_ARBITER */
			< 0) {
				pr_err("add qos request failed. cpu %d\n", cb->first_cpu);
				return -EFAULT;
			}
#endif
			cb->ctl_inited = true;
		}
	}
	return 0;
}

static int cb_main(void *arg)
{
	struct cpufreq_bouncing *cb;
	struct cpufreq_policy *pol;
	int i;

	while (!kthread_should_stop()) {
		set_current_state(TASK_RUNNING);
		if (!enable || !self_activate) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule();
		}

		for (i = 0; i < min(NR_CLUS_MAX, cb_pol_idx); ++i) {
			cb = &cb_stuff[i];
			pol = cpufreq_cpu_get(cb->first_cpu);
			if (pol) {
				cb_update(pol, ktime_to_ns(ktime_get()));
				cpufreq_cpu_put(pol);
			}
		}

		if (self_activate) {
			usleep_range(MSEC_TO_USEC(sleep_range_ms[0]), MSEC_TO_USEC(sleep_range_ms[1])); /* min ~ max */
		}
	}

	return 0;
}

static unsigned long is_init = 0;
void cb_stuff_init(struct cpufreq_policy *policy)
{
	int i, first_cpu, cb_idx;
	struct device *cpu_dev;

	first_cpu = cpumask_first(policy->related_cpus);
	if (test_bit(first_cpu, &is_init))
		return;

	cpu_dev = get_cpu_device(first_cpu);
	if (cpu_dev == NULL) {
		pr_err("cb failed to get cpu device, return\n");
		return;
	}

	cb_idx = topology_cluster_id(cpu_dev->id);
	if (cb_idx >= NR_CLUS_MAX) {
		pr_err("cb get error cluster id, return\n");
		return;
	}

	cb_parse_cpufreq(policy);
	if (cb_init_ctl(policy)) {
		pr_err("cb init ctl failed\n");
		if (cb_qos_wq) {
			destroy_workqueue(cb_qos_wq);
			cb_qos_wq = NULL;
		}

		for_each_cpu(i, policy->related_cpus) {
			if (per_cpu(cbs, i))
				per_cpu(cbs, i)->ctl_inited = false;
		}
		pr_err("cb finish init with disabled by default\n");
		return;
	}

	set_bit(first_cpu, &is_init);

	/* here now, the three policy all are init sucessful. */
	if (cpumask_last(policy->related_cpus) == num_possible_cpus() - 1) {
		cb_task = kthread_run(cb_main, NULL, "cb_task");
		pr_info("cb stuff init sucessful\n");
		init_complete = true;
	}
}

static int cpufreq_bouncing_register_callback(struct notifier_block *nb,
						unsigned long val, void *data)
{
	struct cpufreq_policy *policy = (struct cpufreq_policy *)data;

	if (IS_ERR_OR_NULL(policy)) {
		pr_err("%s:null cpu policy\n", __func__);
		return NOTIFY_DONE;
	}

	switch (val) {
	case CPUFREQ_CREATE_POLICY:
		cb_stuff_init(policy);
		break;
	}

	return NOTIFY_OK;
}


static struct notifier_block cpufreq_bouncing_register_notifier = {
	.notifier_call = cpufreq_bouncing_register_callback,
};

static int __init cb_init(void)
{
	int ret = 0;

	init_complete = false;
	ret = cpufreq_register_notifier(&cpufreq_bouncing_register_notifier,
					CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_info("cpufreq_bouncing register failed!\n");
		return -EINVAL;
	}
	pr_info("cpufreq bouncing init...\n");

	return 0;
}

static void __exit cb_exit(void)
{
	init_complete = false;
	cpufreq_unregister_notifier(&cpufreq_bouncing_register_notifier,
					CPUFREQ_POLICY_NOTIFIER);
	pr_info("cpufreq bouncing exit\n");
}
module_init(cb_init);
module_exit(cb_exit);
MODULE_DESCRIPTION("OPLUS CPUFREQ BOUNCING Driver");
MODULE_LICENSE("GPL v2");
