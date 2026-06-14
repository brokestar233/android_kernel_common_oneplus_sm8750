// SPDX-License-Identifier: GPL-2.0-only
/*
 * zram optimization helpers integrated as part of zram core module.
 * Uses system memory usage as the PRIMARY signal for swap aggressiveness,
 * with zram health as a secondary modifier.  Target: <65% memory usage.
 * 75% is the yellow line, 80% is the absolute red line.
 */

#define pr_fmt(fmt) "zram_opt: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cgroup.h>
#include <linux/mm.h>
#include <linux/memcontrol.h>
#include <linux/proc_fs.h>
#include <linux/psi.h>
#include <linux/sched/rt.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/swap.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <trace/hooks/mm.h>
#include <trace/hooks/vmscan.h>

#include "bmavg.h"
#include "zram_drv.h"

static int g_direct_swappiness = 60;
static int g_swappiness = 160;

static int threshold1_vm_swappiness;
static int threshold2_vm_swappiness;
static int threshold1_swappiness_size;
static int threshold2_swappiness_size;
static struct proc_dir_entry *dynamic_swappiness_entry;

static int g_dynamic_direct_swappiness;
static int g_dynamic_direct_swappiness_threshold;
static struct proc_dir_entry *dynamic_direct_swappiness_entry;
#define check_swappiness(val) (((val) > 200) || ((val) < 0))
#define check_vm_threshold(val) ((val) < 0)

#define ZO_PRESSURE_LOW		0
#define ZO_PRESSURE_MEDIUM	1
#define ZO_PRESSURE_HIGH	2

#define ZO_TIER_ROOT		0
#define ZO_TIER_TOPAPP		1
#define ZO_TIER_FOREGROUND	2
#define ZO_TIER_BACKGROUND	3
#define ZO_TIER_SYSTEM		4

static int oplus_extra_free_kbytes;

#define PARA_BUF_LEN 512
static struct proc_dir_entry *para_entry;

static atomic64_t zo_swappiness_rewrites;
static atomic64_t zo_zero_swappiness_events;
static atomic64_t zo_direct_bypass_events;
static atomic64_t zo_balance_true_events;
static atomic64_t zo_balance_false_events;
static atomic64_t zo_pressure_low_events;
static atomic64_t zo_pressure_medium_events;
static atomic64_t zo_pressure_high_events;
static atomic64_t zo_scan_skip_events;
static atomic64_t zo_file_is_tiny_events;

static int zo_last_pressure = ZO_PRESSURE_LOW;
static int zo_last_tier = ZO_TIER_ROOT;
static int zo_last_swappiness;
static int zo_last_vm_swappiness = -1;
static bool zo_last_balance_anon_file_reclaim;
static unsigned long zo_last_fullness;
static unsigned long zo_last_huge_ratio;
static unsigned long zo_last_compress_pct;
static unsigned long zo_last_mem_usage_pct;
static struct bmavg_u16 zo_fullness_avg;
static struct bmavg_u16 zo_huge_ratio_avg;
static struct bmavg_u16 zo_compress_pct_avg;

/*
 * System memory usage thresholds.
 * <65% = target zone (green), 65-75% = elevated (orange),
 * 75% = yellow line, 80% = absolute red line.
 */
#define ZO_MEM_PCT_GREEN_CEIL	65
#define ZO_MEM_PCT_YELLOW	75
#define ZO_MEM_PCT_RED		80

/*
 * Zram health pressure thresholds (secondary signal).
 */
#define ZO_PRESSURE_HIGH_FULLNESS	90
#define ZO_PRESSURE_HIGH_FULLNESS_EXIT	82
#define ZO_PRESSURE_MEDIUM_FULLNESS	68
#define ZO_PRESSURE_MEDIUM_FULLNESS_EXIT	60
#define ZO_PRESSURE_HIGH_HUGE_RATIO	40
#define ZO_PRESSURE_HIGH_HUGE_RATIO_EXIT	32
#define ZO_PRESSURE_MEDIUM_HUGE_RATIO	25
#define ZO_PRESSURE_MEDIUM_HUGE_RATIO_EXIT	18
#define ZO_PRESSURE_MEDIUM_COMPRESS_PCT	140
#define ZO_PRESSURE_MEDIUM_COMPRESS_PCT_EXIT	150

#define ZO_TIER_KEYWORD_LEN	64

static char zo_topapp_keywords[ZO_TIER_KEYWORD_LEN] = "top-app";
static char zo_foreground_keywords[ZO_TIER_KEYWORD_LEN] = "foreground,fg";
static char zo_background_keywords[ZO_TIER_KEYWORD_LEN] = "background,bg";
static char zo_system_keywords[ZO_TIER_KEYWORD_LEN] = "system";

static int g_kswapd_pid = -1;
#define KSWAPD_COMM "kswapd0"

static int g_kcompactd_pid = -1;
#define KCOMPACTD_COMM "kcompactd0"

