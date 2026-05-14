// SPDX-License-Identifier: GPL-2.0-only
/*
 * zram optimization helpers integrated as part of zram core module.
 * Ported from external zram_opt implementation with tree-external
 * dependencies removed or replaced by in-tree equivalents.
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

static int zo_last_pressure = ZO_PRESSURE_LOW;
static int zo_last_tier = ZO_TIER_ROOT;
static int zo_last_swappiness;
static int zo_last_vm_swappiness = -1;
static bool zo_last_balance_anon_file_reclaim;
static unsigned long zo_last_fullness;
static unsigned long zo_last_huge_ratio;
static unsigned long zo_last_compress_pct;
static struct bmavg_u16 zo_fullness_avg;
static struct bmavg_u16 zo_huge_ratio_avg;
static struct bmavg_u16 zo_compress_pct_avg;

#define ZO_PRESSURE_HIGH_FULLNESS	92
#define ZO_PRESSURE_HIGH_FULLNESS_EXIT	85
#define ZO_PRESSURE_MEDIUM_FULLNESS	75
#define ZO_PRESSURE_MEDIUM_FULLNESS_EXIT	68
#define ZO_PRESSURE_HIGH_HUGE_RATIO	35
#define ZO_PRESSURE_HIGH_HUGE_RATIO_EXIT	28
#define ZO_PRESSURE_MEDIUM_HUGE_RATIO	20
#define ZO_PRESSURE_MEDIUM_HUGE_RATIO_EXIT	16
#define ZO_PRESSURE_MEDIUM_COMPRESS_PCT	130
#define ZO_PRESSURE_MEDIUM_COMPRESS_PCT_EXIT	140

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
	} else if (current->in_memstall &&
		   (fullness_avg >= ZO_PRESSURE_MEDIUM_FULLNESS ||
		    huge_ratio_avg >= ZO_PRESSURE_MEDIUM_HUGE_RATIO ||
		    compress_avg <= ZO_PRESSURE_MEDIUM_COMPRESS_PCT)) {
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

	if (fullness >= 98)
		return false;
	if (huge_ratio >= 55 && fullness >= 90)
		return false;
	if (compress <= 108 && fullness >= 90)
		return false;
	return true;
}

static int zo_compute_zram_penalty(unsigned long fullness,
		unsigned long huge_ratio, unsigned long compress)
{
	int penalty = 0;

	if (fullness >= 95)
		penalty += 45;
	else if (fullness >= 90)
		penalty += 30;
	else if (fullness >= 85)
		penalty += 15;

	if (huge_ratio >= 45)
		penalty += 25;
	else if (huge_ratio >= 35)
		penalty += 15;
	else if (huge_ratio >= 25)
		penalty += 8;

	if (compress <= 110)
		penalty += 25;
	else if (compress <= 120)
		penalty += 15;
	else if (compress <= 135)
		penalty += 8;

	return penalty;
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

static void zo_set_swappiness(void *data, int *swappiness)
{
	struct zram_opt_stats stats;
	unsigned long fullness = 0, huge_ratio = 0, compress = 100;
	int pressure;
	int tier;
	int tuned;

	if (!zram_get_opt_stats(&stats))
		return;

	pressure = zo_compute_pressure(&stats, &fullness, &huge_ratio, &compress);
	tier = zo_detect_memcg_tier(current);
	zo_last_pressure = pressure;
	zo_last_tier = tier;
	zo_last_fullness = fullness;
	zo_last_huge_ratio = huge_ratio;
	zo_last_compress_pct = compress;

	if (!zo_free_zram_is_usable(&stats, fullness, huge_ratio, compress)) {
		*swappiness = 0;
		zo_last_swappiness = 0;
		if (current_is_kswapd())
			zo_sync_vm_swappiness(0);
		atomic64_inc(&zo_zero_swappiness_events);
		atomic64_inc(&zo_swappiness_rewrites);
		return;
	}

	if (current_is_kswapd()) {
		tuned = tune_dynamic_swappines();
		switch (pressure) {
		case ZO_PRESSURE_LOW:
			tuned = max(tuned - 20, 40);
			break;
		case ZO_PRESSURE_HIGH:
			tuned = min(tuned + 20, 200);
			break;
		default:
			break;
		}
	} else {
		tuned = tune_dynamic_direct_swappiness();
		switch (pressure) {
		case ZO_PRESSURE_LOW:
			tuned = max(tuned - 20, 20);
			break;
		case ZO_PRESSURE_HIGH:
			tuned = min(tuned, 40);
			break;
		default:
			break;
		}

		switch (tier) {
		case ZO_TIER_TOPAPP:
			tuned = pressure == ZO_PRESSURE_HIGH ? min(tuned, 20) :
				min(tuned, 40);
			break;
		case ZO_TIER_FOREGROUND:
			tuned = pressure == ZO_PRESSURE_HIGH ? min(tuned, 30) :
				min(tuned, 55);
			break;
		case ZO_TIER_BACKGROUND:
			tuned = min(tuned + 15, 160);
			break;
		case ZO_TIER_SYSTEM:
			tuned = min(tuned + 5, 140);
			break;
		default:
			break;
		}
	}

	tuned -= zo_compute_zram_penalty(fullness, huge_ratio, compress);
	if (current_is_kswapd())
		tuned = max(tuned, 20);
	else
		tuned = max(tuned, 10);

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

static void balance_reclaim(void *unused, bool *balance_anon_file_reclaim)
{
	struct zram_opt_stats stats;
	pg_data_t *pgdat;
	struct zone *zone;
	unsigned long free_pages_threshold;
	unsigned long normal_zone_free_pages;
	unsigned long fullness = 0, huge_ratio = 0, compress = 100;
	int pressure = ZO_PRESSURE_LOW;
	int tier = zo_detect_memcg_tier(current);

	pgdat = NODE_DATA(0);
	zone = &pgdat->node_zones[ZONE_NORMAL];
	free_pages_threshold = low_wmark_pages(zone) +
		((high_wmark_pages(zone) - low_wmark_pages(zone)) >> 1);
	if (zram_get_opt_stats(&stats)) {
		pressure = zo_compute_pressure(&stats, &fullness, &huge_ratio,
					      &compress);
		if (pressure == ZO_PRESSURE_HIGH)
			free_pages_threshold = high_wmark_pages(zone);
		else if (pressure == ZO_PRESSURE_LOW)
			free_pages_threshold = low_wmark_pages(zone);
		if (tier == ZO_TIER_TOPAPP || tier == ZO_TIER_FOREGROUND)
			free_pages_threshold = min_t(unsigned long,
				free_pages_threshold,
				high_wmark_pages(zone));
	}

	normal_zone_free_pages = zone_page_state(zone, NR_FREE_PAGES);
	*balance_anon_file_reclaim =
		normal_zone_free_pages >= free_pages_threshold;
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

static void adjust_zone_wmark(void *unused, struct zone *zone, u64 interval)
{
	struct zram_opt_stats stats;
	unsigned long delta;
	unsigned long lowmem_pages = 0;
	unsigned long fullness = 0, huge_ratio = 0, compress = 100;
	unsigned long scaled_extra;
	struct zone *z;

	if (!oplus_extra_free_kbytes)
		return;

	scaled_extra = oplus_extra_free_kbytes;
	if (zram_get_opt_stats(&stats)) {
		int pressure = zo_compute_pressure(&stats, &fullness, &huge_ratio,
					      &compress);
		if (pressure == ZO_PRESSURE_HIGH)
			scaled_extra = scaled_extra * 2;
		else if (pressure == ZO_PRESSURE_MEDIUM)
			scaled_extra = scaled_extra + (scaled_extra >> 1);
		else
			scaled_extra = max_t(unsigned long, scaled_extra >> 1, 1);

		if (fullness >= 85)
			scaled_extra += scaled_extra >> 1;
	}

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

	return 0;

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
