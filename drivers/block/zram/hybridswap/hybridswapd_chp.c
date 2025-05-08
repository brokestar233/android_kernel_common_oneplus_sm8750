// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020-2022 Oplus. All rights reserved.
 */

#define pr_fmt(fmt) "[HYB_ZRAM]" fmt

#include <uapi/linux/sched/types.h>
#include <linux/sched.h>
#include <linux/memory.h>
#include <linux/freezer.h>
#include <linux/swap.h>
#include <linux/cgroup-defs.h>
#include <linux/seq_file.h>
#include <linux/device.h>
#include <linux/cpuhotplug.h>
#include <linux/cpumask.h>
#include <linux/file.h>

#if IS_ENABLED(CONFIG_DRM_PANEL_NOTIFY) || IS_ENABLED(CONFIG_QCOM_PANEL_EVENT_NOTIFIER)
#include <linux/soc/qcom/panel_event_notifier.h>
#include <drm/drm_panel.h>
#include <linux/of.h>
#elif IS_ENABLED(CONFIG_DRM_MSM) || IS_ENABLED(CONFIG_DRM_OPLUS_NOTIFY)
#include <linux/msm_drm_notify.h>
#elif IS_ENABLED(CONFIG_OPLUS_MTK_DRM_GKI_NOTIFY)
#include <linux/mtk_panel_ext.h>
#include <linux/mtk_disp_notify.h>
#endif

#include "../zram_drv.h"
#include "../zram_drv_internal.h"
#include "./internal.h"
#ifdef CONFIG_CONT_PTE_HUGEPAGE
#include "chp_ext.h"
#endif

enum swapd_pressure_level {
	LEVEL_LOW = 0,
	LEVEL_MEDIUM,
	LEVEL_CRITICAL,
	LEVEL_COUNT
};

struct swapd_param {
	unsigned int min_score;
	unsigned int max_score;
	unsigned int ub_mem2zram_ratio;
	unsigned int ub_zram2ufs_ratio;
	unsigned int refault_threshold;
};

struct hybridswapd_task {
	wait_queue_head_t swapd_wait;
	atomic_t swapd_wait_flag;
	struct task_struct *swapd;
	struct cpumask swapd_bind_cpumask;
};
#define PGDAT_ITEM_DATA(pgdat) ((struct hybridswapd_task *)(pgdat)->android_oem_data1)
#define PGDAT_ITEM(pgdat, item) (PGDAT_ITEM_DATA(pgdat)->item)

#define HS_SWAP_ANON_REFAULT_THRESHOLD 22000
#define ANON_REFAULT_SNAPSHOT_MIN_INTERVAL 200
#define EMPTY_ROUND_SKIP_INTERNVAL 20
#define MAX_SKIP_INTERVAL 1000
#define EMPTY_ROUND_CHECK_THRESHOLD 10
#define ZRAM_WM_RATIO 75
#define COMPRESS_RATIO 30
#define SWAPD_MAX_LEVEL_NUM 10
#define SWAPD_DEFAULT_BIND_CPUS "0-3"
#define MAX_RECLAIMIN_SZ (200llu << 20)
#define page_to_kb(nr) (nr << (PAGE_SHIFT - 10))
#define SWAPD_SHRINK_WINDOW (HZ * 10)
#define SWAPD_SHRINK_SIZE_PER_WINDOW 1024
#define PAGES_TO_MB(pages) ((pages) >> 8)
#define PAGES_PER_1MB (1 << 8)
#define BATCH_PER_CYCLE_MB (32)

typedef bool (*free_swap_is_low_func)(void);
static free_swap_is_low_func free_swap_is_low_fp;

static unsigned long long swapd_skip_interval;
static bool last_round_is_empty;
static unsigned long last_swapd_time;
static struct eventfd_ctx *swapd_press_efd[LEVEL_COUNT];
static atomic64_t zram_wm_ratio = ATOMIC_LONG_INIT(ZRAM_WM_RATIO);
static atomic64_t compress_ratio = ATOMIC_LONG_INIT(COMPRESS_RATIO);
static atomic_t avail_buffers = ATOMIC_INIT(0);
static atomic_t min_avail_buffers = ATOMIC_INIT(0);
static atomic_t high_avail_buffers = ATOMIC_INIT(0);
static atomic_t max_reclaim_size = ATOMIC_INIT(100);
static atomic64_t free_swap_threshold = ATOMIC64_INIT(0);
static atomic64_t zram_crit_thres = ATOMIC_LONG_INIT(0);
static atomic64_t cpuload_threshold = ATOMIC_LONG_INIT(0);
static atomic64_t empty_round_skip_interval = ATOMIC_LONG_INIT(EMPTY_ROUND_SKIP_INTERNVAL);
static atomic64_t max_skip_interval = ATOMIC_LONG_INIT(MAX_SKIP_INTERVAL);
static atomic64_t empty_round_check_threshold = ATOMIC_LONG_INIT(EMPTY_ROUND_CHECK_THRESHOLD);
static unsigned long reclaim_exceed_sleep_ms = 50;
static unsigned long all_totalreserve_pages;
static u64 zram_used_limit_pages = 0;
static unsigned int chp_per_reclaim_mib = 64;

static DEFINE_MUTEX(pressure_event_lock);
static pid_t swapd_pid = -1;
static struct swapd_param zswap_param[SWAPD_MAX_LEVEL_NUM];
static enum cpuhp_state swapd_online;
static u64 max_reclaimin_size = MAX_RECLAIMIN_SZ;
static atomic_long_t fault_out_pause = ATOMIC_LONG_INIT(0);
static atomic_t display_off = ATOMIC_LONG_INIT(0);
#if IS_ENABLED(CONFIG_DRM_MSM) || IS_ENABLED(CONFIG_DRM_OPLUS_NOTIFY) || IS_ENABLED(CONFIG_OPLUS_MTK_DRM_GKI_NOTIFY)
static struct notifier_block fb_notif;
#elif IS_ENABLED(CONFIG_DRM_PANEL_NOTIFY) || IS_ENABLED(CONFIG_QCOM_PANEL_EVENT_NOTIFIER)
static void *g_panel_cookie;
#endif

static unsigned long swapd_shrink_window = SWAPD_SHRINK_WINDOW;
static unsigned long swapd_shrink_limit_per_window = SWAPD_SHRINK_SIZE_PER_WINDOW;
static unsigned long swapd_last_window_start;
static unsigned long swapd_last_window_shrink;
static atomic_t swapd_pause = ATOMIC_INIT(0);
static atomic_t swapd_enabled = ATOMIC_INIT(0);
static unsigned long swapd_nap_jiffies = 1;

extern unsigned long try_to_free_mem_cgroup_pages(struct mem_cgroup *memcg,
						  unsigned long nr_pages,
						  gfp_t gfp_mask,
						  unsigned int reclaim_options);

static void wake_up_all_hybridswapds(void);

#ifdef CONFIG_OPLUS_JANK
extern u32 get_cpu_load(u32 win_cnt, struct cpumask *mask);
#endif

static inline bool current_is_hybrid_swapd(void)
{
	return current->pid == swapd_pid;
}

static inline u64 get_zram_wm_ratio_value(void)
{
	return atomic64_read(&zram_wm_ratio);
}

static inline u64 get_compress_ratio_value(void)
{
	return atomic64_read(&compress_ratio);
}

static inline unsigned int get_avail_buffers_value(void)
{
	return atomic_read(&avail_buffers);
}

static inline unsigned int get_min_avail_buffers_value(void)
{
	return atomic_read(&min_avail_buffers);
}

static inline unsigned int get_high_avail_buffers_value(void)
{
	return atomic_read(&high_avail_buffers);
}

static inline u64 get_swapd_max_reclaim_size(void)
{
	return atomic_read(&max_reclaim_size);
}