static bool zo_match_tier_keywords(const char *name, const char *keywords)
{
	char buf[ZO_TIER_KEYWORD_LEN];
	char *cur;
	char *token;

	if (!name || !keywords || !keywords[0])
		return false;

	strscpy(buf, keywords, sizeof(buf));
	cur = buf;
	while ((token = strsep(&cur, ",")) != NULL) {
		token = strim(token);
		if (!token[0])
			continue;
		if (strnstr(name, token, strlen(name)))
			return true;
	}

	return false;
}

static int zo_detect_memcg_tier(struct task_struct *task)
{
	struct cgroup *cgrp;
	char name[64] = { 0 };

	if (!task || task_css_is_root(task, memory_cgrp_id))
		return ZO_TIER_ROOT;

	cgrp = task_cgroup(task, memory_cgrp_id);
	if (!cgrp || cgroup_name(cgrp, name, sizeof(name)) < 0)
		return ZO_TIER_ROOT;

	if (zo_match_tier_keywords(name, zo_topapp_keywords))
		return ZO_TIER_TOPAPP;
	if (zo_match_tier_keywords(name, zo_foreground_keywords))
		return ZO_TIER_FOREGROUND;
	if (zo_match_tier_keywords(name, zo_background_keywords))
		return ZO_TIER_BACKGROUND;
	if (zo_match_tier_keywords(name, zo_system_keywords))
		return ZO_TIER_SYSTEM;

	return ZO_TIER_ROOT;
}

/*
 * Estimate system memory usage percentage.
 * Delegates to the shared zram_mem_usage_pct() to avoid code duplication.
 * Matches how Android userspace calculates "used" memory:
 *   used = total - free - file_cache - slab_reclaimable
 */
static unsigned long zo_get_mem_usage_pct(void)
{
	return zram_mem_usage_pct();
}

static int zo_compute_pressure(struct zram_opt_stats *stats,
		unsigned long *fullness_pct,
		unsigned long *huge_ratio_pct,
		unsigned long *compress_pct)
{
	unsigned long fullness = 0;
	unsigned long huge_ratio = 0;
	unsigned long compress = 100;
	u16 fullness_avg;
	u16 huge_ratio_avg;
	u16 compress_avg;
	int pressure;

	if (stats && stats->disksize_pages)
		fullness = div_u64(stats->pages_stored * 100,
				  stats->disksize_pages);
	if (stats && stats->pages_stored)
		huge_ratio = div_u64(stats->huge_pages * 100,
				    stats->pages_stored);
	if (stats && stats->compr_data_size)
		compress = div_u64(stats->pages_stored * PAGE_SIZE * 100,
				  stats->compr_data_size);

	if (fullness_pct)
		*fullness_pct = fullness;
	if (huge_ratio_pct)
		*huge_ratio_pct = huge_ratio;
	if (compress_pct)
		*compress_pct = compress;

	bmavg_write_u16(&zo_fullness_avg, min_t(unsigned long, fullness, U16_MAX));
	bmavg_write_u16(&zo_huge_ratio_avg, min_t(unsigned long, huge_ratio, U16_MAX));
	bmavg_write_u16(&zo_compress_pct_avg, min_t(unsigned long, compress, U16_MAX));

	fullness_avg = bmavg_read_u16(&zo_fullness_avg);
	huge_ratio_avg = bmavg_read_u16(&zo_huge_ratio_avg);
	compress_avg = bmavg_read_u16(&zo_compress_pct_avg);

	if (fullness_avg >= ZO_PRESSURE_HIGH_FULLNESS ||
	    huge_ratio_avg >= ZO_PRESSURE_HIGH_HUGE_RATIO) {
		pressure = ZO_PRESSURE_HIGH;
	} else if (fullness_avg >= ZO_PRESSURE_MEDIUM_FULLNESS ||
		   huge_ratio_avg >= ZO_PRESSURE_MEDIUM_HUGE_RATIO ||
		   compress_avg <= ZO_PRESSURE_MEDIUM_COMPRESS_PCT) {
		pressure = ZO_PRESSURE_MEDIUM;
	} else if (zo_last_pressure == ZO_PRESSURE_HIGH) {
		if (fullness_avg >= ZO_PRESSURE_HIGH_FULLNESS_EXIT ||
		    huge_ratio_avg >= ZO_PRESSURE_HIGH_HUGE_RATIO_EXIT)
			pressure = ZO_PRESSURE_HIGH;
		else if (fullness_avg >= ZO_PRESSURE_MEDIUM_FULLNESS ||
			 huge_ratio_avg >= ZO_PRESSURE_MEDIUM_HUGE_RATIO ||
			 compress_avg <= ZO_PRESSURE_MEDIUM_COMPRESS_PCT)
			pressure = ZO_PRESSURE_MEDIUM;
		else
			pressure = ZO_PRESSURE_LOW;
	} else if (fullness_avg >= ZO_PRESSURE_MEDIUM_FULLNESS ||
		   huge_ratio_avg >= ZO_PRESSURE_MEDIUM_HUGE_RATIO ||
		   compress_avg <= ZO_PRESSURE_MEDIUM_COMPRESS_PCT) {
		pressure = ZO_PRESSURE_MEDIUM;
	} else if (zo_last_pressure == ZO_PRESSURE_MEDIUM) {
		if (fullness_avg >= ZO_PRESSURE_MEDIUM_FULLNESS_EXIT ||
		    huge_ratio_avg >= ZO_PRESSURE_MEDIUM_HUGE_RATIO_EXIT ||
		    compress_avg <= ZO_PRESSURE_MEDIUM_COMPRESS_PCT_EXIT)
			pressure = ZO_PRESSURE_MEDIUM;
		else
			pressure = ZO_PRESSURE_LOW;
	} else {
		pressure = ZO_PRESSURE_LOW;
	}

	if (fullness_pct)
		*fullness_pct = fullness_avg;
	if (huge_ratio_pct)
		*huge_ratio_pct = huge_ratio_avg;
	if (compress_pct)
		*compress_pct = compress_avg;

	switch (pressure) {
	case ZO_PRESSURE_HIGH:
		atomic64_inc(&zo_pressure_high_events);
		break;
	case ZO_PRESSURE_MEDIUM:
		atomic64_inc(&zo_pressure_medium_events);
		break;
	default:
		atomic64_inc(&zo_pressure_low_events);
		break;
	}

	return pressure;
}

