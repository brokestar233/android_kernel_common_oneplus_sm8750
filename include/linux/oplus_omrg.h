// SPDX-License-Identifier: GPL-2.0-only
/*
 * oplus_omrg.h
 *
 * header file of oplus_omrg module
 *
 * Copyright (c) 2020-2021 Oplus. All rights reserved.
 *
 */

#ifndef _OPLUS_OMRG_H
#define _OPLUS_OMRG_H

#include <linux/cpufreq.h>
#include <linux/devfreq.h>

enum omrg_dev_type {
	OMRG_NONE_DEV = 0,
	/* CPU TYPE if new cpu add after(eg. small strong ...) */
	OMRG_CPU_CLUSTER0 = (1 << 0) << 8,
	OMRG_CPU_CLUSTER1,
	OMRG_CPU_CLUSTER2,
	/* CACHE TYPE if new cache add after(eg. l4 l5 l6 ...)*/
	OMRG_L3_CACHE = (1 << 1) << 8,
	/* If new type add after here */
};

struct omrg_dev_freq {
	/* enum omrg_dev_type */
	unsigned int type;
	/* described as Hz  */
	unsigned long max_freq;
	unsigned long min_freq;
};

#define DEV_TYPE_CPU_CLUSTER (1 << 0) << 8

#define MAX_OMRG_MARGIN      1024U

#if IS_ENABLED(CONFIG_OPLUS_OMRG)
void omrg_devfreq_register(struct devfreq *df);
void omrg_devfreq_unregister(struct devfreq *df);
void omrg_cpufreq_cooling_update(unsigned int cpu, unsigned int clip_freq);
void omrg_devfreq_cooling_update(struct devfreq *df, unsigned long clip_freq);
unsigned int omrg_cpufreq_check_limit(struct cpufreq_policy *policy,
                     unsigned int target_freq);
unsigned long omrg_devfreq_check_limit(struct devfreq *df,
                      unsigned long target_freq);
int perf_ctrl_get_omrg_dev_freq(void __user *uarg);
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CEILING_FREE)
void omrg_ceiling_free(bool ceiling_free_enable);
#endif
#else
static inline void omrg_devfreq_register(struct devfreq *df) {}
static inline void omrg_devfreq_unregister(struct devfreq *df) {}
static inline void omrg_cpufreq_cooling_update(unsigned int cpu,
                          unsigned int clip_freq) {}
static inline void omrg_devfreq_cooling_update(struct devfreq *df,
                          unsigned long clip_freq) {}
static inline unsigned int omrg_cpufreq_check_limit(struct cpufreq_policy *policy,
                           unsigned int target_freq)
{
	return target_freq;
}

static inline unsigned long omrg_devfreq_check_limit(struct devfreq *df,
                            unsigned long target_freq)
{
	return target_freq;
}

static inline int perf_ctrl_get_omrg_dev_freq(void __user *uarg)
{
	return -EFAULT;
}
#if IS_ENABLED(CONFIG_OPLUS_FEATURE_CEILING_FREE)
static inline void omrg_ceiling_free(bool ceiling_free_enable) {}
#endif
#endif

/* extern bool oplus_cluster_cpu_all_pwrdn(void); */
#endif /* _OPLUS_OMRG_H */