static inline u64 get_free_swap_threshold_value(void)
{
	return atomic64_read(&free_swap_threshold);
}

static inline unsigned long long get_empty_round_skip_interval_value(void)
{
	return atomic64_read(&empty_round_skip_interval);
}

static inline unsigned long long get_max_skip_interval_value(void)
{
	return atomic64_read(&max_skip_interval);
}

static inline unsigned long long get_empty_round_check_threshold_value(void)
{
	return atomic64_read(&empty_round_check_threshold);
}

static inline u64 get_zram_critical_threshold_value(void)
{
	return atomic64_read(&zram_crit_thres);
}

static inline u64 get_cpuload_threshold_value(void)
{
	return atomic64_read(&cpuload_threshold);
}

static ssize_t avail_buffers_params_write(struct kernfs_open_file *of,
					  char *buf, size_t nbytes, loff_t off)
{
	unsigned int avail_buffers_value;
	unsigned int min_avail_buffers_value;
	unsigned int high_avail_buffers_value;
	u64 free_swap_threshold_value;

	buf = strstrip(buf);

	if (sscanf(buf, "%u %u %u %llu",
		   &avail_buffers_value,
		   &min_avail_buffers_value,
		   &high_avail_buffers_value,
		   &free_swap_threshold_value) != 4)
		return -EINVAL;

	atomic_set(&avail_buffers, avail_buffers_value);
	atomic_set(&min_avail_buffers, min_avail_buffers_value);
	atomic_set(&high_avail_buffers, high_avail_buffers_value);
	atomic64_set(&free_swap_threshold,
		     (free_swap_threshold_value * (SZ_1M / PAGE_SIZE)));

	wake_up_all_hybridswapds();

	return nbytes;
}

static int avail_buffers_params_show(struct seq_file *m, void *v)
{
	seq_printf(m, "avail_buffers: %u\n",
		   atomic_read(&avail_buffers));
	seq_printf(m, "min_avail_buffers: %u\n",
		   atomic_read(&min_avail_buffers));
	seq_printf(m, "high_avail_buffers: %u\n",
		   atomic_read(&high_avail_buffers));
	seq_printf(m, "free_swap_threshold: %llu\n",
		   (atomic64_read(&free_swap_threshold) * PAGE_SIZE / SZ_1M));

	return 0;
}

static ssize_t swapd_max_reclaim_size_write(struct kernfs_open_file *of,
					    char *buf, size_t nbytes, loff_t off)
{
	const unsigned int base = 10;
	u32 max_reclaim_size_value;
	int ret;

	buf = strstrip(buf);
	ret = kstrtouint(buf, base, &max_reclaim_size_value);
	if (ret)
		return -EINVAL;

	atomic_set(&max_reclaim_size, max_reclaim_size_value);

	return nbytes;
}

static int swapd_max_reclaim_size_show(struct seq_file *m, void *v)
{
	seq_printf(m, "swapd_max_reclaim_size: %u\n",
		   atomic_read(&max_reclaim_size));

	return 0;
}

static int empty_round_skip_interval_write(struct cgroup_subsys_state *css,
					   struct cftype *cft, s64 val)
{
	if (val < 0)
		return -EINVAL;

	atomic64_set(&empty_round_skip_interval, val);

	return 0;
}

static s64 empty_round_skip_interval_read(struct cgroup_subsys_state *css,
					  struct cftype *cft)
{
	return atomic64_read(&empty_round_skip_interval);
}

static int max_skip_interval_write(struct cgroup_subsys_state *css,
				   struct cftype *cft, s64 val)
{
	if (val < 0)
		return -EINVAL;

	atomic64_set(&max_skip_interval, val);

	return 0;
}

static s64 max_skip_interval_read(struct cgroup_subsys_state *css,
				  struct cftype *cft)
{
	return atomic64_read(&max_skip_interval);
}

static int empty_round_check_threshold_write(struct cgroup_subsys_state *css,
					     struct cftype *cft, s64 val)
{
	if (val < 0)
		return -EINVAL;

	atomic64_set(&empty_round_check_threshold, val);

	return 0;
}

static s64 empty_round_check_threshold_read(struct cgroup_subsys_state *css,
					    struct cftype *cft)
{
	return atomic64_read(&empty_round_check_threshold);
}


static int zram_critical_thres_write(struct cgroup_subsys_state *css,
				     struct cftype *cft, s64 val)
{
	if (val < 0)
		return -EINVAL;

	atomic64_set(&zram_crit_thres, val << (20 - PAGE_SHIFT));

	return 0;
}

static s64 zram_critical_thres_read(struct cgroup_subsys_state *css,
				    struct cftype *cft)
{
	return atomic64_read(&zram_crit_thres) >> (20 - PAGE_SHIFT);
}

static s64 cpuload_threshold_read(struct cgroup_subsys_state *css,
				  struct cftype *cft)

{
	return atomic64_read(&cpuload_threshold);
}

static int cpuload_threshold_write(struct cgroup_subsys_state *css,
				   struct cftype *cft, s64 val)
{
	if (val < 0)
		return -EINVAL;

	atomic64_set(&cpuload_threshold, val);

	return 0;
}

static ssize_t swapd_pressure_event_control(struct kernfs_open_file *of,
					    char *buf, size_t nbytes, loff_t off)
{
	int efd;
	unsigned int level;
	struct fd efile;
	int ret;

	buf = strstrip(buf);
	if (sscanf(buf, "%d %u", &efd, &level) != 2)
		return -EINVAL;

	if (level >= LEVEL_COUNT)
		return -EINVAL;

	if (efd < 0)
		return -EBADF;

	mutex_lock(&pressure_event_lock);
	efile = fdget(efd);
	if (!efile.file) {
		ret = -EBADF;
		goto out;
	}
	swapd_press_efd[level] = eventfd_ctx_fileget(efile.file);
	if (IS_ERR(swapd_press_efd[level])) {
		ret = PTR_ERR(swapd_press_efd[level]);
		goto out_put_efile;
	}
	fdput(efile);
	mutex_unlock(&pressure_event_lock);
	return nbytes;

out_put_efile:
	fdput(efile);
out:
	mutex_unlock(&pressure_event_lock);

	return ret;
}

static void swapd_pressure_report(enum swapd_pressure_level level)
{
	int ret;

	if (swapd_press_efd[level] == NULL)
		return;

	ret = eventfd_signal(swapd_press_efd[level], 1);
	log_info("SWAP-MM: level:%u, ret:%d ", level, ret);
}

static s64 swapd_pid_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	return swapd_pid;
}

static void swapd_memcgs_param_parse(int level_num)
{
	struct mem_cgroup *memcg = NULL;
	memcg_hybs_t *hybs = NULL;
	int i;

	while ((memcg = get_next_memcg(memcg))) {
		hybs = MEMCGRP_ITEM_DATA(memcg);

		for (i = 0; i < level_num; ++i) {
			if (atomic64_read(&hybs->app_score) >= zswap_param[i].min_score &&
			    atomic64_read(&hybs->app_score) <= zswap_param[i].max_score)
				break;
		}
		atomic_set(&hybs->ub_mem2zram_ratio, zswap_param[i].ub_mem2zram_ratio);
		atomic_set(&hybs->ub_zram2ufs_ratio, zswap_param[i].ub_zram2ufs_ratio);
		atomic_set(&hybs->refault_threshold, zswap_param[i].refault_threshold);
	}
}

static void update_swapd_memcg_hybs(memcg_hybs_t *hybs)
{
	int i;

	for (i = 0; i < SWAPD_MAX_LEVEL_NUM; ++i) {
		if (!zswap_param[i].min_score && !zswap_param[i].max_score)
			return;

		if (atomic64_read(&hybs->app_score) >= zswap_param[i].min_score &&
		    atomic64_read(&hybs->app_score) <= zswap_param[i].max_score)
			break;
	}

	if (i == SWAPD_MAX_LEVEL_NUM)
		return;

	atomic_set(&hybs->ub_mem2zram_ratio, zswap_param[i].ub_mem2zram_ratio);
	atomic_set(&hybs->ub_zram2ufs_ratio, zswap_param[i].ub_zram2ufs_ratio);
	atomic_set(&hybs->refault_threshold, zswap_param[i].refault_threshold);
}