static bool zo_free_zram_is_usable(struct zram_opt_stats *stats,
		unsigned long fullness,
		unsigned long huge_ratio,
		unsigned long compress)
{
	if (!stats || !stats->has_capacity)
		return false;

	/*
	 * Only consider zram truly unusable when it is essentially full.
	 * Even an unhealthy zram is better than OOM-killing on mobile.
	 */
	if (fullness >= 99)
		return false;
	return true;
}

/*
 * Zram health penalty: gentle nudge rather than a hard cut.
 * Maximum possible penalty is capped at 15 to ensure swap never
 * stops — on mobile, even a degraded zram beats OOM-kill.
 * Penalty is only applied when memory is below the yellow line.
 */
static int zo_compute_zram_penalty(unsigned long fullness,
		unsigned long huge_ratio, unsigned long compress)
{
	int penalty = 0;

	if (fullness >= 95)
		penalty += 6;
	else if (fullness >= 90)
		penalty += 4;
	else if (fullness >= 85)
		penalty += 2;

	if (huge_ratio >= 45)
		penalty += 4;
	else if (huge_ratio >= 35)
		penalty += 2;
	else if (huge_ratio >= 25)
		penalty += 1;

	if (compress <= 110)
		penalty += 4;
	else if (compress <= 120)
		penalty += 2;
	else if (compress <= 135)
		penalty += 1;

	return min(penalty, 15);
}

static int tune_dynamic_swappines(void)
{
	unsigned long nr_file_pages;

	nr_file_pages = global_node_page_state(NR_ACTIVE_FILE) +
		global_node_page_state(NR_INACTIVE_FILE);

	if (threshold1_swappiness_size &&
	    (nr_file_pages >= (threshold1_swappiness_size << 8)))
		return threshold1_vm_swappiness ?: g_swappiness;
	else if (threshold2_swappiness_size &&
		 (nr_file_pages >= (threshold2_swappiness_size << 8)))
		return threshold2_vm_swappiness ?: g_swappiness;

	return g_swappiness;
}

static int tune_dynamic_direct_swappiness(void)
{
	unsigned long nr_file_pages;

	nr_file_pages = global_node_page_state(NR_ACTIVE_FILE) +
		global_node_page_state(NR_INACTIVE_FILE);

	if (g_dynamic_direct_swappiness_threshold &&
	    nr_file_pages >= (g_dynamic_direct_swappiness_threshold << 8))
		return g_dynamic_direct_swappiness;

	return g_direct_swappiness;
}

static void zo_sync_vm_swappiness(int swappiness)
{
	swappiness = clamp(swappiness, 0, 200);
	zo_last_vm_swappiness = swappiness;

	if (READ_ONCE(vm_swappiness) != swappiness)
		WRITE_ONCE(vm_swappiness, swappiness);
}

/*
 * Primary swappiness control: system memory usage drives swap aggressiveness.
 *
 *   >= 80% (RED)   : swappiness = 200, no exceptions
 *   >= 75% (YELLOW): swappiness = 200
 *   >= 65% (ORANGE): swappiness = 180
 *   <  65% (GREEN) : swappiness = 160 - zram_penalty (min 130)
 *
 * Zram health penalty is only applied in the green zone; above the yellow
 * line the system must swap regardless of zram efficiency.
 */
static void zo_set_swappiness(void *data, int *swappiness)
{
	struct zram_opt_stats stats;
	unsigned long mem_pct;
	unsigned long fullness = 0, huge_ratio = 0, compress = 100;
	int pressure;
	int tier;
	int tuned;
	bool has_zram;

	has_zram = zram_get_opt_stats(&stats);
	mem_pct = zo_get_mem_usage_pct();
	zo_last_mem_usage_pct = mem_pct;

	if (has_zram) {
		pressure = zo_compute_pressure(&stats, &fullness,
					       &huge_ratio, &compress);
		zo_last_pressure = pressure;
		zo_last_fullness = fullness;
		zo_last_huge_ratio = huge_ratio;
		zo_last_compress_pct = compress;
	} else {
		pressure = ZO_PRESSURE_LOW;
	}
	tier = zo_detect_memcg_tier(current);
	zo_last_tier = tier;

	/* Zram 99% full — use minimal swappiness, never zero */
	if (has_zram &&
	    !zo_free_zram_is_usable(&stats, fullness, huge_ratio, compress)) {
		*swappiness = current_is_kswapd() ? 80 : 60;
		zo_last_swappiness = *swappiness;
		if (current_is_kswapd())
			zo_sync_vm_swappiness(*swappiness);
		atomic64_inc(&zo_zero_swappiness_events);
		atomic64_inc(&zo_swappiness_rewrites);
		return;
	}

	/* ---- PRIMARY: memory-usage-driven swappiness ---- */
	if (mem_pct >= ZO_MEM_PCT_RED) {
		/* RED LINE: absolute maximum swap */
		tuned = 200;
	} else if (mem_pct >= ZO_MEM_PCT_YELLOW) {
		/* YELLOW LINE: very aggressive swap */
		tuned = 200;
	} else if (mem_pct >= ZO_MEM_PCT_GREEN_CEIL) {
		/* ORANGE zone: aggressive swap */
		tuned = 180;
	} else {
		/* GREEN zone: moderately aggressive baseline */
		tuned = 160;
	}

	/* ---- SECONDARY: zram health penalty (green zone only) ---- */
	if (mem_pct < ZO_MEM_PCT_YELLOW && has_zram) {
		int penalty = zo_compute_zram_penalty(fullness,
						      huge_ratio, compress);
		tuned -= penalty;
	}

	/* ---- Tier-based caps for direct reclaim ---- */
	if (!current_is_kswapd()) {
		switch (tier) {
		case ZO_TIER_TOPAPP:
			tuned = min(tuned, 160);
			break;
		case ZO_TIER_FOREGROUND:
			tuned = min(tuned, 170);
			break;
		case ZO_TIER_BACKGROUND:
			tuned = min(tuned + 10, 200);
			break;
		case ZO_TIER_SYSTEM:
			tuned = min(tuned + 5, 200);
			break;
		default:
			break;
		}
	}

	/* ---- Minimum swappiness floors ---- */
	if (mem_pct >= ZO_MEM_PCT_YELLOW) {
		/* At yellow/red line, never go below 160 */
		tuned = max(tuned, 160);
	} else if (mem_pct >= ZO_MEM_PCT_GREEN_CEIL) {
		/* Orange zone: floor at 140 */
		tuned = max(tuned, 140);
	} else {
		/* Green zone: floor at 130 for kswapd, 100 for direct */
		if (current_is_kswapd())
			tuned = max(tuned, 130);
		else
			tuned = max(tuned, 100);
	}

	*swappiness = clamp(tuned, 0, 200);
	zo_last_swappiness = *swappiness;
	if (current_is_kswapd())
		zo_sync_vm_swappiness(*swappiness);
	atomic64_inc(&zo_swappiness_rewrites);
}

static void zo_set_inactive_ratio(void *data,
		unsigned long *inactive_ratio, int file)
{
	if (file)
		*inactive_ratio = min(2UL, *inactive_ratio);
	else
		*inactive_ratio = 1;
}

/*
 * Balance anon/file reclaim: always enable anon reclaim when memory
 * is above the green ceiling, so the kernel swaps out anon pages
 * instead of only dropping file cache.
 */
static void balance_reclaim(void *unused, bool *balance_anon_file_reclaim)
{
	unsigned long mem_pct = zo_get_mem_usage_pct();

	/* Above green ceiling: ALWAYS balance to encourage swap */
	if (mem_pct >= ZO_MEM_PCT_GREEN_CEIL) {
		*balance_anon_file_reclaim = true;
		zo_last_balance_anon_file_reclaim = true;
		atomic64_inc(&zo_balance_true_events);
		return;
	}

	/* Below green ceiling: use watermark-based heuristic */
	{
		pg_data_t *pgdat = NODE_DATA(0);
		struct zone *zone = &pgdat->node_zones[ZONE_NORMAL];
		unsigned long free_pages;

		free_pages = zone_page_state(zone, NR_FREE_PAGES);
		*balance_anon_file_reclaim =
			free_pages >= low_wmark_pages(zone);
	}

	zo_last_balance_anon_file_reclaim = *balance_anon_file_reclaim;
	if (*balance_anon_file_reclaim)
		atomic64_inc(&zo_balance_true_events);
	else
		atomic64_inc(&zo_balance_false_events);
}