static void update_swapd_memcg_param(struct mem_cgroup *memcg)
{
	memcg_hybs_t *hybs = MEMCGRP_ITEM_DATA(memcg);

	if (!hybs)
		return;

	update_swapd_memcg_hybs(hybs);
}

static int update_swapd_memcgs_param(char *buf)
{
	static const char delim[] = " ";
	char *token = NULL;
	int level_num;
	int i;

	buf = strstrip(buf);
	token = strsep(&buf, delim);

	if (!token)
		return -EINVAL;

	if (kstrtoint(token, 0, &level_num))
		return -EINVAL;

	if (level_num > SWAPD_MAX_LEVEL_NUM || level_num < 0)
		return -EINVAL;

	log_warn("%s\n", buf);

	mutex_lock(&reclaim_para_lock);
	for (i = 0; i < level_num; ++i) {
		token = strsep(&buf, delim);
		if (!token)
			goto out;

		if (kstrtoint(token, 0, &zswap_param[i].min_score) ||
		    zswap_param[i].min_score > MAX_APP_SCORE)
			goto out;

		token = strsep(&buf, delim);
		if (!token)
			goto out;

		if (kstrtoint(token, 0, &zswap_param[i].max_score) ||
		    zswap_param[i].max_score > MAX_APP_SCORE)
			goto out;

		token = strsep(&buf, delim);
		if (!token)
			goto out;

		if (kstrtoint(token, 0, &zswap_param[i].ub_mem2zram_ratio) ||
		    zswap_param[i].ub_mem2zram_ratio > MAX_RATIO)
			goto out;

		token = strsep(&buf, delim);
		if (!token)
			goto out;

		if (kstrtoint(token, 0, &zswap_param[i].ub_zram2ufs_ratio) ||
		    zswap_param[i].ub_zram2ufs_ratio > MAX_RATIO)
			goto out;

		token = strsep(&buf, delim);
		if (!token)
			goto out;

		if (kstrtoint(token, 0, &zswap_param[i].refault_threshold))
			goto out;
	}

	swapd_memcgs_param_parse(level_num);
	mutex_unlock(&reclaim_para_lock);
	return 0;

out:
	mutex_unlock(&reclaim_para_lock);
	return -EINVAL;
}

static ssize_t swapd_memcgs_param_write(struct kernfs_open_file *of, char *buf,
					size_t nbytes, loff_t off)
{
	int ret = update_swapd_memcgs_param(buf);

	return ret ? ret : nbytes;
}

static int swapd_memcgs_param_show(struct seq_file *m, void *v)
{
	int i;

	for (i = 0; i < SWAPD_MAX_LEVEL_NUM; ++i) {
		seq_printf(m, "level %d min score: %u\n",
			   i, zswap_param[i].min_score);
		seq_printf(m, "level %d max score: %u\n",
			   i, zswap_param[i].max_score);
		seq_printf(m, "level %d ub_mem2zram_ratio: %u\n",
			   i, zswap_param[i].ub_mem2zram_ratio);
		seq_printf(m, "level %d ub_zram2ufs_ratio: %u\n",
			   i, zswap_param[i].ub_zram2ufs_ratio);
		seq_printf(m, "memcg %d refault_threshold: %u\n",
			   i, zswap_param[i].refault_threshold);
	}

	return 0;
}

static ssize_t swapd_nap_jiffies_write(struct kernfs_open_file *of, char *buf,
				       size_t nbytes, loff_t off)
{
	unsigned long nap;

	buf = strstrip(buf);
	if (!buf)
		return -EINVAL;

	if (kstrtoul(buf, 0, &nap))
		return -EINVAL;

	swapd_nap_jiffies = nap;
	return nbytes;
}

static int swapd_nap_jiffies_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%lu\n", swapd_nap_jiffies);

	return 0;
}

static ssize_t swapd_single_memcg_param_write(struct kernfs_open_file *of,
					      char *buf, size_t nbytes, loff_t off)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(of_css(of));
	unsigned int ub_mem2zram_ratio;
	unsigned int ub_zram2ufs_ratio;
	unsigned int refault_threshold;
	memcg_hybs_t *hybs = MEMCGRP_ITEM_DATA(memcg);

	if (!hybs)
		return -EINVAL;

	buf = strstrip(buf);

	if (sscanf(buf, "%u %u %u", &ub_mem2zram_ratio, &ub_zram2ufs_ratio,
		   &refault_threshold) != 3)
		return -EINVAL;

	if (ub_mem2zram_ratio > MAX_RATIO || ub_zram2ufs_ratio > MAX_RATIO)
		return -EINVAL;

	log_warn("%u %u %u\n",
		 ub_mem2zram_ratio, ub_zram2ufs_ratio, refault_threshold);

	atomic_set(&MEMCGRP_ITEM(memcg, ub_mem2zram_ratio), ub_mem2zram_ratio);
	atomic_set(&MEMCGRP_ITEM(memcg, ub_zram2ufs_ratio), ub_zram2ufs_ratio);
	atomic_set(&MEMCGRP_ITEM(memcg, refault_threshold), refault_threshold);

	return nbytes;
}


static int swapd_single_memcg_param_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = mem_cgroup_from_css(seq_css(m));
	memcg_hybs_t *hybs = MEMCGRP_ITEM_DATA(memcg);

	if (!hybs)
		return -EINVAL;

	seq_printf(m, "memcg score: %lld\n",
		   atomic64_read(&hybs->app_score));
	seq_printf(m, "memcg ub_mem2zram_ratio: %u\n",
		   atomic_read(&hybs->ub_mem2zram_ratio));
	seq_printf(m, "memcg ub_zram2ufs_ratio: %u\n",
		   atomic_read(&hybs->ub_zram2ufs_ratio));
	seq_printf(m, "memcg refault_threshold: %u\n",
		   atomic_read(&hybs->refault_threshold));

	return 0;
}

static int mem_cgroup_zram_wm_ratio_write(struct cgroup_subsys_state *css,
					  struct cftype *cft, s64 val)
{
	if (val > MAX_RATIO || val < MIN_RATIO)
		return -EINVAL;

	atomic64_set(&zram_wm_ratio, val);

	return 0;
}

static s64 mem_cgroup_zram_wm_ratio_read(struct cgroup_subsys_state *css,
					 struct cftype *cft)
{
	return atomic64_read(&zram_wm_ratio);
}

static int mem_cgroup_compress_ratio_write(struct cgroup_subsys_state *css,
					   struct cftype *cft, s64 val)
{
	if (val > MAX_RATIO || val < MIN_RATIO)
		return -EINVAL;

	atomic64_set(&compress_ratio, val);

	return 0;
}

static s64 mem_cgroup_compress_ratio_read(struct cgroup_subsys_state *css,
					  struct cftype *cft)
{
	return atomic64_read(&compress_ratio);
}