static void vh_android_vh_throttle_direct_reclaim_bypass(void *data,
		bool *bypass)
{
	if (task_is_realtime(current) || current->prio == MAX_RT_PRIO) {
		*bypass = true;
		atomic64_inc(&zo_direct_bypass_events);
	}
}

/*
 * Prevent the kernel from skipping swap during reclaim when memory
 * is above the green ceiling.  This hook is called from
 * do_try_to_free_pages() before the reclaim loop.
 */
static void zo_tune_scan_control(void *data, bool *skip_swap)
{
	if (zo_get_mem_usage_pct() >= ZO_MEM_PCT_GREEN_CEIL) {
		*skip_swap = false;
		atomic64_inc(&zo_scan_skip_events);
	}
}

/*
 * Force anon scanning when memory is elevated.  Setting file_is_tiny
 * tells the reclaim scanner that file pages are dangerously low, which
 * causes it to prefer scanning anon pages (i.e. swapping).
 * Also bump nr_to_reclaim at the red/yellow line for deeper reclaim.
 */
static void zo_modify_scan_control(void *data, u64 *ext,
		unsigned long *nr_to_reclaim,
		struct mem_cgroup *target_mem_cgroup,
		bool *file_is_tiny, bool *may_writepage)
{
	unsigned long mem_pct = zo_get_mem_usage_pct();

	if (mem_pct >= ZO_MEM_PCT_GREEN_CEIL) {
		*file_is_tiny = true;
		atomic64_inc(&zo_file_is_tiny_events);
	}

	if (mem_pct >= ZO_MEM_PCT_YELLOW) {
		unsigned long target = SWAP_CLUSTER_MAX * 4UL;

		if (*nr_to_reclaim < target)
			*nr_to_reclaim = target;
	}
}

/*
 * Always use global vm_swappiness (which we control) instead of
 * per-memcg swappiness, so our tuning takes effect everywhere.
 */
static void zo_use_vm_swappiness(void *data, bool *use_vm_swappiness)
{
	*use_vm_swappiness = true;
}

/*
 * Dynamically adjust zone watermarks.  Scale oplus_extra_free_kbytes
 * based on system memory usage:
 *   RED    : 4x
 *   YELLOW : 3x
 *   ORANGE : 2x
 *   GREEN  : 1x (base)
 *   <GREEN : 0.5x
 */
static void adjust_zone_wmark(void *unused, struct zone *zone, u64 interval)
{
	unsigned long delta;
	unsigned long lowmem_pages = 0;
	unsigned long scaled_extra;
	unsigned long mem_pct;
	struct zone *z;

	if (!oplus_extra_free_kbytes)
		return;

	mem_pct = zo_get_mem_usage_pct();
	scaled_extra = oplus_extra_free_kbytes;

	if (mem_pct >= ZO_MEM_PCT_RED)
		scaled_extra = scaled_extra * 4;
	else if (mem_pct >= ZO_MEM_PCT_YELLOW)
		scaled_extra = scaled_extra * 3;
	else if (mem_pct >= ZO_MEM_PCT_GREEN_CEIL)
		scaled_extra = scaled_extra * 2;
	else
		scaled_extra = max_t(unsigned long, scaled_extra >> 1, 1);

	for_each_zone(z) {
		if (!is_highmem(z))
			lowmem_pages += zone_managed_pages(z);
	}

	if (is_highmem(zone) || !lowmem_pages)
		return;

	delta = scaled_extra >> (PAGE_SHIFT - 10);
	delta *= zone_managed_pages(zone);
	do_div(delta, lowmem_pages);
	zone->_watermark[WMARK_LOW] += delta;
	zone->_watermark[WMARK_HIGH] += delta;
}

static int register_zram_opt_vendor_hooks(void)
{
	int ret;

	ret = register_trace_android_vh_tune_swappiness(zo_set_swappiness, NULL);
	if (ret)
		goto out;

	ret = register_trace_android_vh_tune_inactive_ratio(
			zo_set_inactive_ratio, NULL);
	if (ret)
		goto unregister_swappiness;

	ret = register_trace_android_rvh_set_balance_anon_file_reclaim(
			balance_reclaim, NULL);
	if (ret)
		goto unregister_inactive_ratio;

	ret = register_trace_android_vh_init_adjust_zone_wmark(
			adjust_zone_wmark, NULL);
	if (ret)
		goto unregister_balance_reclaim;

	ret = register_trace_android_vh_throttle_direct_reclaim_bypass(
			vh_android_vh_throttle_direct_reclaim_bypass, NULL);
	if (ret)
		goto unregister_adjust_zone_wmark;

	ret = register_trace_android_vh_tune_scan_control(
			zo_tune_scan_control, NULL);
	if (ret)
		goto unregister_throttle_bypass;

	ret = register_trace_android_vh_modify_scan_control(
			zo_modify_scan_control, NULL);
	if (ret)
		goto unregister_scan_control;

	ret = register_trace_android_vh_use_vm_swappiness(
			zo_use_vm_swappiness, NULL);
	if (ret)
		goto unregister_modify_scan;

	return 0;

unregister_modify_scan:
	unregister_trace_android_vh_modify_scan_control(
			zo_modify_scan_control, NULL);
unregister_scan_control:
	unregister_trace_android_vh_tune_scan_control(
			zo_tune_scan_control, NULL);
unregister_throttle_bypass:
	unregister_trace_android_vh_throttle_direct_reclaim_bypass(
			vh_android_vh_throttle_direct_reclaim_bypass, NULL);
unregister_adjust_zone_wmark:
	unregister_trace_android_vh_init_adjust_zone_wmark(adjust_zone_wmark,
			NULL);
unregister_balance_reclaim:
unregister_inactive_ratio:
	unregister_trace_android_vh_tune_inactive_ratio(zo_set_inactive_ratio,
			NULL);
unregister_swappiness:
	unregister_trace_android_vh_tune_swappiness(zo_set_swappiness, NULL);
out:
	return ret;
}

static void unregister_zram_opt_vendor_hooks(void)
{
	unregister_trace_android_vh_use_vm_swappiness(
			zo_use_vm_swappiness, NULL);
	unregister_trace_android_vh_modify_scan_control(
			zo_modify_scan_control, NULL);
	unregister_trace_android_vh_tune_scan_control(
			zo_tune_scan_control, NULL);
	unregister_trace_android_vh_throttle_direct_reclaim_bypass(
			vh_android_vh_throttle_direct_reclaim_bypass, NULL);
	unregister_trace_android_vh_init_adjust_zone_wmark(adjust_zone_wmark,
			NULL);
	unregister_trace_android_vh_tune_inactive_ratio(zo_set_inactive_ratio,
			NULL);
	unregister_trace_android_vh_tune_swappiness(zo_set_swappiness, NULL);
}

static int debug_get_val(char *buf, char *token, unsigned long *val)
{
	char *str = strstr(buf, token);
	int ret;

	if (!str)
		return -EINVAL;

	ret = kstrtoul(str + strlen(token), 0, val);
	if (ret)
		return -EINVAL;

	if (*val > 200)
		return -EINVAL;

	return 0;
}

static ssize_t swappiness_para_write(struct file *file,
		const char __user *buff, size_t len, loff_t *ppos)
{
	char kbuf[PARA_BUF_LEN] = { '\0' };
	char *str;
	unsigned long val;

	if (len > PARA_BUF_LEN - 1)
		return -EINVAL;

	if (copy_from_user(&kbuf, buff, len))
		return -EFAULT;
	kbuf[len] = '\0';

	str = strstrip(kbuf);
	if (!str)
		return -EINVAL;

	if (!debug_get_val(str, "vm_swappiness=", &val)) {
		g_swappiness = val;
		return len;
	}

	if (!debug_get_val(str, "direct_swappiness=", &val)) {
		g_direct_swappiness = val;
		return len;
	}

	return -EINVAL;
}

static ssize_t swappiness_para_read(struct file *file,
		char __user *buffer, size_t count, loff_t *off)
{
	char *kbuf;
	int len;
	ssize_t ret;

	kbuf = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	len = scnprintf(kbuf, PAGE_SIZE, "vm_swappiness: %d\n", g_swappiness);
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"direct_swappiness: %d\n",
			tune_dynamic_direct_swappiness());
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"kswapd_swappiness: %d\n", tune_dynamic_swappines());
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"balance_anon_file_reclaim: %s\n",
			zo_last_balance_anon_file_reclaim ? "true" : "false");
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"mem_usage_pct: %lu\n", zo_last_mem_usage_pct);
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"pressure_level: %d\n", zo_last_pressure);
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"memcg_tier: %d\n", zo_last_tier);
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"last_swappiness: %d\n", zo_last_swappiness);
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"vm_swappiness_live: %d\n", READ_ONCE(vm_swappiness));
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"last_vm_swappiness: %d\n", zo_last_vm_swappiness);
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"zram_fullness: %lu\n", zo_last_fullness);
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"huge_ratio: %lu\n", zo_last_huge_ratio);
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"compress_pct: %lu\n", zo_last_compress_pct);
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"swappiness_rewrites: %lld\n",
			atomic64_read(&zo_swappiness_rewrites));
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"zero_swappiness_events: %lld\n",
			atomic64_read(&zo_zero_swappiness_events));
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"direct_bypass_events: %lld\n",
			atomic64_read(&zo_direct_bypass_events));
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"balance_true_events: %lld\n",
			atomic64_read(&zo_balance_true_events));
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"balance_false_events: %lld\n",
			atomic64_read(&zo_balance_false_events));
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"pressure_low_events: %lld\n",
			atomic64_read(&zo_pressure_low_events));
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"pressure_medium_events: %lld\n",
			atomic64_read(&zo_pressure_medium_events));
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"pressure_high_events: %lld\n",
			atomic64_read(&zo_pressure_high_events));
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"scan_skip_events: %lld\n",
			atomic64_read(&zo_scan_skip_events));
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"file_is_tiny_events: %lld\n",
			atomic64_read(&zo_file_is_tiny_events));
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"tier_topapp_keywords: %s\n", zo_topapp_keywords);
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"tier_foreground_keywords: %s\n", zo_foreground_keywords);
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"tier_background_keywords: %s\n", zo_background_keywords);
	len += scnprintf(kbuf + len, PAGE_SIZE - len,
			"tier_system_keywords: %s\n", zo_system_keywords);

	ret = simple_read_from_buffer(buffer, count, off, kbuf, len);
	kfree(kbuf);

	return ret;
}