static int memcg_active_app_info_list_show(struct seq_file *m, void *v)
{
	struct mem_cgroup *memcg = NULL;
	unsigned long anon_size;
	unsigned long zram_size;
	unsigned long eswap_size;

	while ((memcg = get_next_memcg(memcg))) {
		u64 score;

		if (!MEMCGRP_ITEM_DATA(memcg))
			continue;

		score = atomic64_read(&MEMCGRP_ITEM(memcg, app_score));
		anon_size = memcg_anon_pages(memcg);
		eswap_size = hybridswap_read_memcg_stats(memcg,
							 MCG_DISK_STORED_PG_SZ);
		zram_size = hybridswap_read_memcg_stats(memcg,
							MCG_ZRAM_STORED_PG_SZ);

		if (anon_size + zram_size + eswap_size == 0)
			continue;

		if (!strlen(MEMCGRP_ITEM(memcg, name)))
			continue;

		anon_size *= PAGE_SIZE / SZ_1K;
		zram_size *= PAGE_SIZE / SZ_1K;
		eswap_size *= PAGE_SIZE / SZ_1K;

		seq_printf(m, "%s %llu %lu %lu %lu %llu\n",
			   MEMCGRP_ITEM(memcg, name), score,
			   anon_size, zram_size, eswap_size,
			   MEMCGRP_ITEM(memcg, reclaimed_pagefault));
	}
	return 0;
}

static unsigned long get_totalreserve_pages(void)
{
	int nid;
	unsigned long val = 0;

	for_each_node_state(nid, N_MEMORY) {
		pg_data_t *pgdat = NODE_DATA(nid);

		if (pgdat)
			val += pgdat->totalreserve_pages;
	}

	return val;
}

static struct pglist_data *first_online_pgdat_dup(void)
{
	return NODE_DATA(first_online_node);
}

static struct pglist_data *next_online_pgdat_dup(struct pglist_data *pgdat)
{
	int nid = next_online_node(pgdat->node_id);

	if (nid == MAX_NUMNODES)
		return NULL;
	return NODE_DATA(nid);
}

static struct zone *next_zone_dup(struct zone *zone)
{
	pg_data_t *pgdat = zone->zone_pgdat;

	if (zone < pgdat->node_zones + MAX_NR_ZONES - 1)
		zone++;
	else {
		pgdat = next_online_pgdat_dup(pgdat);
		if (pgdat)
			zone = pgdat->node_zones;
		else
			zone = NULL;
	}
	return zone;
}

#define for_each_zone_dup(zone)			        \
	for (zone = (first_online_pgdat_dup())->node_zones; \
		zone;					\
		zone = next_zone_dup(zone))

static unsigned int system_cur_avail_buffers(void)
{
	unsigned long reclaimable;
	long buffers;
	unsigned long pagecache;
	unsigned long wmark_low = 0;
	struct zone *zone;
	int chp_pool_pages = 0;

	chp_pool_pages = chp_read_info_ext(CHP_EXT_CMD_POOL_PAGES);
	buffers = global_zone_page_state(NR_FREE_PAGES) - all_totalreserve_pages;

	for_each_zone_dup(zone)
		wmark_low += low_wmark_pages(zone);
	pagecache = global_node_page_state(NR_ACTIVE_FILE) +
		global_node_page_state(NR_INACTIVE_FILE);
	pagecache -= min(pagecache / 2, wmark_low);
	buffers += pagecache;

	reclaimable = global_node_page_state(NR_SLAB_RECLAIMABLE_B) +
		global_node_page_state(NR_KERNEL_MISC_RECLAIMABLE);
	reclaimable += chp_pool_pages;
	buffers += reclaimable - min(reclaimable / 2, wmark_low);

	if (buffers < 0)
		buffers = 0;

	return buffers >> 8; /* pages to MB */
}

static bool min_buffer_is_suitable(void)
{
	u32 curr_buffers = system_cur_avail_buffers();

	if (curr_buffers >= get_min_avail_buffers_value())
		return true;

	return false;
}

static bool buffer_is_suitable(void)
{
	u32 curr_buffers = system_cur_avail_buffers();

	if (curr_buffers >= get_avail_buffers_value())
		return true;

	return false;
}

static int reclaim_exceed_sleep_ms_write(struct cgroup_subsys_state *css,
					 struct cftype *cft, s64 val)
{
	if (val < 0)
		return -EINVAL;

	reclaim_exceed_sleep_ms = val;

	return 0;
}

static s64 reclaim_exceed_sleep_ms_read(struct cgroup_subsys_state *css,
					struct cftype *cft)
{
	return reclaim_exceed_sleep_ms;
}

static int max_reclaimin_size_mb_write(struct cgroup_subsys_state *css,
				       struct cftype *cft, u64 val)
{
	max_reclaimin_size = (val << 20);

	return 0;
}

static u64 max_reclaimin_size_mb_read(
				      struct cgroup_subsys_state *css, struct cftype *cft)
{
	return max_reclaimin_size >> 20;
}

static ssize_t swapd_shrink_parameter_write(struct kernfs_open_file *of,
					    char *buf, size_t nbytes, loff_t off)
{
	unsigned long window, limit;

	buf = strstrip(buf);
	if (sscanf(buf, "%lu %lu", &window, &limit) != 2)
		return -EINVAL;

	swapd_shrink_window = msecs_to_jiffies(window);
	swapd_shrink_limit_per_window = limit;

	return nbytes;
}

static int swapd_shrink_parameter_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%-32s %lu(jiffies) %u(msec)\n", "swapd_shrink_window",
		   swapd_shrink_window, jiffies_to_msecs(swapd_shrink_window));
	seq_printf(m, "%-32s %lu MB\n", "swapd_shrink_limit_per_window",
		   swapd_shrink_limit_per_window);
	seq_printf(m, "%-32s %u msec\n", "swapd_last_window",
		   jiffies_to_msecs(jiffies - swapd_last_window_start));
	seq_printf(m, "%-32s %lu MB\n", "swapd_last_window_shrink",
		   swapd_last_window_shrink);

	return 0;
}

static int zram_used_limit_mb_write(struct cgroup_subsys_state *css,
				    struct cftype *cft, s64 val)
{
	zram_used_limit_pages = (val << 20) >> PAGE_SHIFT;
	return 0;
}

static s64 zram_used_limit_mb_read(struct cgroup_subsys_state *css,
				   struct cftype *cft)
{
	return (zram_used_limit_pages << PAGE_SHIFT) >> 20;
}

static int chp_per_reclaim_mib_write(struct cgroup_subsys_state *css,
				     struct cftype *cft, s64 val)
{
	if (val < BATCH_PER_CYCLE_MB || val > 1024)
		return 0;

	chp_per_reclaim_mib = (unsigned int)val;
	return 0;
}

static s64 chp_per_reclaim_mib_read(struct cgroup_subsys_state *css,
				    struct cftype *cft)
{
	return chp_per_reclaim_mib;
}