static const struct proc_ops proc_swappiness_para_ops = {
	.proc_write = swappiness_para_write,
	.proc_read = swappiness_para_read,
	.proc_lseek = default_llseek,
};

static ssize_t dynamic_swappiness_write(struct file *file,
		const char __user *buff, size_t len, loff_t *ppos)
{
	char kbuf[PARA_BUF_LEN] = { '\0' };
	char *str;
	int swappiness1, swappiness2;
	int size1, size2, ret;

	if ((len > PARA_BUF_LEN - 1) || (len == 0))
		return -EINVAL;

	if (copy_from_user(&kbuf, buff, len))
		return -EFAULT;

	str = strstrip(kbuf);
	if (!str)
		return -EINVAL;

	ret = sscanf(str, "%d %d %d %d", &swappiness1, &size1,
			     &swappiness2, &size2);
	if (ret != 4)
		return -EINVAL;

	if (check_swappiness(swappiness1) || check_swappiness(swappiness2))
		return -EINVAL;

	if (check_vm_threshold(size1) || check_vm_threshold(size2))
		return -EINVAL;

	threshold1_vm_swappiness = swappiness1;
	threshold1_swappiness_size = size1;
	threshold2_vm_swappiness = swappiness2;
	threshold2_swappiness_size = size2;

	return len;
}

static ssize_t dynamic_swappiness_read(struct file *file,
		char __user *buffer, size_t count, loff_t *off)
{
	char kbuf[PARA_BUF_LEN] = { '\0' };
	int len;

	len = scnprintf(kbuf, PARA_BUF_LEN, "%d %d %d %d\n",
			threshold1_vm_swappiness,
			threshold1_swappiness_size,
			threshold2_vm_swappiness,
			threshold2_swappiness_size);

	return simple_read_from_buffer(buffer, count, off, kbuf, len);
}

static const struct proc_ops proc_dynamic_swappiness_ops = {
	.proc_write = dynamic_swappiness_write,
	.proc_read = dynamic_swappiness_read,
	.proc_lseek = default_llseek,
};

static int create_dynamic_swappiness_proc(void)
{
	dynamic_swappiness_entry = proc_create("oplus_mem/dynamic_swappiness",
			0666, NULL, &proc_dynamic_swappiness_ops);

	if (dynamic_swappiness_entry)
		return 0;

	return -ENOMEM;
}

static void destroy_dynamic_swappiness_proc(void)
{
	proc_remove(dynamic_swappiness_entry);
	dynamic_swappiness_entry = NULL;
}

static ssize_t dynamic_direct_swappiness_write(struct file *file,
		const char __user *buff, size_t len, loff_t *ppos)
{
	char kbuf[PARA_BUF_LEN] = { '\0' };
	char *str;
	int swappiness, size, ret;

	if ((len > PARA_BUF_LEN - 1) || (len == 0))
		return -EINVAL;

	if (copy_from_user(&kbuf, buff, len))
		return -EFAULT;

	str = strstrip(kbuf);
	if (!str)
		return -EINVAL;

	ret = sscanf(str, "%d %d", &swappiness, &size);
	if (ret != 2)
		return -EINVAL;

	if (check_swappiness(swappiness) || check_vm_threshold(size))
		return -EINVAL;

	g_dynamic_direct_swappiness = swappiness;
	g_dynamic_direct_swappiness_threshold = size;
	return len;
}

static ssize_t dynamic_direct_swappiness_read(struct file *file,
		char __user *buffer, size_t count, loff_t *off)
{
	char kbuf[PARA_BUF_LEN] = { '\0' };
	int len;

	len = scnprintf(kbuf, PARA_BUF_LEN, "%d %d\n",
			g_dynamic_direct_swappiness,
			g_dynamic_direct_swappiness_threshold);

	return simple_read_from_buffer(buffer, count, off, kbuf, len);
}

static const struct proc_ops proc_dynamic_direct_swappiness_ops = {
	.proc_write = dynamic_direct_swappiness_write,
	.proc_read = dynamic_direct_swappiness_read,
	.proc_lseek = default_llseek,
};