static struct cftype mem_cgroup_swapd_legacy_files[] = {
	{
		.name = "active_app_info_list",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.seq_show = memcg_active_app_info_list_show,
	},
	{
		.name = "zram_wm_ratio",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = mem_cgroup_zram_wm_ratio_write,
		.read_s64 = mem_cgroup_zram_wm_ratio_read,
	},
	{
		.name = "compress_ratio",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = mem_cgroup_compress_ratio_write,
		.read_s64 = mem_cgroup_compress_ratio_read,
	},
	{
		.name = "swapd_pressure",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write = swapd_pressure_event_control,
	},
	{
		.name = "swapd_pid",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.read_s64 = swapd_pid_read,
	},
	{
		.name = "chp_per_reclaim_mib",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.read_s64 = chp_per_reclaim_mib_read,
		.write_s64 = chp_per_reclaim_mib_write,
	},
	{
		.name = "avail_buffers",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write = avail_buffers_params_write,
		.seq_show = avail_buffers_params_show,
	},
	{
		.name = "swapd_max_reclaim_size",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write = swapd_max_reclaim_size_write,
		.seq_show = swapd_max_reclaim_size_show,
	},
	{
		.name = "empty_round_skip_interval",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = empty_round_skip_interval_write,
		.read_s64 = empty_round_skip_interval_read,
	},
	{
		.name = "max_skip_interval",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = max_skip_interval_write,
		.read_s64 = max_skip_interval_read,
	},
	{
		.name = "empty_round_check_threshold",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = empty_round_check_threshold_write,
		.read_s64 = empty_round_check_threshold_read,
	},
	{
		.name = "swapd_memcgs_param",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write = swapd_memcgs_param_write,
		.seq_show = swapd_memcgs_param_show,
	},
	{
		.name = "swapd_single_memcg_param",
		.write = swapd_single_memcg_param_write,
		.seq_show = swapd_single_memcg_param_show,
	},
	{
		.name = "zram_critical_threshold",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = zram_critical_thres_write,
		.read_s64 = zram_critical_thres_read,
	},
	{
		.name = "cpuload_threshold",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = cpuload_threshold_write,
		.read_s64 = cpuload_threshold_read,
	},
	{
		.name = "reclaim_exceed_sleep_ms",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = reclaim_exceed_sleep_ms_write,
		.read_s64 = reclaim_exceed_sleep_ms_read,
	},
	{
		.name = "max_reclaimin_size_mb",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_u64 = max_reclaimin_size_mb_write,
		.read_u64 = max_reclaimin_size_mb_read,
	},
	{
		.name = "zram_used_limit_mb",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write_s64 = zram_used_limit_mb_write,
		.read_s64 = zram_used_limit_mb_read,
	},
	{
		.name = "swapd_shrink_parameter",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write = swapd_shrink_parameter_write,
		.seq_show = swapd_shrink_parameter_show,
	},
	{
		.name = "swapd_nap_jiffies",
		.flags = CFTYPE_ONLY_ON_ROOT,
		.write = swapd_nap_jiffies_write,
		.seq_show = swapd_nap_jiffies_show,
	},
	{ }, /* terminate */
};

static unsigned long zram_used_pages(void)
{
	int i;
	unsigned long ret = 0;

	for (i = 0; i < ZRAM_TYPE_MAX; i++)
		ret += zram_page_state(zram_arr[i], ZRAM_STATE_USED);
	return ret;
}

static unsigned long zram_compressed_pages(void)
{
	int i;
	unsigned long ret = 0;

	for (i = 0; i < ZRAM_TYPE_MAX; i++)
		ret += zram_page_state(zram_arr[i], ZRAM_STATE_COMPRESSED_PAGE);
	return ret;
}

static unsigned long zram_total_pages(void)
{
	int i;
	unsigned long nr_zram = 0;

	if (likely(zram_used_limit_pages))
		return zram_used_limit_pages;

	for (i = 0; i < ZRAM_TYPE_MAX; i++)
		nr_zram += zram_arr[i]->disksize >> PAGE_SHIFT;
	return nr_zram ?: 1;
}

static bool zram_is_full(struct zram *zram)
{
	unsigned long nr_used, nr_total;

	nr_used = zram_page_state(zram, ZRAM_STATE_USED);
	nr_total = zram_page_state(zram, ZRAM_STATE_TOTAL);
	return (nr_total - nr_used) < nr_total >> 6;
}

static bool zram_watermark_ok(void)
{
	long long diff_buffers;
	long long wm = 0;
	long long cur_ratio = 0;
	unsigned long zram_used = zram_used_pages();
	const unsigned int percent_constant = 100;

	diff_buffers = get_high_avail_buffers_value() -
		system_cur_avail_buffers();
	diff_buffers *= SZ_1M / PAGE_SIZE;
	diff_buffers *= get_compress_ratio_value() / 10;
	diff_buffers = diff_buffers * percent_constant / zram_total_pages();

	cur_ratio = zram_used * percent_constant / zram_total_pages();
	wm  = min(get_zram_wm_ratio_value(),
		  get_zram_wm_ratio_value() - diff_buffers);

	return cur_ratio > wm;
}

static bool free_zram_is_ok(void)
{
	unsigned long nr_used, nr_tot, nr_rsv;

	nr_tot = zram_total_pages();
	nr_used = zram_used_pages();
	nr_rsv = max(nr_tot >> 6, (unsigned long)SZ_256M >> PAGE_SHIFT);

	return nr_used < (nr_tot - nr_rsv);
}

static bool zram_watermark_exceed(void)
{
	u64 nr_zram_used;
	u64 nr_wm = get_zram_critical_threshold_value();

	if (!nr_wm)
		return false;

	nr_zram_used = zram_used_pages();

	if (nr_zram_used > nr_wm)
		return true;

	return false;
}


#ifdef CONFIG_OPLUS_JANK
static bool is_cpu_busy(void)
{
	unsigned int cpuload = 0;
	int i;
	struct cpumask mask;

	cpumask_clear(&mask);

	for (i = 0; i < 6; i++)
		cpumask_set_cpu(i, &mask);

	cpuload = get_cpu_load(1, &mask);
	if (cpuload > get_cpuload_threshold_value()) {
		log_info("cpuload %d\n", cpuload);
		return true;
	}

	return false;
}
#endif

enum reclaim_type {
	RT_NONE,
	RT_PAGE,
	RT_CHP,
	NR_RT_TYPE,
};

char *reclaim_type_text[NR_RT_TYPE] = {
	"->NONE",
	"->P",
	"->CHP"
};

#define INBALANCE_BASE_FACTOR 30
#define CHP_OVER_COMPRESSED_RATIO 150 /* zram1 vs anon hugepages: 1.5 vs 1*/
#define INBALANCE_CHP_FACTOR 15
#define SHALLOW_CMA_POOL_FACTOR 2 /* cma pool is less than 2 * high */

static inline int get_hybridswapd_reclaim_type(void)
{
	int type = RT_NONE;
	unsigned long anon_pg, anon_chp_pg, swap_pg,
		      swap_chp_pg, base_factor, chp_factor,
		      pool_cma_count, pool_high, pool_total_cma_count;
	bool cma_pool_shallow, cma_pool_size_huge;

	anon_chp_pg = global_node_page_state(NR_ANON_THPS);
	swap_chp_pg = zram_page_state(zram_arr[ZRAM_TYPE_CHP],
				      ZRAM_STATE_USED);

	anon_pg = global_node_page_state(NR_ANON_MAPPED) - anon_chp_pg;
	swap_pg = zram_page_state(zram_arr[ZRAM_TYPE_BASEPAGE], ZRAM_STATE_USED);

	base_factor = swap_pg * 100 / (anon_pg + 1);
	chp_factor =  swap_chp_pg * 100 / (anon_chp_pg + 1);

	pool_cma_count = chp_read_info_ext(CHP_EXT_CMD_POOL_CMA_COUNT);
	pool_total_cma_count = chp_read_info_ext(CHP_EXT_CMD_POOL_TOTAL_CMA_COUNT);
	pool_high = chp_read_info_ext(CHP_EXT_CMD_POOL_WM_HIGH);

	cma_pool_shallow = pool_cma_count < SHALLOW_CMA_POOL_FACTOR * pool_high;
	cma_pool_size_huge = pool_cma_count > pool_total_cma_count / 3;

	if (!cma_pool_size_huge &&
	    ((cma_pool_shallow && chp_factor < CHP_OVER_COMPRESSED_RATIO) ||
	    (long)(base_factor - chp_factor) > INBALANCE_CHP_FACTOR))
		type =  RT_CHP;
	else if ((long)(chp_factor - base_factor) > INBALANCE_BASE_FACTOR)
		type =  RT_PAGE;

	if (hybridswap_loglevel() >= HS_LOG_DEBUG) {
		trace_printk("@ %s:%d total_cma_count/3: %lu anon_pg: %lu anon_chp_pg: %lu swap_pg: %lu "
			     "swap_chp_pg: %lu base_factor:%lu chp_factor:%lu type:%s cma_count:%lu 2*high:%lu cma_pool_shallow:%d "
			     "cma_pool_size_huge:%d (base_factor - chp_factor):%ld (chp_factor - base_factor):%ld @\n",
			     __func__, __LINE__, pool_total_cma_count / 3,
			     anon_pg, anon_chp_pg, swap_pg, swap_chp_pg, base_factor,
			     chp_factor,  reclaim_type_text[type], pool_cma_count,
			     SHALLOW_CMA_POOL_FACTOR * pool_high,
			     cma_pool_shallow, cma_pool_size_huge,
			     (long)(base_factor - chp_factor),
			     (long)(chp_factor - base_factor));
	}

	return type;
}

static void wakeup_hybridswapd(pg_data_t *pgdat)
{
	unsigned long curr_interval;
	struct hybridswapd_task *hyb_task = PGDAT_ITEM_DATA(pgdat);

	if (!hyb_task || !hyb_task->swapd)
		return;

	if (atomic_read(&swapd_pause)) {
		count_swapd_event(SWAPD_MANUAL_PAUSE);
		return;
	}

	if (atomic_read(&display_off))
		return;

#ifdef CONFIG_OPLUS_JANK
	if (is_cpu_busy()) {
		count_swapd_event(SWAPD_CPU_BUSY_BREAK_TIMES);
		return;
	}
#endif

	if (!waitqueue_active(&hyb_task->swapd_wait))
		return;

	if (!free_zram_is_ok())
		return;

	if (get_hybridswapd_reclaim_type() == RT_NONE &&
	    min_buffer_is_suitable()) {
		count_swapd_event(SWAPD_OVER_MIN_BUFFER_SKIP_TIMES);
		return;
	}

	curr_interval = jiffies_to_msecs(jiffies - last_swapd_time);
	if (curr_interval < swapd_skip_interval) {
		count_swapd_event(SWAPD_EMPTY_ROUND_SKIP_TIMES);
		return;
	}

	atomic_set(&hyb_task->swapd_wait_flag, 1);
	wake_up_interruptible(&hyb_task->swapd_wait);
}

static void wake_up_all_hybridswapds(void)
{
	pg_data_t *pgdat = NULL;
	int nid;

	for_each_online_node(nid) {
		pgdat = NODE_DATA(nid);
		wakeup_hybridswapd(pgdat);
	}
}

static bool free_swap_is_low(void)
{
	struct sysinfo info;

	si_swapinfo(&info);

	return (info.freeswap < get_free_swap_threshold_value());
}

static unsigned long calc_each_memcg_pages(int type)
{
	struct mem_cgroup *memcg = NULL;
	struct chp_lruvec *lruvec;
	unsigned long global_reclaimed = 0;
	struct mem_cgroup_per_node *mz;

	while ((memcg = get_next_memcg(memcg))) {
		unsigned long nr = 0;
		int zid;
		memcg_hybs_t *hybs;

		hybs = MEMCGRP_ITEM_DATA(memcg);
		if (!hybs)
			continue;

		switch (type) {
		case RT_PAGE:
			mz = memcg->nodeinfo[0];
			for (zid = 0; zid < MAX_NR_ZONES; zid++)
				nr += READ_ONCE(mz->lru_zone_size[zid][LRU_INACTIVE_ANON]) +
					READ_ONCE(mz->lru_zone_size[zid][LRU_ACTIVE_ANON]);
			break;
		case RT_CHP:
			lruvec = (struct chp_lruvec *)memcg->deferred_split_queue.split_queue_len;
			for (zid = 0; zid < MAX_NR_ZONES; zid++)
				nr += READ_ONCE(lruvec->lru_zone_size[zid][LRU_INACTIVE_ANON]) +
					READ_ONCE(lruvec->lru_zone_size[zid][LRU_ACTIVE_ANON]);
			break;
		}
		hybs->can_reclaimed = nr;
		global_reclaimed += nr;
	}
	return global_reclaimed;
}

static unsigned long shrink_memcg_chp_pages(void)
{
	struct mem_cgroup *memcg = NULL;
	unsigned long tot_reclaimed = 0;
	long nr_to_reclaim = chp_per_reclaim_mib * (SZ_1M >> PAGE_SHIFT);
	unsigned long batch_per_cycle = BATCH_PER_CYCLE_MB * (SZ_1M >> PAGE_SHIFT);
	unsigned long global_reclaimed = 0;
	unsigned long start_js = jiffies;
	unsigned long reclaim_cycles;
	gfp_t gfp_mask = GFP_KERNEL;
	int type = RT_PAGE;
	int zram_inx = 0;
	int min_cluster = SWAP_CLUSTER_MAX;

	type = get_hybridswapd_reclaim_type();
	/* available mem is low, relaim nomal page! */
	if (type == RT_NONE)
		type = RT_PAGE;
	else if (type == RT_CHP) {
		zram_inx = 1;
		gfp_mask |= POOL_USER_ALLOC;
		min_cluster = CHP_SWAP_CLUSTER_MAX;
	}

	if (zram_is_full(zram_arr[zram_inx]))
		goto out;

	global_reclaimed = calc_each_memcg_pages(type);
	if (unlikely(global_reclaimed < batch_per_cycle)) {
		log_err("global_reclaimed:%lu < %lu\n", global_reclaimed,
			batch_per_cycle);
		goto out;
	}

	nr_to_reclaim = min(nr_to_reclaim, (long)global_reclaimed);
	reclaim_cycles = nr_to_reclaim / batch_per_cycle;
again:
	while ((memcg = get_next_memcg(memcg))) {
		memcg_hybs_t *hybs;
		unsigned long nr_reclaimed, to_reclaim;

		hybs = MEMCGRP_ITEM_DATA(memcg);

		to_reclaim = batch_per_cycle * hybs->can_reclaimed / global_reclaimed;

		if (to_reclaim < min_cluster) {
			hybs->can_reclaimed = 0;
			continue;
		}

		nr_reclaimed = try_to_free_mem_cgroup_pages(memcg, to_reclaim,
							    gfp_mask, MEMCG_RECLAIM_MAY_SWAP);

		hybs->can_reclaimed -= nr_reclaimed;
		if (hybs->can_reclaimed < 0)
			hybs->can_reclaimed = 0;

		tot_reclaimed += nr_reclaimed;

		if (tot_reclaimed >= nr_to_reclaim) {
			get_next_memcg_break(memcg);
			goto out;
		}

		if (swapd_nap_jiffies && time_after_eq(jiffies, start_js + swapd_nap_jiffies)) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout((jiffies - start_js) * 2);
			start_js = jiffies;
		}
	}

	if (!zram_is_full(zram_arr[zram_inx]) && --reclaim_cycles)
		goto again;
out:
	log_info("t: %s nr_to_reclaim: %lu total_reclaimed: %lu global_reclaimed: %lu\n",
		 reclaim_type_text[type], nr_to_reclaim, tot_reclaimed, global_reclaimed);
	return tot_reclaimed;
}

static int swapd_update_cpumask(struct task_struct *tsk, char *buf,
		struct pglist_data *pgdat)
{
	int retval;
	struct cpumask temp_mask;
	const struct cpumask *cpumask = cpumask_of_node(pgdat->node_id);
	struct hybridswapd_task *hyb_task = PGDAT_ITEM_DATA(pgdat);

	if (unlikely(!hyb_task)) {
		log_err("set task %s cpumask %s node %d failed, "
			"hyb_task is NULL\n", tsk->comm, buf, pgdat->node_id);
		return -EINVAL;
	}

	cpumask_clear(&temp_mask);
	retval = cpulist_parse(buf, &temp_mask);
	if (retval < 0 || cpumask_empty(&temp_mask)) {
		log_err("%s are invalid, use default\n", buf);
		goto use_default;
	}

	if (!cpumask_subset(&temp_mask, cpu_present_mask)) {
		log_err("%s is not subset of cpu_present_mask, use default\n",
				buf);
		goto use_default;
	}

	if (!cpumask_subset(&temp_mask, cpumask)) {
		log_err("%s is not subset of cpumask, use default\n", buf);
		goto use_default;
	}