static int create_dynamic_direct_swappiness_proc(void)
{
	dynamic_direct_swappiness_entry = proc_create(
			"oplus_mem/dynamic_direct_swappiness",
			0666, NULL, &proc_dynamic_direct_swappiness_ops);

	if (dynamic_direct_swappiness_entry)
		return 0;

	return -ENOMEM;
}

static void destroy_dynamic_direct_swappiness_proc(void)
{
	proc_remove(dynamic_direct_swappiness_entry);
	dynamic_direct_swappiness_entry = NULL;
}

static int create_swappiness_para_proc(void)
{
	struct proc_dir_entry *root_dir_entry = proc_mkdir("oplus_mem", NULL);

	para_entry = proc_create((root_dir_entry ?
				"swappiness_para" : "oplus_mem/swappiness_para"),
			0666, root_dir_entry, &proc_swappiness_para_ops);

	if (para_entry)
		return 0;

	return -ENOMEM;
}

static void destroy_swappiness_para_proc(void)
{
	proc_remove(para_entry);
	para_entry = NULL;
}

int zram_opt_init(void)
{
	int ret;
	struct task_struct *p;
	struct task_struct *p1;

	bmavg_init_u16(&zo_fullness_avg);
	bmavg_init_u16(&zo_huge_ratio_avg);
	bmavg_init_u16(&zo_compress_pct_avg);
	bmavg_set_limit_u16(&zo_fullness_avg, 6);
	bmavg_set_limit_u16(&zo_huge_ratio_avg, 6);
	bmavg_set_limit_u16(&zo_compress_pct_avg, 6);

	ret = create_swappiness_para_proc();
	if (ret)
		return ret;

	ret = register_zram_opt_vendor_hooks();
	if (ret) {
		destroy_swappiness_para_proc();
		return ret;
	}

	rcu_read_lock();
	for_each_process(p) {
		if ((p->flags & PF_KTHREAD) &&
		    !strncmp(p->comm, KSWAPD_COMM, sizeof(KSWAPD_COMM) - 1)) {
			g_kswapd_pid = p->pid;
			break;
		}
	}
	rcu_read_unlock();

	rcu_read_lock();
	for_each_process(p1) {
		if ((p1->flags & PF_KTHREAD) &&
		    !strncmp(p1->comm, KCOMPACTD_COMM,
			     sizeof(KCOMPACTD_COMM) - 1)) {
			g_kcompactd_pid = p1->pid;
			break;
		}
	}
	rcu_read_unlock();

	ret = create_dynamic_swappiness_proc();
	if (ret) {
		unregister_zram_opt_vendor_hooks();
		destroy_swappiness_para_proc();
		return ret;
	}

	ret = create_dynamic_direct_swappiness_proc();
	if (ret) {
		destroy_dynamic_swappiness_proc();
		unregister_zram_opt_vendor_hooks();
		destroy_swappiness_para_proc();
		return ret;
	}


	pr_info("zram_opt_init succeed kswapd %d,kcompactd %d!\n",
		g_kswapd_pid, g_kcompactd_pid);
	return 0;
}

void zram_opt_exit(void)
{
	destroy_dynamic_direct_swappiness_proc();
	destroy_dynamic_swappiness_proc();
	unregister_zram_opt_vendor_hooks();
	destroy_swappiness_para_proc();
	pr_info("zram_opt_exit succeed!\n");
}

module_param_named(vm_swappiness, g_swappiness, int, 0644);
module_param_named(direct_vm_swappiness, g_direct_swappiness, int, 0644);
module_param_named(kswapd_pid, g_kswapd_pid, int, 0644);
module_param_named(kcompactd_pid, g_kcompactd_pid, int, 0644);
module_param_named(vm_swappiness_threshold1, threshold1_vm_swappiness, int,
		   0644);
module_param_named(vm_swappiness_threshold2, threshold2_vm_swappiness, int,
		   0644);
module_param_named(swappiness_threshold1_size, threshold1_swappiness_size,
		   int, 0644);
module_param_named(swappiness_threshold2_size, threshold2_swappiness_size,
		   int, 0644);
module_param_named(dynamic_direct_swappiness, g_dynamic_direct_swappiness,
		   int, 0644);
module_param_named(dynamic_direct_swappiness_threshold,
		   g_dynamic_direct_swappiness_threshold, int, 0644);
module_param_named(oplus_extra_free_kbytes, oplus_extra_free_kbytes, int,
		   0644);
module_param_string(tier_topapp_keywords, zo_topapp_keywords,
		    sizeof(zo_topapp_keywords), 0644);
module_param_string(tier_foreground_keywords, zo_foreground_keywords,
		    sizeof(zo_foreground_keywords), 0644);
module_param_string(tier_background_keywords, zo_background_keywords,
		    sizeof(zo_background_keywords), 0644);
module_param_string(tier_system_keywords, zo_system_keywords,
		    sizeof(zo_system_keywords), 0644);