	set_cpus_allowed_ptr(tsk, &temp_mask);
	cpumask_copy(&hyb_task->swapd_bind_cpumask, &temp_mask);
	return 0;

use_default:
	if (cpumask_empty(&hyb_task->swapd_bind_cpumask))
		set_cpus_allowed_ptr(tsk, cpumask);
	return -EINVAL;
}

static int hybridswapd(void *p)
{
	const unsigned int increase_rate = 2;
	pg_data_t *pgdat = (pg_data_t *)p;
	struct task_struct *tsk = current;
	struct hybridswapd_task *hyb_task = PGDAT_ITEM_DATA(pgdat);
	unsigned long nr_reclaimed = 0;

	/* save swapd pid for schedule strategy */
	swapd_pid = tsk->pid;

	/* swapd do not runnint on super core */
	cpumask_clear(&hyb_task->swapd_bind_cpumask);
	(void)swapd_update_cpumask(tsk, SWAPD_DEFAULT_BIND_CPUS, pgdat);
	set_freezable();

	swapd_last_window_start = jiffies - swapd_shrink_window;
	while (!kthread_should_stop()) {
		wait_event_freezable(hyb_task->swapd_wait,
				     atomic_read(&hyb_task->swapd_wait_flag));
		atomic_set(&hyb_task->swapd_wait_flag, 0);
		if (unlikely(kthread_should_stop())) {
			log_err("hybridswapd exit\n");
			break;
		}
		count_swapd_event(SWAPD_WAKEUP);

		nr_reclaimed = shrink_memcg_chp_pages();
		last_swapd_time = jiffies;

		if (nr_reclaimed < get_empty_round_check_threshold_value()) {
			count_swapd_event(SWAPD_EMPTY_ROUND);
			if (last_round_is_empty)
				swapd_skip_interval = min(swapd_skip_interval *
						increase_rate,
						get_max_skip_interval_value());
			else
				swapd_skip_interval =
					get_empty_round_skip_interval_value();
			last_round_is_empty = true;
		} else {
			swapd_skip_interval = 0;
			last_round_is_empty = false;
		}

		if (!buffer_is_suitable()) {
			if (free_swap_is_low() || zram_watermark_exceed()) {
				swapd_pressure_report(LEVEL_CRITICAL);
				count_swapd_event(SWAPD_CRITICAL_PRESS);
			}
		}
	}
	return 0;
}

/*
 * This swapd start function will be called by init and node-hot-add.
 */
static int hybridswapd_run(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);
	struct hybridswapd_task *hyb_task = PGDAT_ITEM_DATA(pgdat);
	int ret;

	if (!hyb_task || hyb_task->swapd)
		return 0;

	atomic_set(&hyb_task->swapd_wait_flag, 0);
	hyb_task->swapd = kthread_create(hybridswapd, pgdat, "hybridswapd:%d",
					 nid);
	if (IS_ERR(hyb_task->swapd)) {
		log_err("Failed to start swapd on node %d\n", nid);
		ret = PTR_ERR(hyb_task->swapd);
		hyb_task->swapd = NULL;
		return ret;
	}

	wake_up_process(hyb_task->swapd);
	return 0;
}

/*
 * Called by memory hotplug when all memory in a node is offlined.  Caller must
 * hold mem_hotplug_begin/end().
 */
static void swapd_stop(int nid)
{
	struct pglist_data *pgdata = NODE_DATA(nid);
	struct task_struct *swapd;
	struct hybridswapd_task *hyb_task;

	if (unlikely(!PGDAT_ITEM_DATA(pgdata))) {
		log_err("nid %d pgdata %p PGDAT_ITEM_DATA is NULL\n",
			nid, pgdata);
		return;
	}
	log_err("exit\n");
	dump_stack();

	hyb_task = PGDAT_ITEM_DATA(pgdata);
	swapd = hyb_task->swapd;
	if (swapd) {
		atomic_set(&hyb_task->swapd_wait_flag, 1);
		kthread_stop(swapd);
		hyb_task->swapd = NULL;
	}

	swapd_pid = -1;
}

static int mem_hotplug_swapd_notifier(struct notifier_block *nb,
				      unsigned long action, void *data)
{
	struct memory_notify *arg = (struct memory_notify *)data;
	int nid = arg->status_change_nid;

	if (action == MEM_ONLINE)
		hybridswapd_run(nid);
	else if (action == MEM_OFFLINE)
		swapd_stop(nid);

	return NOTIFY_OK;
}

static struct notifier_block swapd_notifier_nb = {
	.notifier_call = mem_hotplug_swapd_notifier,
};

static int swapd_cpu_online(unsigned int cpu)
{
	int nid;

	for_each_node_state(nid, N_MEMORY) {
		pg_data_t *pgdat = NODE_DATA(nid);
		struct hybridswapd_task *hyb_task;
		struct cpumask *mask;

		hyb_task = PGDAT_ITEM_DATA(pgdat);
		mask = &hyb_task->swapd_bind_cpumask;

		if (cpumask_any_and(cpu_online_mask, mask) < nr_cpu_ids)
			/* One of our CPUs online: restore mask */
			set_cpus_allowed_ptr(PGDAT_ITEM(pgdat, swapd), mask);
	}
	return 0;
}

static void vh_tune_scan_type(void *data, enum scan_balance *s_balance)
{
	if (current_is_hybrid_swapd()) {
		*s_balance = SCAN_ANON;
		return;
	}

	/*triggered from user through force_shrink_anon*/
	if (current->flags & PF_SHRINK_ANON) {
		*s_balance = SCAN_ANON;
		return;
	}

	/*real zram full, scan file only*/
	if (!free_zram_is_ok()) {
		*s_balance = SCAN_FILE;
		return;
	}
}

static void vh_alloc_pages_slowpath(void *data, gfp_t gfp_flags,
				unsigned int order, unsigned long delta)
{
	if (gfp_flags & __GFP_KSWAPD_RECLAIM)
		wake_up_all_hybridswapds();
}

static int create_hybridswapd_thread(void)
{
	int nid;
	int ret;
	struct pglist_data *pgdat;
	struct hybridswapd_task *tsk_info;

	for_each_node(nid) {
		pgdat = NODE_DATA(nid);
		if (!PGDAT_ITEM_DATA(pgdat)) {
			tsk_info = kzalloc(sizeof(struct hybridswapd_task),
					   GFP_KERNEL);
			if (!tsk_info) {
				log_err("kmalloc tsk_info failed node %d\n", nid);
				goto error_out;
			}

			pgdat->android_oem_data1 = (u64)tsk_info;
		}

		init_waitqueue_head(&PGDAT_ITEM(pgdat, swapd_wait));
	}

	for_each_node_state(nid, N_MEMORY) {
		if (hybridswapd_run(nid))
			goto error_out;
	}

	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
					"mm/swapd:online", swapd_cpu_online, NULL);
	if (ret < 0) {
		log_err("swapd: failed to register hotplug callbacks.\n");
		goto error_out;
	}
	swapd_online = ret;

	return 0;

error_out:
	for_each_node(nid) {
		pgdat = NODE_DATA(nid);

		if (!PGDAT_ITEM_DATA(pgdat))
			continue;

		if (PGDAT_ITEM(pgdat, swapd)) {
			kthread_stop(PGDAT_ITEM(pgdat, swapd));
			PGDAT_ITEM(pgdat, swapd) = NULL;
		}

		kfree((void *)PGDAT_ITEM_DATA(pgdat));
		pgdat->android_oem_data1 = 0;
	}

	return -ENOMEM;
}

static void destroy_swapd_thread(void)
{
	int nid;
	struct pglist_data *pgdat;

	cpuhp_remove_state_nocalls(swapd_online);
	for_each_node(nid) {
		pgdat = NODE_DATA(nid);
		if (!PGDAT_ITEM_DATA(pgdat))
			continue;

		swapd_stop(nid);
		kfree((void *)PGDAT_ITEM_DATA(pgdat));
		pgdat->android_oem_data1 = 0;
	}
}

#if IS_ENABLED(CONFIG_DRM_PANEL_NOTIFY) || IS_ENABLED(CONFIG_QCOM_PANEL_EVENT_NOTIFIER)
static struct drm_panel *get_active_panel(void)
{
	int i;
	int count;
	struct device_node *panel_node = NULL;
	struct drm_panel *panel = NULL;
	struct device_node *np = NULL;

	np = of_find_node_by_name(NULL, "oplus,dsi-display-dev");
	if (!np) {
		log_err("oplus,dsi-display-dev node missing\n");
		return NULL;
	}

	log_warn("oplus,dsi-display-dev node found\n");
	count = of_count_phandle_with_args(np, "oplus,dsi-panel-primary", NULL);
	if (count <= 0) {
		log_err("oplus,dsi-panel-primary missing\n");
		goto not_found;
	}

	for (i = 0; i < count; i++) {
		panel_node = of_parse_phandle(np, "oplus,dsi-panel-primary", i);
		panel = of_drm_find_panel(panel_node);
		of_node_put(panel_node);
		if (!IS_ERR(panel)) {
			log_warn("active panel found\n");
			goto found;
		}
	}
not_found:
	panel = NULL;
found:
	of_node_put(np);
	return panel;
}

static void bright_fb_notifier_callback(enum panel_event_notifier_tag tag,
	struct panel_event_notification *notification, void *client_data)
{
	if (!notification) {
		log_info("%s, invalid notify\n", __func__);
		return;
	}

	switch (notification->notif_type) {
	case DRM_PANEL_EVENT_BLANK:
		atomic_set(&display_off, 1);
		break;
	case DRM_PANEL_EVENT_UNBLANK:
		atomic_set(&display_off, 0);
		break;
	default:
		break;
	}
}
#elif IS_ENABLED(CONFIG_DRM_MSM) || IS_ENABLED(CONFIG_DRM_OPLUS_NOTIFY)
static int bright_fb_notifier_callback(struct notifier_block *self,
				       unsigned long event, void *data)
{
	struct msm_drm_notifier *evdata = data;
	int *blank;

	if (evdata && evdata->data) {
		blank = evdata->data;

		if (*blank ==  MSM_DRM_BLANK_POWERDOWN)
			atomic_set(&display_off, 1);
		else if (*blank == MSM_DRM_BLANK_UNBLANK)
			atomic_set(&display_off, 0);
	}

	return NOTIFY_OK;
}
#elif IS_ENABLED(CONFIG_OPLUS_MTK_DRM_GKI_NOTIFY)
static int mtk_bright_fb_notifier_callback(struct notifier_block *self,
					   unsigned long event, void *data)
{
	int *blank = (int *)data;

	if (!blank) {
		log_err("get disp stat err, blank is NULL!\n");
		return 0;
	}

	if (*blank == MTK_DISP_BLANK_POWERDOWN)
		atomic_set(&display_off, 1);
	else if (*blank == MTK_DISP_BLANK_UNBLANK)
		atomic_set(&display_off, 0);
	return NOTIFY_OK;
}
#endif

static void register_panel_event_notifier(void)
{
#if IS_ENABLED(CONFIG_DRM_PANEL_NOTIFY) || IS_ENABLED(CONFIG_QCOM_PANEL_EVENT_NOTIFIER)
	struct drm_panel *active_panel;
	void *cookie;
	active_panel = get_active_panel();
	if (active_panel)
		cookie = panel_event_notifier_register(PANEL_EVENT_NOTIFICATION_PRIMARY,
			PANEL_EVENT_NOTIFIER_CLIENT_MM, active_panel, bright_fb_notifier_callback, NULL);

	if (active_panel && !IS_ERR(cookie)) {
		log_warn("register_panel_event_notifier success\n");
		g_panel_cookie = cookie;
	} else {
		log_err("register_panel_event_notifier failed. need fix\n");
	}
#elif IS_ENABLED(CONFIG_DRM_MSM) || IS_ENABLED(CONFIG_DRM_OPLUS_NOTIFY)
	fb_notif.notifier_call = bright_fb_notifier_callback;
	if (msm_drm_register_client(&fb_notif))
		log_err("msm_drm_register_client failed\n");
#elif IS_ENABLED(CONFIG_OPLUS_MTK_DRM_GKI_NOTIFY)
	fb_notif.notifier_call = mtk_bright_fb_notifier_callback;
	if (mtk_disp_notifier_register("Oplus_hybridswap", &fb_notif))
		log_err("mtk_disp_notifier_register failed\n");
#endif
}

static void unregister_panel_event_notifier(void)
{
#if IS_ENABLED(CONFIG_DRM_PANEL_NOTIFY) || IS_ENABLED(CONFIG_QCOM_PANEL_EVENT_NOTIFIER)
	if (g_panel_cookie) {
		panel_event_notifier_unregister(g_panel_cookie);
		g_panel_cookie = NULL;
	}
#elif IS_ENABLED(CONFIG_DRM_MSM) || IS_ENABLED(CONFIG_DRM_OPLUS_NOTIFY)
	msm_drm_unregister_client(&fb_notif);
#elif IS_ENABLED(CONFIG_OPLUS_MTK_DRM_GKI_NOTIFY)
	mtk_disp_notifier_unregister(&fb_notif);
#endif
}

static void swapd_pre_init(void)
{
	free_swap_is_low_fp = free_swap_is_low;
	all_totalreserve_pages = get_totalreserve_pages();
}

static void swapd_pre_deinit(void)
{
	all_totalreserve_pages = 0;
}

static int swapd_init(struct zram **zram)
{
	int ret;

	ret = register_memory_notifier(&swapd_notifier_nb);
	if (ret) {
		log_err("register_memory_notifier failed, ret = %d\n", ret);
		return ret;
	}

	register_panel_event_notifier();

	ret = create_hybridswapd_thread();
	if (ret) {
		log_err("create_hybridswapd_thread failed, ret=%d\n", ret);
		goto create_swapd_fail;
	}
	log_err("create hybridswapd thread\n");

	atomic_set(&swapd_enabled, 1);
	return 0;

create_swapd_fail:
	unregister_panel_event_notifier();
	unregister_memory_notifier(&swapd_notifier_nb);
	return ret;
}

static void swapd_exit(void)
{
	destroy_swapd_thread();
	unregister_panel_event_notifier();
	unregister_memory_notifier(&swapd_notifier_nb);
	atomic_set(&swapd_enabled, 0);
}

static bool hybridswap_swapd_enabled(void)
{
	return !!atomic_read(&swapd_enabled);
}

void hybridswapd_chp_ops_init(struct hybridswapd_operations *ops)
{
	ops->fault_out_pause = &fault_out_pause;
	ops->fault_out_pause_cnt = &fault_out_pause;
	ops->swapd_pause = &swapd_pause;

	ops->memcg_legacy_files = mem_cgroup_swapd_legacy_files;
	ops->update_memcg_param = update_swapd_memcg_param;

	ops->pre_init = swapd_pre_init;
	ops->pre_deinit = swapd_pre_deinit;

	ops->init = swapd_init;
	ops->deinit = swapd_exit;
	ops->enabled = hybridswap_swapd_enabled;

	ops->zram_total_pages = zram_total_pages;
	ops->zram_used_pages = zram_used_pages;
	ops->zram_compressed_pages = zram_compressed_pages;

	ops->free_zram_is_ok = free_zram_is_ok;
	ops->zram_watermark_ok = zram_watermark_ok;
	ops->wakeup_kthreads = wake_up_all_hybridswapds;

	ops->vh_alloc_pages_slowpath = vh_alloc_pages_slowpath;
	ops->vh_tune_scan_type = vh_tune_scan_type;
}
