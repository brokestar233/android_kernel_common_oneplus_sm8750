// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020-2022 Oplus. All rights reserved.
 */

#define pr_fmt(fmt) "[HYB_ZRAM]" fmt

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/sched/task.h>
#include <linux/sched/debug.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/memcontrol.h>
#include <linux/swap.h>
#include <linux/blkdev.h>

#include "../zram_drv.h"
#include "../zram_drv_internal.h"

#include "internal.h"

#ifdef CONFIG_FG_TASK_UID
#include <linux/healthinfo/fg.h>
#endif

#define HS_LIST_PTR_SHIFT 23
#define HS_LIST_MCG_SHIFT_HALF 8
#define HS_LIST_LOCK_BIT HS_LIST_MCG_SHIFT_HALF
#define HS_LIST_PRIV_BIT (HS_LIST_PTR_SHIFT + HS_LIST_MCG_SHIFT_HALF + \
			  HS_LIST_MCG_SHIFT_HALF + 1)

struct hs_list_head {
	unsigned int mcg_hi : 8;
	unsigned int lock : 1;
	unsigned int prev : 23;
	unsigned int mcg_lo : 8;
	unsigned int priv : 1;
	unsigned int next : 23;
};
struct hs_list_table {
	struct hs_list_head *(*get_node)(int, void *);
	void *private;
};
#define idx_node(idx, tab) ((tab)->get_node((idx), (tab)->private))
#define next_idx(idx, tab) (idx_node((idx), (tab))->next)
#define prev_idx(idx, tab) (idx_node((idx), (tab))->prev)

#define is_last_idx(idx, hidx, tab) (next_idx(idx, tab) == (hidx))
#define is_first_idx(idx, hidx, tab) (prev_idx(idx, tab) == (hidx))

int obj_idx(struct hybridswap *hs_swap, int idx);
int ext_idx(struct hybridswap *hs_swap, int idx);
int mcg_idx(struct hybridswap *hs_swap, int idx);

struct hs_list_table *alloc_table(struct hs_list_head *(*get_node)(int, void *),
				  void *private, gfp_t gfp);
void hs_lock_list(int idx, struct hs_list_table *table);
void hs_unlock_list(int idx, struct hs_list_table *table);

void hs_list_init(int idx, struct hs_list_table *table);
void hs_list_add_nolock(int idx, int hidx, struct hs_list_table *table);
void hs_list_add_tail_nolock(int idx, int hidx, struct hs_list_table *table);
void hs_list_del_nolock(int idx, int hidx, struct hs_list_table *table);
void hs_list_add(int idx, int hidx, struct hs_list_table *table);
void hs_list_add_tail(int idx, int hidx, struct hs_list_table *table);
void hs_list_del(int idx, int hidx, struct hs_list_table *table);

unsigned short hs_list_get_mcgid(int idx, struct hs_list_table *table);
void hs_list_set_mcgid(int idx, struct hs_list_table *table, int mcg_id);
bool hs_list_set_priv(int idx, struct hs_list_table *table);
bool hs_list_clear_priv(int idx, struct hs_list_table *table);
bool hs_list_test_priv(int idx, struct hs_list_table *table);

bool hs_list_empty(int hidx, struct hs_list_table *table);

#define hs_list_for_each_entry(idx, hidx, tab) \
	for ((idx) = next_idx((hidx), (tab)); \
	     (idx) != (hidx); (idx) = next_idx((idx), (tab)))
#define hs_list_for_each_entry_safe(idx, tmp, hidx, tab) \
	for ((idx) = next_idx((hidx), (tab)), (tmp) = next_idx((idx), (tab)); \
	     (idx) != (hidx); (idx) = (tmp), (tmp) = next_idx((idx), (tab)))
#define hs_list_for_each_entry_reverse(idx, hidx, tab) \
	for ((idx) = prev_idx((hidx), (tab)); \
	     (idx) != (hidx); (idx) = prev_idx((idx), (tab)))
#define hs_list_for_each_entry_reverse_safe(idx, tmp, hidx, tab) \
	for ((idx) = prev_idx((hidx), (tab)), (tmp) = prev_idx((idx), (tab)); \
	     (idx) != (hidx); (idx) = (tmp), (tmp) = prev_idx((idx), (tab)))

#ifdef CONFIG_FG_TASK_UID
#endif
#define DUMP_BUF_LEN 512

static unsigned long warning_threshold[SCENE_MAX] = {
	0, 200, 500, 0
};

const char *key_point_name[STAGE_MAX] = {
	"START",
	"INIT",
	"IOENTRY_ALLOC",
	"FIND_EXTENT",
	"IO_EXTENT",
	"SEGMENT_ALLOC",
	"BIO_ALLOC",
	"SUBMIT_BIO",
	"END_IO",
	"SCHED_WORK",
	"END_WORK",
	"CALL_BACK",
	"WAKE_UP",
	"ZRAM_LOCK",
	"DONE"
};

static void perf_dump_stage_latency(struct hybridswap_record_stage *record, ktime_t start)
{
	int i;

	for (i = 0; i < STAGE_MAX; ++i) {
		if (!record->key_point[i].record_cnt)
			continue;

		hybp(HS_LOG_ERR,
		     "%s diff %lld cnt %u end %u lat %lld ravg_sum %llu\n",
		     key_point_name[i],
		     ktime_us_delta(record->key_point[i].first_time, start),
		     record->key_point[i].record_cnt,
		     record->key_point[i].end_cnt,
		     record->key_point[i].proc_total_time,
		     record->key_point[i].proc_ravg_sum);
	}
}

static void perf_dump_record_stage(struct hybridswap_record_stage *record, char *log,
				   unsigned int *count)
{
	int i;
	unsigned int point = 0;

	for (i = 0; i < STAGE_MAX; ++i)
		if (record->key_point[i].record_cnt)
			point = i;

	point++;
	if (point < STAGE_MAX)
		*count += snprintf(log + *count,
				   (size_t)(DUMP_BUF_LEN - *count),
				   " no_record_point %s", key_point_name[point]);
	else
		*count += snprintf(log + *count,
				   (size_t)(DUMP_BUF_LEN - *count), " all_point_record");
}

static long long perf_calc_speed(s64 page_cnt, s64 time)
{
	s64 size;

	if (!page_cnt)
		return 0;

	size = page_cnt * PAGE_SIZE * BITS_PER_BYTE;
	if (time)
		return size * USEC_PER_SEC / time;
	else
		return S64_MAX;
}

static void perf_dump_latency(struct hybridswap_record_stage *record,
			      ktime_t curr_time,
			      bool perf_end_flag)
{
	char log[DUMP_BUF_LEN] = { 0 };
	unsigned int count = 0;
	ktime_t start;
	s64 total_time;

	start = record->key_point[STAGE_START].first_time;
	total_time = ktime_us_delta(curr_time, start);
	count += snprintf(log + count,
			  (size_t)(DUMP_BUF_LEN - count),
			  "totaltime(us) %lld scene %u task %s nice %d",
			  total_time, record->scene, record->task_comm, record->nice);

	if (perf_end_flag)
		count += snprintf(log + count, (size_t)(DUMP_BUF_LEN - count),
				  " page %d segment %d speed(bps) %lld threshold %lu",
				  record->page_cnt, record->segment_cnt,
				  perf_calc_speed(record->page_cnt, total_time),
				  record->warning_threshold);
	else
		count += snprintf(log + count, (size_t)(DUMP_BUF_LEN - count),
				  " state %c", task_state_to_char(record->task));

	perf_dump_record_stage(record, log, &count);

	log_err("perf end flag %u %s\n", perf_end_flag, log);
	perf_dump_stage_latency(record, start);
	dump_stack();
}

static unsigned long perf_warning_threshold(enum hybridswap_scene scene)
{
	if (unlikely(scene >= SCENE_MAX))
		return 0;

	return warning_threshold[scene];
}

void perf_warning(struct timer_list *t)
{
	struct hybridswap_record_stage *record =
		from_timer(record, t, lat_monitor);
	static unsigned long last_dump_lat_jiffies = 0;

	if (!record->warning_threshold)
		return;

	if (jiffies_to_msecs(jiffies - last_dump_lat_jiffies) <= 60000)
		return;

	perf_dump_latency(record, ktime_get(), false);

	if (likely(record->task))
		sched_show_task(record->task);
	last_dump_lat_jiffies = jiffies;
	record->warning_threshold <<= 2;
	record->timeout_flag = true;
	mod_timer(&record->lat_monitor,
		  jiffies + msecs_to_jiffies(record->warning_threshold));
}

static void perf_init_monitor(struct hybridswap_record_stage *record,
			      enum hybridswap_scene scene)
{
	record->warning_threshold = perf_warning_threshold(scene);

	if (!record->warning_threshold)
		return;

	record->task = current;
	get_task_struct(record->task);
	timer_setup(&record->lat_monitor, perf_warning, 0);
	mod_timer(&record->lat_monitor,
		  jiffies + msecs_to_jiffies(record->warning_threshold));
}

static void hybridswap_perf_stop_monitor(struct hybridswap_record_stage *record)
{
	if (!record->warning_threshold)
		return;

	del_timer_sync(&record->lat_monitor);
	put_task_struct(record->task);
}

static void perf_init(struct hybridswap_record_stage *record,
		      enum hybridswap_scene scene)
{
	int i;

	for (i = 0; i < STAGE_MAX; ++i)
		spin_lock_init(&record->key_point[i].time_lock);

	record->nice = task_nice(current);
	record->scene = scene;
	get_task_comm(record->task_comm, current);
	perf_init_monitor(record, scene);
}

void perf_routine_begin(struct hybridswap_record_stage *record,
			enum hybridswap_stage type, ktime_t curr_time,
			unsigned long long current_ravg_sum)
{
	struct hybridswap_stage_info *key_point =
		&record->key_point[type];

	if (!key_point->record_cnt)
		key_point->first_time = curr_time;

	key_point->record_cnt++;
	key_point->last_time = curr_time;
	key_point->last_ravg_sum = current_ravg_sum;
}

void perf_routine_end(struct hybridswap_record_stage *record,
		      enum hybridswap_stage type, ktime_t curr_time,
		      unsigned long long current_ravg_sum)
{
	struct hybridswap_stage_info *key_point =
		&record->key_point[type];
	s64 diff_time = ktime_us_delta(curr_time, key_point->last_time);

	key_point->proc_total_time += diff_time;
	if (diff_time > key_point->proc_max_time)
		key_point->proc_max_time = diff_time;

	key_point->proc_ravg_sum += current_ravg_sum -
		key_point->last_ravg_sum;
	key_point->end_cnt++;
	key_point->last_time = curr_time;
	key_point->last_ravg_sum = current_ravg_sum;
}

void perf_async_set(struct hybridswap_record_stage *record,
		    enum hybridswap_stage type, ktime_t start,
		    unsigned long long start_ravg_sum)
{
	unsigned long long current_ravg_sum = ((type == STAGE_CALL_BACK) ||
					       (type == STAGE_END_WORK)) ? hybridswap_get_ravg_sum() : 0;
	unsigned long flags;

	spin_lock_irqsave(&record->key_point[type].time_lock, flags);
	perf_routine_begin(record, type, start, start_ravg_sum);
	perf_routine_end(record, type, ktime_get(),
			 current_ravg_sum);
	spin_unlock_irqrestore(&record->key_point[type].time_lock, flags);
}

void hybridswap_perf_lat_point(struct hybridswap_record_stage *record,
			       enum hybridswap_stage type)
{
	perf_routine_begin(record, type, ktime_get(),
			   hybridswap_get_ravg_sum());
	record->key_point[type].end_cnt++;
}

void perf_begin(struct hybridswap_record_stage *record,
		ktime_t stsrt, unsigned long long start_ravg_sum,
		enum hybridswap_scene scene)
{
	perf_init(record, scene);
	perf_routine_begin(record, STAGE_START, stsrt,
			   start_ravg_sum);
	record->key_point[STAGE_START].end_cnt++;
}

void hybridswap_perf_lat_stat(struct hybridswap_record_stage *record)
{
	int task_is_fg = 0;
	struct hybridswap_stat *stat = hybridswap_get_stat_obj();
	s64 curr_lat;
	s64 timeout_value[SCENE_MAX] = {
		2000000, 100000, 500000, 2000000
	};

	if (!stat || (record->scene >= SCENE_MAX))
		return;

	curr_lat = ktime_us_delta(record->key_point[STAGE_DONE].first_time,
				  record->key_point[STAGE_START].first_time);
	atomic64_add(curr_lat, &stat->lat[record->scene].latency_tot);
	if (curr_lat > atomic64_read(&stat->lat[record->scene].latency_max))
		atomic64_set(&stat->lat[record->scene].latency_max, curr_lat);
	if (curr_lat > timeout_value[record->scene])
		atomic64_inc(&stat->lat[record->scene].timeout_cnt);
	if (record->scene == SCENE_FAULT_OUT) {
		if (curr_lat <= timeout_value[SCENE_FAULT_OUT])
			return;
#ifdef CONFIG_FG_TASK_UID
		task_is_fg = current_is_fg() ? 1 : 0;
#endif
		if (curr_lat > 500000)
			atomic64_inc(&stat->fault_stat[task_is_fg].timeout_500ms_cnt);
		else if (curr_lat > 100000)
			atomic64_inc(&stat->fault_stat[task_is_fg].timeout_100ms_cnt);
		log_info("task %s:%d fault out timeout us %llu fg %d\n",
			 current->comm, current->pid, curr_lat, task_is_fg);
	}
}

void perf_end(struct hybridswap_record_stage *record)
{
	int loglevel;

	hybridswap_perf_stop_monitor(record);
	hybridswap_perf_lat_point(record, STAGE_DONE);
	hybridswap_perf_lat_stat(record);

	loglevel = record->timeout_flag ? HS_LOG_ERR : HS_LOG_DEBUG;
	if (loglevel > hybridswap_loglevel())
		return;

	perf_dump_latency(record,
			  record->key_point[STAGE_DONE].first_time, true);
}

void perf_latency_begin(struct hybridswap_record_stage *record,
			enum hybridswap_stage type)
{
	perf_routine_begin(record, type, ktime_get(),
			   hybridswap_get_ravg_sum());
}

void perf_latency_end(struct hybridswap_record_stage *record,
		      enum hybridswap_stage type)
{
	perf_routine_end(record, type, ktime_get(),
			 hybridswap_get_ravg_sum());
}

void perf_stat_io(struct hybridswap_record_stage *record, int page_cnt,
		  int segment_cnt)
{
	record->page_cnt = page_cnt;
	record->segment_cnt = segment_cnt;
}


#define SCENARIO_NAME_LEN 32
#define MBYTE_SHIFT 20

static char scene_name[SCENE_MAX][SCENARIO_NAME_LEN] = {
	"reclaim_in",
	"fault_out",
	"batch_out",
	"pre_out"
};

static char *fg_bg[2] = {"BG", "FG"};

static void latency_show(struct seq_file *m,
			 struct hybridswap_stat *stat)
{
	int i;

	for (i = 0; i < SCENE_MAX; ++i) {
		seq_printf(m, "%s_latency_tot: %lld\n",
			   scene_name[i],
			   atomic64_read(&stat->lat[i].latency_tot));
		seq_printf(m, "%s_latency_max: %lld\n",
			   scene_name[i],
			   atomic64_read(&stat->lat[i].latency_max));
		seq_printf(m, "%s_timeout_cnt: %lld\n",
			   scene_name[i],
			   atomic64_read(&stat->lat[i].timeout_cnt));
	}

	for (i = 0; i < 2; i++) {
		seq_printf(m, "fault_out_timeout_100ms_cnt(%s): %lld\n",
			   fg_bg[i],
			   atomic64_read(&stat->fault_stat[i].timeout_100ms_cnt));
		seq_printf(m, "fault_out_timeout_500ms_cnt(%s): %lld\n",
			   fg_bg[i],
			   atomic64_read(&stat->fault_stat[i].timeout_500ms_cnt));
	}
}

static void stats_show(struct seq_file *m,
		       struct hybridswap_stat *stat)
{
	seq_printf(m, "out_times: %lld\n",
		   atomic64_read(&stat->reclaimin_cnt));
	seq_printf(m, "out_comp_size: %lld MB\n",
		   atomic64_read(&stat->reclaimin_bytes) >> MBYTE_SHIFT);
	if (PAGE_SHIFT < MBYTE_SHIFT)
		seq_printf(m, "out_ori_size: %lld MB\n",
			   atomic64_read(&stat->reclaimin_pages) >>
			   (MBYTE_SHIFT - PAGE_SHIFT));
	seq_printf(m, "in_times: %lld\n",
		   atomic64_read(&stat->batchout_cnt));
	seq_printf(m, "in_comp_size: %lld MB\n",
		   atomic64_read(&stat->batchout_bytes) >> MBYTE_SHIFT);
	if (PAGE_SHIFT < MBYTE_SHIFT)
		seq_printf(m, "in_ori_size: %lld MB\n",
			   atomic64_read(&stat->batchout_pages) >>
			   (MBYTE_SHIFT - PAGE_SHIFT));
	seq_printf(m, "all_fault: %lld\n",
		   atomic64_read(&stat->fault_cnt));
	seq_printf(m, "fault: %lld\n",
		   atomic64_read(&stat->hybridswap_fault_cnt));
}

static void hybridswap_info_show(struct seq_file *m,
				 struct hybridswap_stat *stat)
{
	seq_printf(m, "reout_ori_size: %lld MB\n",
		   atomic64_read(&stat->reout_pages) >>
		   (MBYTE_SHIFT - PAGE_SHIFT));
	seq_printf(m, "reout_comp_size: %lld MB\n",
		   atomic64_read(&stat->reout_bytes) >> MBYTE_SHIFT);
	seq_printf(m, "store_comp_size: %lld MB\n",
		   atomic64_read(&stat->stored_size) >> MBYTE_SHIFT);
	seq_printf(m, "store_ori_size: %lld MB\n",
		   atomic64_read(&stat->stored_pages) >>
		   (MBYTE_SHIFT - PAGE_SHIFT));
	seq_printf(m, "notify_free_size: %lld MB\n",
		   atomic64_read(&stat->notify_free) >>
		   (MBYTE_SHIFT - EXTENT_SHIFT));
	seq_printf(m, "store_memcg_cnt: %lld\n",
		   atomic64_read(&stat->mcg_cnt));
	seq_printf(m, "store_extent_cnt: %lld\n",
		   atomic64_read(&stat->ext_cnt));
	seq_printf(m, "store_fragment_cnt: %lld\n",
		   atomic64_read(&stat->frag_cnt));
}

static void error_show(struct seq_file *m,
		       struct hybridswap_stat *stat)
{
	int i;

	for (i = 0; i < SCENE_MAX; ++i) {
		seq_printf(m, "%s_io_fail_cnt: %lld\n",
			   scene_name[i],
			   atomic64_read(&stat->io_fail_cnt[i]));
		seq_printf(m, "%s_alloc_fail_cnt: %lld\n",
			   scene_name[i],
			   atomic64_read(&stat->alloc_fail_cnt[i]));
	}
}

int hybridswap_psi_show(struct seq_file *m, void *v)
{
	struct hybridswap_stat *stat = NULL;

	if (!hybridswap_core_enabled())
		return -EINVAL;

	stat = hybridswap_get_stat_obj();
	if (unlikely(!stat)) {
		log_err("can't get stat obj!\n");
		return -EINVAL;
	}

	stats_show(m, stat);
	hybridswap_info_show(m, stat);
	latency_show(m, stat);
	error_show(m, stat);

	return 0;
}

unsigned long long hybridswap_read_zram_pagefault(void)
{
	struct hybridswap_stat *stat = NULL;

	if (!hybridswap_core_enabled())
		return 0;

	stat = hybridswap_get_stat_obj();
	if (unlikely(!stat)) {
		log_err("can't get stat obj!\n");

		return 0;
	}

	return atomic64_read(&stat->fault_cnt);
}

bool is_hybridswap_reclaim_work_running(void)
{
	struct hybridswap_stat *stat = NULL;

	if (!hybridswap_core_enabled())
		return false;

	stat = hybridswap_get_stat_obj();
	if (unlikely(!stat)) {
		log_err("can't get stat obj!\n");

		return 0;
	}

	return atomic64_read(&stat->reclaimin_infight) ? true : false;
}

unsigned long long hybridswap_read_memcg_stats(struct mem_cgroup *mcg,
					       enum hybridswap_memcg_member mcg_member)
{
	struct mem_cgroup_hybridswap *mcg_hybs;

	unsigned long long val = 0;
	int extcnt;

	if (!hybridswap_core_enabled())
		return 0;

	mcg_hybs = MEMCGRP_ITEM_DATA(mcg);
	if (!mcg_hybs) {
		log_dbg("NULL mcg_hybs\n");
		return 0;
	}

	switch (mcg_member) {
	case MCG_ZRAM_STORED_SZ:
		val = atomic64_read(&mcg_hybs->zram_stored_size);
		break;
	case MCG_ZRAM_STORED_PG_SZ:
		val = atomic64_read(&mcg_hybs->zram_page_size);
		break;
	case MCG_DISK_STORED_SZ:
		val = atomic64_read(&mcg_hybs->hybridswap_stored_size);
		break;
	case MCG_DISK_STORED_PG_SZ:
		val = atomic64_read(&mcg_hybs->hybridswap_stored_pages);
		break;
	case MCG_ANON_FAULT_CNT:
		val = atomic64_read(&mcg_hybs->hybridswap_allfaultcnt);
		break;
	case MCG_DISK_FAULT_CNT:
		val = atomic64_read(&mcg_hybs->hybridswap_faultcnt);
		break;
	case MCG_ESWAPOUT_CNT:
		val = atomic64_read(&mcg_hybs->hybridswap_outcnt);
		break;
	case MCG_ESWAPOUT_SZ:
		val = atomic64_read(&mcg_hybs->hybridswap_outextcnt) << EXTENT_SHIFT;
		break;
	case MCG_ESWAPIN_CNT:
		val = atomic64_read(&mcg_hybs->hybridswap_incnt);
		break;
	case MCG_ESWAPIN_SZ:
		val = atomic64_read(&mcg_hybs->hybridswap_inextcnt) << EXTENT_SHIFT;
		break;
	case MCG_DISK_SPACE:
		extcnt = atomic_read(&mcg_hybs->hybridswap_extcnt);
		if (extcnt < 0)
			extcnt = 0;
		val = ((unsigned long long) extcnt) << EXTENT_SHIFT;
		break;
	case MCG_DISK_SPACE_PEAK:
		extcnt = atomic_read(&mcg_hybs->hybridswap_peakextcnt);
		if (extcnt < 0)
			extcnt = 0;
		val = ((unsigned long long) extcnt) << EXTENT_SHIFT;
		break;
	default:
		break;
	}

	return val;
}

void hybridswap_error_record(enum hybridswap_stage_err point,
			     u32 index, int ext_id, unsigned char *task_comm)
{
	struct hybridswap_stat *stat = NULL;
	unsigned long flags;
	unsigned int copylen = strlen(task_comm) + 1;

	stat = hybridswap_get_stat_obj();
	if (unlikely(!stat)) {
		log_err("can't get stat obj!\n");
		return;
	}

	if (copylen > TASK_COMM_LEN) {
		log_err("task_comm len %d is err\n", copylen);
		return;
	}

	spin_lock_irqsave(&stat->record.lock, flags);
	if (stat->record.num < MAX_FAIL_RECORD_NUM) {
		stat->record.record[stat->record.num].point = point;
		stat->record.record[stat->record.num].index = index;
		stat->record.record[stat->record.num].ext_id = ext_id;
		stat->record.record[stat->record.num].time = ktime_get();
		memcpy(stat->record.record[stat->record.num].task_comm,
		       task_comm, copylen);
		stat->record.num++;
	}
	spin_unlock_irqrestore(&stat->record.lock, flags);
}

static void hybridswap_read_fail_record(struct hybridswap_record_err_info *record_info)
{
	struct hybridswap_stat *stat = NULL;
	unsigned long flags;

	if (!hybridswap_core_enabled())
		return;

	stat = hybridswap_get_stat_obj();
	if (unlikely(!stat)) {
		log_err("can't get stat obj!\n");
		return;
	}

	spin_lock_irqsave(&stat->record.lock, flags);
	memcpy(record_info, &stat->record,
	       sizeof(struct hybridswap_record_err_info));
	stat->record.num = 0;
	spin_unlock_irqrestore(&stat->record.lock, flags);
}

static ssize_t hybridswap_error_record_show(char *buf)
{
	int i;
	ssize_t size = 0;
	struct hybridswap_record_err_info record_info = { 0 };

	hybridswap_read_fail_record(&record_info);

	size += scnprintf(buf + size, PAGE_SIZE,
			  "fail_record_num: %d\n", record_info.num);
	for (i = 0; i < record_info.num; ++i)
		size += scnprintf(buf + size, PAGE_SIZE - size,
				  "point[%u]time[%lld]taskname[%s]index[%u]ext_id[%d]\n",
				  record_info.record[i].point,
				  ktime_us_delta(ktime_get(),
						 record_info.record[i].time),
				  record_info.record[i].task_comm,
				  record_info.record[i].index,
				  record_info.record[i].ext_id);

	return size;
}

ssize_t hybridswap_report_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return hybridswap_error_record_show(buf);
}

static inline ssize_t meminfo_show(struct hybridswap_stat *stat, char *buf, ssize_t len)
{
	unsigned long eswap_total_pages = 0, eswap_compressed_pages = 0;
	unsigned long eswap_used_pages = 0;
	unsigned long zram_total_pags, zram_used_pages, zram_compressed;
	ssize_t size = 0;

	if (!stat || !buf || !len)
		return 0;

	(void)hybridswap_stored_info(&eswap_total_pages, &eswap_compressed_pages);
	eswap_used_pages = atomic64_read(&stat->stored_pages);
#ifdef CONFIG_HYBRIDSWAP_SWAPD
	zram_total_pags = hybridswapd_ops->zram_total_pages();
#else
	zram_total_pags = 0;
#endif
	zram_compressed = atomic64_read(&stat->zram_stored_size);
	zram_used_pages = atomic64_read(&stat->zram_stored_pages);

	size += scnprintf(buf + size, len - size, "%-32s %12lu KB\n",
			  "EST:", eswap_total_pages << (PAGE_SHIFT - 10));
	size += scnprintf(buf + size, len - size, "%-32s %12lu KB\n",
			  "ESU_C:", eswap_compressed_pages << (PAGE_SHIFT - 10));
	size += scnprintf(buf + size, len - size, "%-32s %12lu KB\n",
			  "ESU_O:", eswap_used_pages << (PAGE_SHIFT - 10));
	size += scnprintf(buf + size, len - size, "%-32s %12lu KB\n",
			  "ZST:", zram_total_pags << (PAGE_SHIFT - 10));
	size += scnprintf(buf + size, len - size, "%-32s %12lu KB\n",
			  "ZSU_C:", zram_compressed >> 10);
	size += scnprintf(buf + size, len - size, "%-32s %12lu KB\n",
			  "ZSU_O:", zram_used_pages << (PAGE_SHIFT - 10));

	return size;
}

static inline ssize_t chp_meminfo_show(char *buf, ssize_t len)
{
	ssize_t size = 0;
	unsigned long total_pages = hybridswapd_ops->zram_total_pages() << 2;

	size += scnprintf(buf + size, len - size, "%-32s %d KB\n", "EST:", 0);
	size += scnprintf(buf + size, len - size, "%-32s %d KB\n", "ESU_C:", 0);
	size += scnprintf(buf + size, len - size, "%-32s %d KB\n", "ESU_O:", 0);
	size += scnprintf(buf + size, len - size, "%-32s %12lu KB\n",
			  "ZST:", total_pages);
	size += scnprintf(buf + size, len - size, "%-32s %12lu KB\n",
			  "ZSU_C:",
			  hybridswapd_ops->zram_compressed_pages() << 2);
	/*
	 * zram_total_pages may equal to limit zram size not swap total, so
	 * zram used pages may greater than zram_total_pages
	 */
	size += scnprintf(buf + size, len - size, "%-32s %12lu KB\n",
			  "ZSU_O:",
			  min(hybridswapd_ops->zram_used_pages() << 2,
			      total_pages));
	return size;
}

ssize_t hybridswap_stat_snap_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	ssize_t size = 0;
	struct hybridswap_stat *stat = NULL;

	if (!hybridswap_core_enabled())
		return 0;

	stat = hybridswap_get_stat_obj();
	if (unlikely(!stat)) {
		log_info("can't get stat obj!\n");
		return 0;
	}

	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu\n",
			  "reclaimin_cnt:", atomic64_read(&stat->reclaimin_cnt));
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu KB\n",
			  "reclaimin_bytes:", atomic64_read(&stat->reclaimin_bytes) / SZ_1K);
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu KB\n",
			  "reclaimin_real_load:", atomic64_read(&stat->reclaimin_real_load) / SZ_1K);
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu KB\n",
			  "reclaimin_bytes_daily:", atomic64_read(&stat->reclaimin_bytes_daily) / SZ_1K);
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu KB\n",
			  "reclaimin_pages:", atomic64_read(&stat->reclaimin_pages) * PAGE_SIZE / SZ_1K);
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu\n",
			  "reclaimin_infight:", atomic64_read(&stat->reclaimin_infight));
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu\n",
			  "batchout_cnt:", atomic64_read(&stat->batchout_cnt));
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu KB\n",
			  "batchout_bytes:", atomic64_read(&stat->batchout_bytes) / SZ_1K);
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu KB\n",
			  "batchout_real_load:", atomic64_read(&stat->batchout_real_load) / SZ_1K);
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu KB\n",
			  "batchout_pages:", atomic64_read(&stat->batchout_pages) * PAGE_SIZE / SZ_1K);
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu\n",
			  "batchout_inflight:", atomic64_read(&stat->batchout_inflight));
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu\n",
			  "fault_cnt:", atomic64_read(&stat->fault_cnt));
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu\n",
			  "fault_cnt:", atomic64_read(&stat->hybridswap_fault_cnt));
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu KB\n",
			  "reout_pages:", atomic64_read(&stat->reout_pages) * PAGE_SIZE / SZ_1K);
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu KB\n",
			  "reout_bytes:", atomic64_read(&stat->reout_bytes) / SZ_1K);
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu KB\n",
			  "zram_stored_pages:", atomic64_read(&stat->zram_stored_pages) * PAGE_SIZE / SZ_1K);
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu KB\n",
			  "zram_stored_size:", atomic64_read(&stat->zram_stored_size) / SZ_1K);
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu KB\n",
			  "stored_pages:", atomic64_read(&stat->stored_pages) * PAGE_SIZE / SZ_1K);
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu KB\n",
			  "stored_size:", atomic64_read(&stat->stored_size) / SZ_1K);
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu KB\n",
			  "reclain-batchout:", (atomic64_read(&stat->reclaimin_real_load) -
						atomic64_read(&stat->batchout_real_load)) / SZ_1K);
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12lld KB\n",
			  "reclain-batchout-stored:",
			  (atomic64_read(&stat->reclaimin_real_load) -
			   atomic64_read(&stat->batchout_real_load) -
			   atomic64_read(&stat->stored_size)) / SZ_1K);
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12lld KB\n",
			  "dropped_ext_size:", atomic64_read(&stat->dropped_ext_size) / SZ_1K);
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu\n",
			  "notify_free:", atomic64_read(&stat->notify_free));
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu\n",
			  "frag_cnt:", atomic64_read(&stat->frag_cnt));
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu\n",
			  "mcg_cnt:", atomic64_read(&stat->mcg_cnt));
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu\n",
			  "ext_cnt:", atomic64_read(&stat->ext_cnt));
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu\n",
			  "miss_free:", atomic64_read(&stat->miss_free));
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu\n",
			  "mcgid_clear:", atomic64_read(&stat->mcgid_clear));
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu\n",
			  "skip_track_cnt:", atomic64_read(&stat->skip_track_cnt));
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu\n",
			  "null_memcg_skip_track_cnt:",
			  atomic64_read(&stat->null_memcg_skip_track_cnt));
	size += scnprintf(buf + size, PAGE_SIZE - size, "%-32s %12llu\n",
			  "used_swap_pages:", atomic64_read(&stat->used_swap_pages) * PAGE_SIZE / SZ_1K);
	size += meminfo_show(stat, buf + size, PAGE_SIZE - size);

	return size;
}

ssize_t hybridswap_meminfo_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct hybridswap_stat *stat = NULL;

#ifdef CONFIG_CONT_PTE_HUGEPAGE_64K_ZRAM
	if (chp_supported) {
		if (hybridswap_get_stat_obj())
			return chp_meminfo_show(buf, PAGE_SIZE);
		else
			return 0;
	}
#endif
	if (!hybridswap_core_enabled())
		return 0;

	stat = hybridswap_get_stat_obj();
	if (unlikely(!stat)) {
		log_info("can't get stat obj!\n");
		return 0;
	}

	return meminfo_show(stat, buf, PAGE_SIZE);
}

static struct hs_list_head *get_node_default(int idx, void *private)
{
	struct hs_list_head *table = private;

	return &table[idx];
}

struct hs_list_table *alloc_table(struct hs_list_head *(*get_node)(int, void *),
				  void *private, gfp_t gfp)
{
	struct hs_list_table *table =
		kmalloc(sizeof(struct hs_list_table), gfp);

	if (!table)
		return NULL;
	table->get_node = get_node ? get_node : get_node_default;
	table->private = private;

	return table;
}

void hs_lock_list(int idx, struct hs_list_table *table)
{
	struct hs_list_head *node = idx_node(idx, table);

	if (!node) {
		log_err("idx = %d, table = %pK\n", idx, table);
		return;
	}
	bit_spin_lock(HS_LIST_LOCK_BIT, (unsigned long *)node);
}

void hs_unlock_list(int idx, struct hs_list_table *table)
{
	struct hs_list_head *node = idx_node(idx, table);

	if (!node) {
		log_err("idx = %d, table = %pK\n", idx, table);
		return;
	}
	bit_spin_unlock(HS_LIST_LOCK_BIT, (unsigned long *)node);
}

bool hs_list_empty(int hidx, struct hs_list_table *table)
{
	bool ret = false;

	hs_lock_list(hidx, table);
	ret = (prev_idx(hidx, table) == hidx) && (next_idx(hidx, table) == hidx);
	hs_unlock_list(hidx, table);

	return ret;
}

void hs_list_init(int idx, struct hs_list_table *table)
{
	struct hs_list_head *node = idx_node(idx, table);

	if (!node) {
		log_err("idx = %d, table = %pS func %pS\n",
			idx, table, table->get_node);
		return;
	}
	memset(node, 0, sizeof(struct hs_list_head));
	node->prev = idx;
	node->next = idx;
}

void hs_list_add_nolock(int idx, int hidx, struct hs_list_table *table)
{
	struct hs_list_head *node = NULL;
	struct hs_list_head *head = NULL;
	struct hs_list_head *next = NULL;
	int nidx;

	node = idx_node(idx, table);
	if (!node) {
		hybp(HS_LOG_ERR,
		     "NULL node, idx = %d, hidx = %d, table = %pK\n",
		     idx, hidx, table);
		return;
	}
	head = idx_node(hidx, table);
	if (!head) {
		hybp(HS_LOG_ERR,
		     "NULL head, idx = %d, hidx = %d, table = %pK\n",
		     idx, hidx, table);
		return;
	}
	next = idx_node(head->next, table);
	if (!next) {
		hybp(HS_LOG_ERR,
		     "NULL next, idx = %d, hidx = %d, table = %pK\n",
		     idx, hidx, table);
		return;
	}

	nidx = head->next;
	if (idx != hidx)
		hs_lock_list(idx, table);
	node->prev = hidx;
	node->next = nidx;
	if (idx != hidx)
		hs_unlock_list(idx, table);
	head->next = idx;
	if (nidx != hidx)
		hs_lock_list(nidx, table);
	next->prev = idx;
	if (nidx != hidx)
		hs_unlock_list(nidx, table);
}

void hs_list_add_tail_nolock(int idx, int hidx, struct hs_list_table *table)
{
	struct hs_list_head *node = NULL;
	struct hs_list_head *head = NULL;
	struct hs_list_head *tail = NULL;
	int tidx;

	node = idx_node(idx, table);
	if (!node) {
		hybp(HS_LOG_ERR,
		     "NULL node, idx = %d, hidx = %d, table = %pK\n",
		     idx, hidx, table);
		return;
	}
	head = idx_node(hidx, table);
	if (!head) {
		hybp(HS_LOG_ERR,
		     "NULL head, idx = %d, hidx = %d, table = %pK\n",
		     idx, hidx, table);
		return;
	}
	tail = idx_node(head->prev, table);
	if (!tail) {
		hybp(HS_LOG_ERR,
		     "NULL tail, idx = %d, hidx = %d, table = %pK\n",
		     idx, hidx, table);
		return;
	}

	tidx = head->prev;
	if (idx != hidx)
		hs_lock_list(idx, table);
	node->prev = tidx;
	node->next = hidx;
	if (idx != hidx)
		hs_unlock_list(idx, table);
	head->prev = idx;
	if (tidx != hidx)
		hs_lock_list(tidx, table);
	tail->next = idx;
	if (tidx != hidx)
		hs_unlock_list(tidx, table);
}

void hs_list_del_nolock(int idx, int hidx, struct hs_list_table *table)
{
	struct hs_list_head *node = NULL;
	struct hs_list_head *prev = NULL;
	struct hs_list_head *next = NULL;
	int pidx, nidx;

	node = idx_node(idx, table);
	if (!node) {
		hybp(HS_LOG_ERR,
		     "NULL node, idx = %d, hidx = %d, table = %pK\n",
		     idx, hidx, table);
		return;
	}
	prev = idx_node(node->prev, table);
	if (!prev) {
		hybp(HS_LOG_ERR,
		     "NULL prev, idx = %d, hidx = %d, table = %pK\n",
		     idx, hidx, table);
		return;
	}
	next = idx_node(node->next, table);
	if (!next) {
		hybp(HS_LOG_ERR,
		     "NULL next, idx = %d, hidx = %d, table = %pK\n",
		     idx, hidx, table);
		return;
	}

	if (idx != hidx)
		hs_lock_list(idx, table);
	pidx = node->prev;
	nidx = node->next;
	node->prev = idx;
	node->next = idx;
	if (idx != hidx)
		hs_unlock_list(idx, table);
	if (pidx != hidx)
		hs_lock_list(pidx, table);
	prev->next = nidx;
	if (pidx != hidx)
		hs_unlock_list(pidx, table);
	if (nidx != hidx)
		hs_lock_list(nidx, table);
	next->prev = pidx;
	if (nidx != hidx)
		hs_unlock_list(nidx, table);
}

void hs_list_add(int idx, int hidx, struct hs_list_table *table)
{
	hs_lock_list(hidx, table);
	hs_list_add_nolock(idx, hidx, table);
	hs_unlock_list(hidx, table);
}

void hs_list_add_tail(int idx, int hidx, struct hs_list_table *table)
{
	hs_lock_list(hidx, table);
	hs_list_add_tail_nolock(idx, hidx, table);
	hs_unlock_list(hidx, table);
}

void hs_list_del(int idx, int hidx, struct hs_list_table *table)
{
	hs_lock_list(hidx, table);
	hs_list_del_nolock(idx, hidx, table);
	hs_unlock_list(hidx, table);
}

unsigned short hs_list_get_mcgid(int idx, struct hs_list_table *table)
{
	struct hs_list_head *node = idx_node(idx, table);
	int mcg_id;

	if (!node) {
		log_err("idx = %d, table = %pK\n", idx, table);
		return 0;
	}

	hs_lock_list(idx, table);
	mcg_id = (node->mcg_hi << HS_LIST_MCG_SHIFT_HALF) | node->mcg_lo;
	hs_unlock_list(idx, table);

	return mcg_id;
}

void hs_list_set_mcgid(int idx, struct hs_list_table *table, int mcg_id)
{
	struct hs_list_head *node = idx_node(idx, table);

	if (!node) {
		log_err("idx = %d, table = %pK, mcg = %d\n",
			idx, table, mcg_id);
		return;
	}

	hs_lock_list(idx, table);
	node->mcg_hi = (u32)mcg_id >> HS_LIST_MCG_SHIFT_HALF;
	node->mcg_lo = (u32)mcg_id & ((1 << HS_LIST_MCG_SHIFT_HALF) - 1);
	hs_unlock_list(idx, table);
}

bool hs_list_set_priv(int idx, struct hs_list_table *table)
{
	struct hs_list_head *node = idx_node(idx, table);
	bool ret = false;

	if (!node) {
		log_err("idx = %d, table = %pK\n", idx, table);
		return false;
	}
	hs_lock_list(idx, table);
	ret = !test_and_set_bit(HS_LIST_PRIV_BIT, (unsigned long *)node);
	hs_unlock_list(idx, table);

	return ret;
}

bool hs_list_test_priv(int idx, struct hs_list_table *table)
{
	struct hs_list_head *node = idx_node(idx, table);
	bool ret = false;

	if (!node) {
		log_err("idx = %d, table = %pK\n", idx, table);
		return false;
	}
	hs_lock_list(idx, table);
	ret = test_bit(HS_LIST_PRIV_BIT, (unsigned long *)node);
	hs_unlock_list(idx, table);

	return ret;
}

bool hs_list_clear_priv(int idx, struct hs_list_table *table)
{
	struct hs_list_head *node = idx_node(idx, table);
	bool ret = false;

	if (!node) {
		log_err("idx = %d, table = %pK\n", idx, table);
		return false;
	}

	hs_lock_list(idx, table);
	ret = test_and_clear_bit(HS_LIST_PRIV_BIT, (unsigned long *)node);
	hs_unlock_list(idx, table);

	return ret;
}

struct mem_cgroup *get_mem_cgroup(unsigned short mcg_id)
{
	struct mem_cgroup *mcg = NULL;

	rcu_read_lock();
	mcg = mem_cgroup_from_id(mcg_id);
	rcu_read_unlock();

	return mcg;
}

static bool fragment_dec(bool prev_flag, bool next_flag,
			 struct hybridswap_stat *stat)
{
	if (prev_flag && next_flag) {
		atomic64_inc(&stat->frag_cnt);
		return false;
	}

	if (prev_flag || next_flag)
		return false;

	return true;
}

static bool fragment_inc(bool prev_flag, bool next_flag,
			 struct hybridswap_stat *stat)
{
	if (prev_flag && next_flag) {
		atomic64_dec(&stat->frag_cnt);
		return false;
	}

	if (prev_flag || next_flag)
		return false;

	return true;
}

static bool prev_is_cont(struct hybridswap *hs_swap, int ext_id, int mcg_id)
{
	int prev;

	if (is_first_idx(ext_idx(hs_swap, ext_id), mcg_idx(hs_swap, mcg_id),
			 hs_swap->ext_table))
		return false;
	prev = prev_idx(ext_idx(hs_swap, ext_id), hs_swap->ext_table);

	return (prev >= 0) && (ext_idx(hs_swap, ext_id) == prev + 1);
}

static bool next_is_cont(struct hybridswap *hs_swap, int ext_id, int mcg_id)
{
	int next;

	if (is_last_idx(ext_idx(hs_swap, ext_id), mcg_idx(hs_swap, mcg_id),
			hs_swap->ext_table))
		return false;
	next = next_idx(ext_idx(hs_swap, ext_id), hs_swap->ext_table);

	return (next >= 0) && (ext_idx(hs_swap, ext_id) + 1 == next);
}

static void ext_fragment_sub(struct hybridswap *hs_swap, int ext_id)
{
	bool prev_flag = false;
	bool next_flag = false;
	int mcg_id;
	struct hybridswap_stat *stat = hybridswap_get_stat_obj();

	if (!stat) {
		log_err("NULL stat\n");
		return;
	}

	if (!hs_swap->ext_table) {
		log_err("NULL table\n");
		return;
	}
	if (ext_id < 0 || ext_id >= hs_swap->nr_exts) {
		log_err("ext = %d invalid\n", ext_id);
		return;
	}

	mcg_id = hs_list_get_mcgid(ext_idx(hs_swap, ext_id), hs_swap->ext_table);
	if (mcg_id <= 0 || mcg_id >= hs_swap->nr_mcgs) {
		log_err("mcg_id = %d invalid\n", mcg_id);
		return;
	}

	atomic64_dec(&stat->ext_cnt);
	hs_swap->mcg_id_cnt[mcg_id]--;
	if (hs_swap->mcg_id_cnt[mcg_id] == 0) {
		atomic64_dec(&stat->mcg_cnt);
		atomic64_dec(&stat->frag_cnt);
		return;
	}

	prev_flag = prev_is_cont(hs_swap, ext_id, mcg_id);
	next_flag = next_is_cont(hs_swap, ext_id, mcg_id);

	if (fragment_dec(prev_flag, next_flag, stat))
		atomic64_dec(&stat->frag_cnt);
}

static void ext_fragment_add(struct hybridswap *hs_swap, int ext_id)
{
	bool prev_flag = false;
	bool next_flag = false;
	int mcg_id;
	struct hybridswap_stat *stat = hybridswap_get_stat_obj();

	if (!stat) {
		log_err("NULL stat\n");
		return;
	}

	if (!hs_swap->ext_table) {
		log_err("NULL table\n");
		return;
	}
	if (ext_id < 0 || ext_id >= hs_swap->nr_exts) {
		log_err("ext = %d invalid\n", ext_id);
		return;
	}

	mcg_id = hs_list_get_mcgid(ext_idx(hs_swap, ext_id), hs_swap->ext_table);
	if (mcg_id <= 0 || mcg_id >= hs_swap->nr_mcgs) {
		log_err("mcg_id = %d invalid\n", mcg_id);
		return;
	}

	atomic64_inc(&stat->ext_cnt);
	if (hs_swap->mcg_id_cnt[mcg_id] == 0) {
		hs_swap->mcg_id_cnt[mcg_id]++;
		atomic64_inc(&stat->frag_cnt);
		atomic64_inc(&stat->mcg_cnt);
		return;
	}
	hs_swap->mcg_id_cnt[mcg_id]++;

	prev_flag = prev_is_cont(hs_swap, ext_id, mcg_id);
	next_flag = next_is_cont(hs_swap, ext_id, mcg_id);

	if (fragment_inc(prev_flag, next_flag, stat))
		atomic64_inc(&stat->frag_cnt);
}

static int extent_bit2id(struct hybridswap *hs_swap, int bit)
{
	if (bit < 0 || bit >= hs_swap->nr_exts) {
		log_err("bit = %d invalid\n", bit);
		return -EINVAL;
	}

	return hs_swap->nr_exts - bit - 1;
}

static int extent_id2bit(struct hybridswap *hs_swap, int id)
{
	if (id < 0 || id >= hs_swap->nr_exts) {
		log_err("id = %d invalid\n", id);
		return -EINVAL;
	}

	return hs_swap->nr_exts - id - 1;
}

int obj_idx(struct hybridswap *hs_swap, int idx)
{
	if (!hs_swap) {
		log_err("NULL hs_swap\n");
		return -EINVAL;
	}
	if (idx < 0 || idx >= hs_swap->nr_objs) {
		log_err("idx = %d invalid\n", idx);
		return -EINVAL;
	}

	return idx;
}

int ext_idx(struct hybridswap *hs_swap, int idx)
{
	if (!hs_swap) {
		log_err("NULL hs_swap\n");
		return -EINVAL;
	}
	if (idx < 0 || idx >= hs_swap->nr_exts) {
		log_err("idx = %d invalid\n", idx);
		return -EINVAL;
	}

	return idx + hs_swap->nr_objs;
}

int mcg_idx(struct hybridswap *hs_swap, int idx)
{
	if (!hs_swap) {
		log_err("NULL hs_swap\n");
		return -EINVAL;
	}
	if (idx <= 0 || idx >= hs_swap->nr_mcgs) {
		log_err("idx = %d invalid, nr_mcgs %d\n", idx,
			hs_swap->nr_mcgs);
		return -EINVAL;
	}

	return idx + hs_swap->nr_objs + hs_swap->nr_exts;
}

static struct hs_list_head *get_obj_table_node(int idx, void *private)
{
	struct hybridswap *hs_swap = private;

	if (!hs_swap) {
		log_err("NULL hs_swap\n");
		return NULL;
	}
	if (idx < 0) {
		log_err("idx = %d invalid\n", idx);
		return NULL;
	}
	if (idx < hs_swap->nr_objs)
		return &hs_swap->lru[idx];
	idx -= hs_swap->nr_objs;
	if (idx < hs_swap->nr_exts)
		return &hs_swap->rmap[idx];
	idx -= hs_swap->nr_exts;
	if (idx > 0 && idx < hs_swap->nr_mcgs) {
		struct mem_cgroup *mcg = get_mem_cgroup(idx);

		if (!mcg)
			goto err_out;
		return (struct hs_list_head *)(&MEMCGRP_ITEM(mcg, zram_lru));
	}
err_out:
	log_err("idx = %d invalid, mcg is NULL\n", idx);

	return NULL;
}

static void free_obj_list_table(struct hybridswap *hs_swap)
{
	if (!hs_swap) {
		log_err("NULL hs_swap\n");
		return;
	}

	if (hs_swap->lru) {
		vfree(hs_swap->lru);
		hs_swap->lru = NULL;
	}
	if (hs_swap->rmap) {
		vfree(hs_swap->rmap);
		hs_swap->rmap = NULL;
	}

	kfree(hs_swap->obj_table);
	hs_swap->obj_table = NULL;
}

static int init_obj_list_table(struct hybridswap *hs_swap)
{
	int i;

	if (!hs_swap) {
		log_err("NULL hs_swap\n");
		return -EINVAL;
	}

	hs_swap->lru = vzalloc(sizeof(struct hs_list_head) * hs_swap->nr_objs);
	if (!hs_swap->lru) {
		log_err("hs_swap->lru alloc failed\n");
		goto err_out;
	}
	hs_swap->rmap = vzalloc(sizeof(struct hs_list_head) * hs_swap->nr_exts);
	if (!hs_swap->rmap) {
		log_err("hs_swap->rmap alloc failed\n");
		goto err_out;
	}
	hs_swap->obj_table = alloc_table(get_obj_table_node, hs_swap, GFP_KERNEL);
	if (!hs_swap->obj_table) {
		log_err("hs_swap->obj_table alloc failed\n");
		goto err_out;
	}
	for (i = 0; i < hs_swap->nr_objs; i++)
		hs_list_init(obj_idx(hs_swap, i), hs_swap->obj_table);
	for (i = 0; i < hs_swap->nr_exts; i++)
		hs_list_init(ext_idx(hs_swap, i), hs_swap->obj_table);

	log_info("hybridswap obj list table init OK.\n");
	return 0;
err_out:
	free_obj_list_table(hs_swap);
	log_err("hybridswap obj list table init failed.\n");

	return -ENOMEM;
}

static struct hs_list_head *get_ext_table_node(int idx, void *private)
{
	struct hybridswap *hs_swap = private;

	if (!hs_swap) {
		log_err("NULL hs_swap\n");
		return NULL;
	}

	if (idx < hs_swap->nr_objs)
		goto err_out;
	idx -= hs_swap->nr_objs;
	if (idx < hs_swap->nr_exts)
		return &hs_swap->ext[idx];
	idx -= hs_swap->nr_exts;
	if (idx > 0 && idx < hs_swap->nr_mcgs) {
		struct mem_cgroup *mcg = get_mem_cgroup(idx);

		if (!mcg)
			return NULL;
		return (struct hs_list_head *)(&MEMCGRP_ITEM(mcg, ext_lru));
	}
err_out:
	log_err("idx = %d invalid\n", idx);

	return NULL;
}

static void free_ext_list_table(struct hybridswap *hs_swap)
{
	if (!hs_swap) {
		log_err("NULL hs_swap\n");
		return;
	}

	if (hs_swap->ext)
		vfree(hs_swap->ext);

	kfree(hs_swap->ext_table);
}

static int init_ext_list_table(struct hybridswap *hs_swap)
{
	int i;

	if (!hs_swap) {
		log_err("NULL hs_swap\n");
		return -EINVAL;
	}
	hs_swap->ext = vzalloc(sizeof(struct hs_list_head) * hs_swap->nr_exts);
	if (!hs_swap->ext)
		goto err_out;
	hs_swap->ext_table = alloc_table(get_ext_table_node, hs_swap, GFP_KERNEL);
	if (!hs_swap->ext_table)
		goto err_out;
	for (i = 0; i < hs_swap->nr_exts; i++)
		hs_list_init(ext_idx(hs_swap, i), hs_swap->ext_table);
	log_info("hybridswap ext list table init OK.\n");
	return 0;
err_out:
	free_ext_list_table(hs_swap);
	log_err("hybridswap ext list table init failed.\n");

	return -ENOMEM;
}

void free_hybridswap(struct hybridswap *hs_swap)
{
	if (!hs_swap) {
		log_err("NULL hs_swap\n");
		return;
	}

	vfree(hs_swap->bitmap);
	vfree(hs_swap->ext_stored_pages);
	free_obj_list_table(hs_swap);
	free_ext_list_table(hs_swap);
	vfree(hs_swap);
}

struct hybridswap *alloc_hybridswap(unsigned long ori_size,
				    unsigned long comp_size)
{
	struct hybridswap *hs_swap = vzalloc(sizeof(struct hybridswap));

	if (!hs_swap) {
		log_err("hs_swap alloc failed\n");
		goto err_out;
	}
	if (comp_size & (EXTENT_SIZE - 1)) {
		log_err("disksize = %ld align invalid (32K align needed)\n",
			comp_size);
		goto err_out;
	}
	hs_swap->size = comp_size;
	hs_swap->nr_exts = comp_size >> EXTENT_SHIFT;
	hs_swap->nr_mcgs = MEM_CGROUP_ID_MAX;
	hs_swap->nr_objs = ori_size >> PAGE_SHIFT;
	hs_swap->bitmap = vzalloc(BITS_TO_LONGS(hs_swap->nr_exts) * sizeof(long));
	if (!hs_swap->bitmap) {
		log_err("hs_swap->bitmap alloc failed, %lu\n",
			BITS_TO_LONGS(hs_swap->nr_exts) * sizeof(long));
		goto err_out;
	}
	hs_swap->ext_stored_pages = vzalloc(sizeof(atomic_t) * hs_swap->nr_exts);
	if (!hs_swap->ext_stored_pages) {
		log_err("hs_swap->ext_stored_pages alloc failed\n");
		goto err_out;
	}
	if (init_obj_list_table(hs_swap)) {
		log_err("init obj list table failed\n");
		goto err_out;
	}
	if (init_ext_list_table(hs_swap)) {
		log_err("init ext list table failed\n");
		goto err_out;
	}
	log_info("hs_swap %p size %lu nr_exts %d nr_mcgs %d nr_objs %d\n",
		 hs_swap, hs_swap->size, hs_swap->nr_exts, hs_swap->nr_mcgs,
		 hs_swap->nr_objs);
	log_info("hs_swap init OK.\n");
	return hs_swap;
err_out:
	free_hybridswap(hs_swap);
	log_err("hs_swap init failed.\n");

	return NULL;
}

void hybridswap_check_extent(struct hybridswap *hs_swap)
{
	int i;

	if (!hs_swap)
		return;

	for (i = 0; i < hs_swap->nr_exts; i++) {
		int cnt = atomic_read(&hs_swap->ext_stored_pages[i]);
		int ext_id = ext_idx(hs_swap, i);
		bool priv = hs_list_test_priv(ext_id, hs_swap->ext_table);
		int mcg_id = hs_list_get_mcgid(ext_id, hs_swap->ext_table);

		if (cnt < 0 || (cnt > 0 && mcg_id == 0))
			log_err("%8d %8d %8d %8d %4d\n", i, cnt, ext_id,
				mcg_id, priv);
	}
}

void hybridswap_free_extent(struct hybridswap *hs_swap, int ext_id)
{
	if (!hs_swap) {
		log_err("NULL hs_swap\n");
		return;
	}
	if (ext_id < 0 || ext_id >= hs_swap->nr_exts) {
		log_err("INVALID ext %d\n", ext_id);
		return;
	}
	log_dbg("free ext id = %d.\n", ext_id);

	hs_list_set_mcgid(ext_idx(hs_swap, ext_id), hs_swap->ext_table, 0);
	if (!test_and_clear_bit(extent_id2bit(hs_swap, ext_id), hs_swap->bitmap)) {
		log_err("bit not set, ext = %d\n", ext_id);
		WARN_ON_ONCE(1);
	}
	atomic_dec(&hs_swap->stored_exts);
}

static int alloc_bitmap(unsigned long *bitmap, int max, int last_bit)
{
	int bit;

	if (!bitmap) {
		log_err("NULL bitmap.\n");
		return -EINVAL;
	}
retry:
	bit = find_next_zero_bit(bitmap, max, last_bit);
	if (bit == max) {
		if (last_bit == 0) {
			log_err("alloc bitmap failed.\n");
			return -ENOSPC;
		}
		last_bit = 0;
		goto retry;
	}
	if (test_and_set_bit(bit, bitmap))
		goto retry;

	return bit;
}

int hybridswap_alloc_extent(struct hybridswap *hs_swap, struct mem_cgroup *mcg)
{
	int last_bit;
	int bit;
	int ext_id;
	int mcg_id;

	if (!hs_swap) {
		log_err("NULL hs_swap\n");
		return -EINVAL;
	}
	if (!mcg) {
		log_err("NULL mcg\n");
		return -EINVAL;
	}

	last_bit = atomic_read(&hs_swap->last_alloc_bit);
	log_dbg("last_bit = %d.\n", last_bit);
	bit = alloc_bitmap(hs_swap->bitmap, hs_swap->nr_exts, last_bit);
	if (bit < 0) {
		log_err("alloc bitmap failed.\n");
		return bit;
	}
	ext_id = extent_bit2id(hs_swap, bit);
	mcg_id = hs_list_get_mcgid(ext_idx(hs_swap, ext_id), hs_swap->ext_table);
	if (mcg_id) {
		log_err("already has mcg %d, ext %d\n",
			mcg_id, ext_id);
		goto err_out;
	}
	hs_list_set_mcgid(ext_idx(hs_swap, ext_id), hs_swap->ext_table, mcg->id.id);

	atomic_set(&hs_swap->last_alloc_bit, bit);
	atomic_inc(&hs_swap->stored_exts);
	log_dbg("extent %d init OK.\n", ext_id);
	log_dbg("mcg_id = %d, ext id = %d\n", mcg->id.id, ext_id);

	return ext_id;
err_out:
	clear_bit(bit, hs_swap->bitmap);
	WARN_ON_ONCE(1);
	return -EBUSY;
}

int get_extent(struct hybridswap *hs_swap, int ext_id)
{
	int mcg_id;

	if (!hs_swap) {
		log_err("NULL hs_swap\n");
		return -EINVAL;
	}
	if (ext_id < 0 || ext_id >= hs_swap->nr_exts) {
		log_err("ext = %d invalid\n", ext_id);
		return -EINVAL;
	}

	if (!hs_list_clear_priv(ext_idx(hs_swap, ext_id), hs_swap->ext_table))
		return -EBUSY;
	mcg_id = hs_list_get_mcgid(ext_idx(hs_swap, ext_id), hs_swap->ext_table);
	if (mcg_id) {
		ext_fragment_sub(hs_swap, ext_id);
		hs_list_del(ext_idx(hs_swap, ext_id), mcg_idx(hs_swap, mcg_id),
			    hs_swap->ext_table);
	}
	log_dbg("ext id = %d\n", ext_id);

	return ext_id;
}

void put_extent(struct hybridswap *hs_swap, int ext_id)
{
	int mcg_id;

	if (!hs_swap) {
		log_err("NULL hs_swap\n");
		return;
	}
	if (ext_id < 0 || ext_id >= hs_swap->nr_exts) {
		log_err("ext = %d invalid\n", ext_id);
		return;
	}

	mcg_id = hs_list_get_mcgid(ext_idx(hs_swap, ext_id), hs_swap->ext_table);
	if (mcg_id) {
		hs_lock_list(mcg_idx(hs_swap, mcg_id), hs_swap->ext_table);
		hs_list_add_nolock(ext_idx(hs_swap, ext_id), mcg_idx(hs_swap, mcg_id),
				   hs_swap->ext_table);
		ext_fragment_add(hs_swap, ext_id);
		hs_unlock_list(mcg_idx(hs_swap, mcg_id), hs_swap->ext_table);
	}
	if (!hs_list_set_priv(ext_idx(hs_swap, ext_id), hs_swap->ext_table)) {
		log_err("private not set, ext = %d\n", ext_id);
		WARN_ON_ONCE(1);
		return;
	}
	log_dbg("put extent %d.\n", ext_id);
}

int get_memcg_extent(struct hybridswap *hs_swap, struct mem_cgroup *mcg)
{
	int mcg_id;
	int ext_id = -ENOENT;
	int idx;

	if (!hs_swap) {
		log_err("NULL hs_swap\n");
		return -EINVAL;
	}
	if (!hs_swap->ext_table) {
		log_err("NULL table\n");
		return -EINVAL;
	}
	if (!mcg) {
		log_err("NULL mcg\n");
		return -EINVAL;
	}

	mcg_id = mcg->id.id;
	hs_lock_list(mcg_idx(hs_swap, mcg_id), hs_swap->ext_table);
	hs_list_for_each_entry(idx, mcg_idx(hs_swap, mcg_id), hs_swap->ext_table)
		if (hs_list_clear_priv(idx, hs_swap->ext_table)) {
			ext_id = idx - hs_swap->nr_objs;
			break;
		}
	if (ext_id >= 0 && ext_id < hs_swap->nr_exts) {
		ext_fragment_sub(hs_swap, ext_id);
		hs_list_del_nolock(idx, mcg_idx(hs_swap, mcg_id), hs_swap->ext_table);
		log_dbg("ext id = %d\n", ext_id);
	}
	hs_unlock_list(mcg_idx(hs_swap, mcg_id), hs_swap->ext_table);

	return ext_id;
}

int get_memcg_zram_entry(struct hybridswap *hs_swap, struct mem_cgroup *mcg)
{
	int mcg_id, idx;
	int index = -ENOENT;

	if (!hs_swap) {
		log_err("NULL hs_swap\n");
		return -EINVAL;
	}
	if (!hs_swap->obj_table) {
		log_err("NULL table\n");
		return -EINVAL;
	}
	if (!mcg) {
		log_err("NULL mcg\n");
		return -EINVAL;
	}

	mcg_id = mcg->id.id;
	hs_lock_list(mcg_idx(hs_swap, mcg_id), hs_swap->obj_table);
	hs_list_for_each_entry(idx, mcg_idx(hs_swap, mcg_id), hs_swap->obj_table) {
		index = idx;
		break;
	}
	hs_unlock_list(mcg_idx(hs_swap, mcg_id), hs_swap->obj_table);

	return index;
}

int get_extent_zram_entry(struct hybridswap *hs_swap, int ext_id)
{
	int index = -ENOENT;
	int idx;

	if (!hs_swap) {
		log_err("NULL hs_swap\n");
		return -EINVAL;
	}
	if (!hs_swap->obj_table) {
		log_err("NULL table\n");
		return -EINVAL;
	}
	if (ext_id < 0 || ext_id >= hs_swap->nr_exts) {
		log_err("ext = %d invalid\n", ext_id);
		return -EINVAL;
	}

	hs_lock_list(ext_idx(hs_swap, ext_id), hs_swap->obj_table);
	hs_list_for_each_entry(idx, ext_idx(hs_swap, ext_id), hs_swap->obj_table) {
		index = idx;
		break;
	}
	hs_unlock_list(ext_idx(hs_swap, ext_id), hs_swap->obj_table);

	return index;
}

#define esentry_extid(e) ((e) >> EXTENT_SHIFT)

void zram_set_memcg(struct zram *zram, u32 index, int mcg_id)
{
	hs_list_set_mcgid(obj_idx(zram->hs_swap, index),
			  zram->hs_swap->obj_table, mcg_id);
}

struct mem_cgroup *zram_get_memcg(struct zram *zram, u32 index)
{
	unsigned short mcg_id;

	mcg_id = hs_list_get_mcgid(obj_idx(zram->hs_swap, index),
				   zram->hs_swap->obj_table);

	return get_mem_cgroup(mcg_id);
}

int zram_get_memcg_coldest_index(struct hybridswap *hs_swap,
				 struct mem_cgroup *mcg,
				 int *index, int max_cnt)
{
	int cnt = 0;
	u32 i, tmp;

	if (!hs_swap) {
		log_err("NULL hs_swap\n");
		return 0;
	}
	if (!hs_swap->obj_table) {
		log_err("NULL table\n");
		return 0;
	}
	if (!mcg) {
		log_err("NULL mcg\n");
		return 0;
	}
	if (!index) {
		log_err("NULL index\n");
		return 0;
	}

	hs_lock_list(mcg_idx(hs_swap, mcg->id.id), hs_swap->obj_table);
	hs_list_for_each_entry_reverse_safe(i, tmp,
					    mcg_idx(hs_swap, mcg->id.id), hs_swap->obj_table) {
		if (i >= (u32)hs_swap->nr_objs) {
			log_err("index = %d invalid\n", i);
			continue;
		}
		index[cnt++] = i;
		if (cnt >= max_cnt)
			break;
	}
	hs_unlock_list(mcg_idx(hs_swap, mcg->id.id), hs_swap->obj_table);

	return cnt;
}

int zram_rmap_get_extent_index(struct hybridswap *hs_swap,
			       int ext_id, int *index)
{
	int cnt = 0;
	u32 i;

	if (!hs_swap) {
		log_err("NULL hs_swap\n");
		return 0;
	}
	if (!hs_swap->obj_table) {
		log_err("NULL table\n");
		return 0;
	}
	if (!index) {
		log_err("NULL index\n");
		return 0;
	}
	if (ext_id < 0 || ext_id >= hs_swap->nr_exts) {
		log_err("ext = %d invalid\n", ext_id);
		return 0;
	}

	hs_lock_list(ext_idx(hs_swap, ext_id), hs_swap->obj_table);
	hs_list_for_each_entry(i, ext_idx(hs_swap, ext_id), hs_swap->obj_table) {
		if (cnt >= (int)EXTENT_MAX_OBJ_CNT) {
			WARN_ON_ONCE(1);
			break;
		}
		index[cnt++] = i;
	}
	hs_unlock_list(ext_idx(hs_swap, ext_id), hs_swap->obj_table);

	return cnt;
}

void zram_lru_add(struct zram *zram, u32 index, struct mem_cgroup *memcg)
{
	unsigned long size;
	struct hybridswap_stat *stat = hybridswap_get_stat_obj();

	if (!stat) {
		log_err("NULL stat\n");
		return;
	}
	if (!zram || !zram->hs_swap) {
		log_err("NULL zram or hs_swap\n");
		return;
	}
	if (index >= (u32)zram->hs_swap->nr_objs) {
		log_err("index = %d invalid\n", index);
		return;
	}
	if (zram_test_flag(zram, index, ZRAM_WB)) {
		log_err("WB object, index = %d\n", index);
		return;
	}
	if (zram_test_flag(zram, index, ZRAM_SAME))
		return;

	zram_set_memcg(zram, index, memcg->id.id);
	hs_list_add(obj_idx(zram->hs_swap, index),
		    mcg_idx(zram->hs_swap, memcg->id.id),
		    zram->hs_swap->obj_table);

	size = zram_get_obj_size(zram, index);

	atomic64_add(size, &MEMCGRP_ITEM(memcg, zram_stored_size));
	atomic64_inc(&MEMCGRP_ITEM(memcg, zram_page_size));
	atomic64_add(size, &stat->zram_stored_size);
	atomic64_inc(&stat->zram_stored_pages);
}

void zram_lru_add_tail(struct zram *zram, u32 index, struct mem_cgroup *mcg)
{
	unsigned long size;
	struct hybridswap_stat *stat = hybridswap_get_stat_obj();

	if (!stat) {
		log_err("NULL stat\n");
		return;
	}
	if (!zram || !zram->hs_swap) {
		log_err("NULL zram or hs_swap\n");
		return;
	}
	if (!mcg || !MEMCGRP_ITEM(mcg, zram) || !MEMCGRP_ITEM(mcg, zram)->hs_swap) {
		log_err("invalid mcg\n");
		return;
	}
	if (index >= (u32)zram->hs_swap->nr_objs) {
		log_err("index = %d invalid\n", index);
		return;
	}
	if (zram_test_flag(zram, index, ZRAM_WB)) {
		log_err("WB object, index = %d\n", index);
		return;
	}
	if (zram_test_flag(zram, index, ZRAM_SAME))
		return;

	zram_set_memcg(zram, index, mcg->id.id);
	hs_list_add_tail(obj_idx(zram->hs_swap, index),
			 mcg_idx(zram->hs_swap, mcg->id.id),
			 zram->hs_swap->obj_table);

	size = zram_get_obj_size(zram, index);

	atomic64_add(size, &MEMCGRP_ITEM(mcg, zram_stored_size));
	atomic64_inc(&MEMCGRP_ITEM(mcg, zram_page_size));
	atomic64_add(size, &stat->zram_stored_size);
	atomic64_inc(&stat->zram_stored_pages);
}

void zram_lru_del(struct zram *zram, u32 index)
{
	struct mem_cgroup *mcg = NULL;
	unsigned long size;
	struct hybridswap_stat *stat = hybridswap_get_stat_obj();

	if (!stat) {
		log_err("NULL stat\n");
		return;
	}
	if (!zram || !zram->hs_swap) {
		log_err("NULL zram\n");
		return;
	}
	if (index >= (u32)zram->hs_swap->nr_objs) {
		log_err("index = %d invalid\n", index);
		return;
	}
	if (zram_test_flag(zram, index, ZRAM_WB)) {
		log_err("WB object, index = %d\n", index);
		return;
	}

	mcg = zram_get_memcg(zram, index);
	if (!mcg || !MEMCGRP_ITEM(mcg, zram) || !MEMCGRP_ITEM(mcg, zram)->hs_swap)
		return;
	if (zram_test_flag(zram, index, ZRAM_SAME))
		return;

	size = zram_get_obj_size(zram, index);
	hs_list_del(obj_idx(zram->hs_swap, index),
		    mcg_idx(zram->hs_swap, mcg->id.id),
		    zram->hs_swap->obj_table);
	zram_set_memcg(zram, index, 0);

	atomic64_sub(size, &MEMCGRP_ITEM(mcg, zram_stored_size));
	atomic64_dec(&MEMCGRP_ITEM(mcg, zram_page_size));
	atomic64_sub(size, &stat->zram_stored_size);
	atomic64_dec(&stat->zram_stored_pages);
}

void zram_rmap_insert(struct zram *zram, u32 index)
{
	unsigned long eswpentry;
	u32 ext_id;

	if (!zram || !zram->hs_swap) {
		log_err("NULL zram or hs_swap\n");
		return;
	}
	if (index >= (u32)zram->hs_swap->nr_objs) {
		log_err("index = %d invalid\n", index);
		return;
	}

	eswpentry = zram_get_handle(zram, index);
	ext_id = esentry_extid(eswpentry);
	hs_list_add_tail(obj_idx(zram->hs_swap, index),
			 ext_idx(zram->hs_swap, ext_id),
			 zram->hs_swap->obj_table);
}

void zram_rmap_erase(struct zram *zram, u32 index)
{
	unsigned long eswpentry;
	u32 ext_id;

	if (!zram || !zram->hs_swap) {
		log_err("NULL zram or hs_swap\n");
		return;
	}
	if (index >= (u32)zram->hs_swap->nr_objs) {
		log_err("index = %d invalid\n", index);
		return;
	}

	eswpentry = zram_get_handle(zram, index);
	ext_id = esentry_extid(eswpentry);
	hs_list_del(obj_idx(zram->hs_swap, index),
		    ext_idx(zram->hs_swap, ext_id),
		    zram->hs_swap->obj_table);
}

#define PRE_EOL_INFO_OVER_VAL 2
#define LIFE_TIME_EST_OVER_VAL 8
#define DEFAULT_STORED_WM_RATIO 90

struct zs_ext_para {
	struct hybridswap_page_pool *pool;
	size_t alloc_size;
	bool fast;
	bool nofail;
};

struct hybridswap_cfg {
	atomic_t enable;
	atomic_t reclaim_in_enable;
	struct hybridswap_stat *stat;
	struct workqueue_struct *reclaim_wq;
	struct zram *zram;

	atomic_t dev_life;
	unsigned long quota_day;
	struct timer_list lpc_timer;
	struct work_struct lpc_work;
};

struct hybridswap_cfg global_settings;

#define DEVICE_NAME_LEN 64
static char loop_device[DEVICE_NAME_LEN];

void *hybridswap_malloc(size_t size, bool fast, bool nofail)
{
	void *mem = NULL;

	if (likely(fast)) {
		mem = kzalloc(size, GFP_ATOMIC);
		if (likely(mem || !nofail))
			return mem;
	}

	mem = kzalloc(size, GFP_NOIO);

	return mem;
}

void hybridswap_free(const void *mem)
{
	kfree(mem);
}

struct page *hybridswap_alloc_page_common(void *data, gfp_t gfp)
{
	struct page *page = NULL;
	struct zs_ext_para *ext_para = (struct zs_ext_para *)data;

	if (ext_para->pool) {
		spin_lock(&ext_para->pool->page_pool_lock);
		if (!list_empty(&ext_para->pool->page_pool_list)) {
			page = list_first_entry(&ext_para->pool->page_pool_list,
						struct page, lru);
			list_del(&page->lru);
		}
		spin_unlock(&ext_para->pool->page_pool_lock);
	}

	if (!page) {
		if (ext_para->fast) {
			page = alloc_page(GFP_ATOMIC);
			if (likely(page))
				goto out;
		}
		if (ext_para->nofail)
			page = alloc_page(GFP_NOIO);
		else
			page = alloc_page(gfp);
	}
out:
	return page;
}

unsigned long hybridswap_zsmalloc(struct zs_pool *zs_pool,
				  size_t size, struct hybridswap_page_pool *pool)
{
	gfp_t gfp = __GFP_DIRECT_RECLAIM | __GFP_KSWAPD_RECLAIM |
		__GFP_NOWARN | __GFP_HIGHMEM |	__GFP_MOVABLE;
	return zs_malloc_oplus(zs_pool, size, gfp);
}

unsigned long zram_zsmalloc(struct zs_pool *zs_pool, size_t size, gfp_t gfp)
{
	return zs_malloc_oplus(zs_pool, size, gfp);
}

struct page *hybridswap_alloc_page(struct hybridswap_page_pool *pool,
				   gfp_t gfp, bool fast, bool nofail)
{
	struct zs_ext_para ext_para;

	ext_para.pool = pool;
	ext_para.fast = fast;
	ext_para.nofail = nofail;

	return hybridswap_alloc_page_common((void *)&ext_para, gfp);
}

void hybridswap_page_recycle(struct page *page, struct hybridswap_page_pool *pool)
{
	if (pool) {
		spin_lock(&pool->page_pool_lock);
		list_add(&page->lru, &pool->page_pool_list);
		spin_unlock(&pool->page_pool_lock);
	} else {
		__free_page(page);
	}
}

bool hybridswap_reclaim_in_enable(void)
{
	return !!atomic_read(&global_settings.reclaim_in_enable);
}

void hybridswap_set_reclaim_in_disable(void)
{
	atomic_set(&global_settings.reclaim_in_enable, false);
}

void hybridswap_set_reclaim_in_enable(bool en)
{
	atomic_set(&global_settings.reclaim_in_enable, en ? 1 : 0);
}

bool hybridswap_core_enabled(void)
{
#ifdef CONFIG_CONT_PTE_HUGEPAGE_64K_ZRAM
	if (chp_supported)
		return false;
#endif
	return !!atomic_read(&global_settings.enable);
}

void hybridswap_set_enable(bool en)
{
	hybridswap_set_reclaim_in_enable(en);

	if (!hybridswap_core_enabled())
		atomic_set(&global_settings.enable, en ? 1 : 0);
}

struct hybridswap_stat *hybridswap_get_stat_obj(void)
{
	return global_settings.stat;
}

bool hybridswap_dev_life(void)
{
	return !!atomic_read(&global_settings.dev_life);
}

void hybridswap_set_dev_life(bool en)
{
	atomic_set(&global_settings.dev_life, en ? 1 : 0);
}

unsigned long hybridswap_quota_day(void)
{
	return global_settings.quota_day;
}

void hybridswap_set_quota_day(unsigned long val)
{
	global_settings.quota_day = val;
}

bool hybridswap_reach_life_protect(void)
{
	struct hybridswap_stat *stat = hybridswap_get_stat_obj();
	unsigned long quota = hybridswap_quota_day();

	if (hybridswap_dev_life())
		quota /= 10;
	return atomic64_read(&stat->reclaimin_bytes_daily) > quota;
}

static void hybridswap_life_protect_ctrl_work(struct work_struct *work)
{
	struct tm tm;
	struct timespec64 ts;
	struct hybridswap_stat *stat = hybridswap_get_stat_obj();

	ktime_get_real_ts64(&ts);
	time64_to_tm(ts.tv_sec - sys_tz.tz_minuteswest * 60, 0, &tm);

	if (tm.tm_hour > 2)
		atomic64_set(&stat->reclaimin_bytes_daily, 0);
}

static void hybridswap_life_protect_ctrl_timer(struct timer_list *t)
{
	schedule_work(&global_settings.lpc_work);
	mod_timer(&global_settings.lpc_timer,
		  jiffies + HYBRIDSWAP_CHECK_INTERVAL * HZ);
}

void hybridswap_close_bdev(struct block_device *bdev, struct file *backing_dev, struct zram *zram)
{
	if (bdev)
		blkdev_put(bdev, zram);

	if (backing_dev)
		filp_close(backing_dev, NULL);
}

struct file *hybridswap_open_bdev(const char *file_name, struct zram *zram)
{
	struct file *backing_dev = NULL;

	backing_dev = filp_open_block(file_name, O_RDWR|O_LARGEFILE, 0);
	if (unlikely(IS_ERR(backing_dev))) {
		log_err("open the %s failed! eno = %ld\n",
			file_name, PTR_ERR(backing_dev));
		backing_dev = NULL;
		return NULL;
	}

	if (unlikely(!S_ISBLK(backing_dev->f_mapping->host->i_mode))) {
		log_err("%s isn't a blk device\n", file_name);
		hybridswap_close_bdev(NULL, backing_dev, zram);
		return NULL;
	}

	return backing_dev;
}

int hybridswap_bind(struct zram *zram, const char *file_name)
{
	struct file *backing_dev = NULL;
	struct inode *inode = NULL;
	unsigned long nr_pages;
	struct block_device *bdev = NULL;
	int err;

	backing_dev = hybridswap_open_bdev(file_name, zram);
	if (unlikely(!backing_dev))
		return -EINVAL;

	inode = backing_dev->f_mapping->host;
	bdev = blkdev_get_by_dev(inode->i_rdev,
				 FMODE_READ | FMODE_WRITE, zram, NULL);
	if (IS_ERR(bdev)) {
		log_err("%s blkdev_get failed!\n", file_name);
		err = PTR_ERR(bdev);
		bdev = NULL;
		goto out;
	}

	nr_pages = (unsigned long)i_size_read(inode) >> PAGE_SHIFT;
	err = set_blocksize(bdev, PAGE_SIZE);
	if (unlikely(err)) {
		hybp(HS_LOG_ERR,
		     "%s set blocksize failed! eno = %d\n", file_name, err);
		goto out;
	}

	zram->bdev = bdev;
	zram->backing_dev = backing_dev;
	zram->nr_pages = nr_pages;
	return 0;

out:
	hybridswap_close_bdev(bdev, backing_dev, zram);

	return err;
}

void hybridswap_unbind(struct zram *zram)
{
	struct block_device *bdev;

	if (!zram->backing_dev)
		return;

	bdev = zram->bdev;
	blkdev_put(bdev, zram);
	filp_close(zram->backing_dev, NULL);
	zram->backing_dev = NULL;
	zram->bdev = NULL;
	zram->nr_pages = 0;
	memset(loop_device, 0, DEVICE_NAME_LEN);
}

static inline unsigned long get_original_used_swap(void)
{
	struct sysinfo val;

	si_swapinfo(&val);

	return val.totalswap - val.freeswap;
}

void hybridswap_stat_init(struct hybridswap_stat *stat)
{
	int i;

	atomic64_set(&stat->reclaimin_cnt, 0);
	atomic64_set(&stat->reclaimin_bytes, 0);
	atomic64_set(&stat->reclaimin_real_load, 0);
	atomic64_set(&stat->dropped_ext_size, 0);
	atomic64_set(&stat->reclaimin_bytes_daily, 0);
	atomic64_set(&stat->reclaimin_pages, 0);
	atomic64_set(&stat->reclaimin_infight, 0);
	atomic64_set(&stat->batchout_cnt, 0);
	atomic64_set(&stat->batchout_bytes, 0);
	atomic64_set(&stat->batchout_real_load, 0);
	atomic64_set(&stat->batchout_pages, 0);
	atomic64_set(&stat->batchout_inflight, 0);
	atomic64_set(&stat->fault_cnt, 0);
	atomic64_set(&stat->hybridswap_fault_cnt, 0);
	atomic64_set(&stat->reout_pages, 0);
	atomic64_set(&stat->reout_bytes, 0);
	atomic64_set(&stat->zram_stored_pages, 0);
	atomic64_set(&stat->zram_stored_size, 0);
	atomic64_set(&stat->stored_pages, 0);
	atomic64_set(&stat->stored_size, 0);
	atomic64_set(&stat->notify_free, 0);
	atomic64_set(&stat->frag_cnt, 0);
	atomic64_set(&stat->mcg_cnt, 0);
	atomic64_set(&stat->ext_cnt, 0);
	atomic64_set(&stat->miss_free, 0);
	atomic64_set(&stat->mcgid_clear, 0);
	atomic64_set(&stat->skip_track_cnt, 0);
	atomic64_set(&stat->null_memcg_skip_track_cnt, 0);
	atomic64_set(&stat->used_swap_pages, get_original_used_swap());
	atomic64_set(&stat->stored_wm_ratio, DEFAULT_STORED_WM_RATIO);

	for (i = 0; i < SCENE_MAX; ++i) {
		atomic64_set(&stat->io_fail_cnt[i], 0);
		atomic64_set(&stat->alloc_fail_cnt[i], 0);
		atomic64_set(&stat->lat[i].latency_tot, 0);
		atomic64_set(&stat->lat[i].latency_max, 0);
	}

	stat->record.num = 0;
	spin_lock_init(&stat->record.lock);
}

static bool hybridswap_global_setting_init(struct zram *zram)
{
	if (unlikely(global_settings.stat))
		return false;

	global_settings.zram = zram;
	hybridswap_set_enable(false);
	global_settings.stat = hybridswap_malloc(sizeof(struct hybridswap_stat), false, true);
	if (unlikely(!global_settings.stat)) {
		log_err("global stat allocation failed!\n");
		return false;
	}

	hybridswap_stat_init(global_settings.stat);
	global_settings.reclaim_wq = alloc_workqueue("reclaim",
						     WQ_CPU_INTENSIVE, 0);
	if (unlikely(!global_settings.reclaim_wq)) {
		log_err("reclaim workqueue allocation failed!\n");
		hybridswap_free(global_settings.stat);
		global_settings.stat = NULL;

		return false;
	}

	global_settings.quota_day = HYBRIDSWAP_QUOTA_DAY;
	INIT_WORK(&global_settings.lpc_work, hybridswap_life_protect_ctrl_work);
	global_settings.lpc_timer.expires = jiffies + HYBRIDSWAP_CHECK_INTERVAL * HZ;
	timer_setup(&global_settings.lpc_timer, hybridswap_life_protect_ctrl_timer, 0);
	add_timer(&global_settings.lpc_timer);

	log_dbg("global settings init success\n");
	return true;
}

void hybridswap_global_setting_deinit(void)
{
	destroy_workqueue(global_settings.reclaim_wq);
	hybridswap_free(global_settings.stat);
	global_settings.stat = NULL;
	global_settings.zram = NULL;
	global_settings.reclaim_wq = NULL;
}

struct workqueue_struct *hybridswap_get_reclaim_workqueue(void)
{
	return global_settings.reclaim_wq;
}

static int hybridswap_core_init(struct zram *zram)
{
	int ret;

	if (loop_device[0] == '\0') {
		log_err("please setting loop_device first\n");
		return -EINVAL;
	}

	if (!hybridswap_global_setting_init(zram))
		return -EINVAL;

	ret = hybridswap_bind(zram, loop_device);
	if (unlikely(ret)) {
		log_err("bind storage device failed! %d\n", ret);
		hybridswap_global_setting_deinit();
	}

	return 0;
}

int hybridswap_set_enable_init(bool en)
{
	int ret;

	if (hybridswap_core_enabled() || !en)
		return 0;

	if (!global_settings.stat) {
		log_err("global_settings.stat is null!\n");

		return -EINVAL;
	}

	ret = hybridswap_mgr_init(global_settings.zram);
	if (unlikely(ret)) {
		log_err("init manager failed! %d\n", ret);

		return -EINVAL;
	}

	ret = hybridswap_schedule_init();
	if (unlikely(ret)) {
		log_err("init schedule failed! %d\n", ret);
		hybridswap_mgr_deinit(global_settings.zram);

		return -EINVAL;
	}

	return 0;
}

ssize_t hybridswap_core_enable_store(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t len)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (unlikely(ret)) {
		log_err("val is error!\n");

		return -EINVAL;
	}

	if (hybridswap_set_enable_init(!!val))
		return -EINVAL;

	hybridswap_set_enable(!!val);

	return len;
}

ssize_t hybridswap_core_enable_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	int len = snprintf(buf, PAGE_SIZE, "hybridswap %s reclaim_in %s\n",
			   hybridswap_core_enabled() ? "enable" : "disable",
			   hybridswap_reclaim_in_enable() ? "enable" : "disable");

	return len;
}

ssize_t backing_dev_store(struct device *dev,
			  struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram;
	int ret = 0;

	if (len > (DEVICE_NAME_LEN - 1)) {
		log_err("buf %s len %lu is too long\n", buf, len);
		return -EINVAL;
	}

	memcpy(loop_device, buf, len);
	loop_device[len] = '\0';

	zram = dev_to_zram(dev);
	down_write(&zram->init_lock);
	if (zram->disksize == 0) {
		log_err("disksize is 0\n");
		goto out;
	}

	ret = hybridswap_core_init(zram);
	if (ret)
		log_err("core_init init failed\n");

out:
	up_write(&zram->init_lock);
	return len;
}

ssize_t backing_dev_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	int len = 0;

	len = sprintf(buf, "%s\n", loop_device);

	return len;
}

ssize_t hybridswap_loop_device_store(struct device *dev,
				     struct device_attribute *attr, const char *buf, size_t len)
{
#ifdef CONFIG_CONT_PTE_HUGEPAGE_64K_ZRAM
	struct zram *zram;

	if (!chp_supported)
		goto origin;

	if (strncmp("chp", buf, 3) != 0) {
		log_err("failed to set chp loop device\n");
		return -EINVAL;
	}

	zram = dev_to_zram(dev);
	down_write(&zram->init_lock);
	if (!hybridswap_global_setting_init(zram))
		log_err("setting init failed\n");
	up_write(&zram->init_lock);
	return len;
origin:
#endif
	return backing_dev_store(dev, attr, buf, len);
}
ssize_t hybridswap_loop_device_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
#ifdef CONFIG_CONT_PTE_HUGEPAGE_64K_ZRAM
	if (chp_supported)
		return sprintf(buf, "chp\n");
#endif
	return backing_dev_show(dev, attr, buf);
}

ssize_t hybridswap_dev_life_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t len)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (unlikely(ret)) {
		log_err("val is error!\n");

		return -EINVAL;
	}

	hybridswap_set_dev_life(!!val);

	return len;
}

ssize_t hybridswap_dev_life_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	int len = 0;

	len = sprintf(buf, "%s\n",
		      hybridswap_dev_life() ? "enable" : "disable");

	return len;
}

ssize_t hybridswap_quota_day_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t len)
{
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 0, &val);
	if (unlikely(ret)) {
		log_err("val is error!\n");

		return -EINVAL;
	}

	hybridswap_set_quota_day(val);

	return len;
}

ssize_t hybridswap_quota_day_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	int len = 0;

	len = sprintf(buf, "%lu\n", hybridswap_quota_day());

	return len;
}

ssize_t hybridswap_zram_increase_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t len)
{
	char *type_buf = NULL;
	unsigned long val;
	struct zram *zram = dev_to_zram(dev);

	type_buf = strstrip((char *)buf);
	if (kstrtoul(type_buf, 0, &val))
		return -EINVAL;

	zram->increase_nr_pages = (val << 8);
	return len;
}

ssize_t hybridswap_zram_increase_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	ssize_t size = 0;
	struct zram *zram = dev_to_zram(dev);

	size += scnprintf(buf + size, PAGE_SIZE - size,
			  "%lu\n", zram->increase_nr_pages >> 8);

	return size;
}

int mem_cgroup_stored_wm_ratio_write(struct cgroup_subsys_state *css, struct cftype *cft, s64 val)
{
	if (val > MAX_RATIO || val < MIN_RATIO)
		return -EINVAL;

	if (!global_settings.stat)
		return -EINVAL;

	atomic64_set(&global_settings.stat->stored_wm_ratio, val);

	return 0;
}

s64 mem_cgroup_stored_wm_ratio_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	if (!global_settings.stat)
		return -EINVAL;

	return atomic64_read(&global_settings.stat->stored_wm_ratio);
}

int hybridswap_stored_info(unsigned long *total, unsigned long *used)
{
	if (!total || !used)
		return -EINVAL;

	if (!global_settings.stat || !global_settings.zram) {
		*total = 0;
		*used = 0;
		return 0;
	}

	*used = atomic64_read(&global_settings.stat->ext_cnt) * EXTENT_PG_CNT;
	*total = global_settings.zram->nr_pages;

	return 0;
}

bool hybridswap_stored_wm_ok(void)
{
	unsigned long ratio, stored_pages, total_pages, wm_ratio;
	int ret;

	if (!hybridswap_core_enabled())
		return false;

	ret = hybridswap_stored_info(&total_pages, &stored_pages);
	if (ret)
		return false;

	ratio = (stored_pages * 100) / (total_pages + 1);
	wm_ratio = atomic64_read(&global_settings.stat->stored_wm_ratio);

	return ratio <= wm_ratio;
}

int hybridswap_core_enable(void)
{
	int ret;

	ret = hybridswap_set_enable_init(true);
	if (ret) {
		log_err("set true failed, ret=%d\n", ret);
		return ret;
	}

	hybridswap_set_enable(true);
	return 0;
}

void hybridswap_core_disable(void)
{
	(void)hybridswap_set_enable_init(false);
	hybridswap_set_enable(false);
}

#define esentry_extid(e) ((e) >> EXTENT_SHIFT)
#define esentry_pgid(e) (((e) & ((1 << EXTENT_SHIFT) - 1)) >> PAGE_SHIFT)
#define esentry_pgoff(e) ((e) & (PAGE_SIZE - 1))

static struct io_extent *alloc_io_extent(struct hybridswap_page_pool *pool,
					 bool fast, bool nofail)
{
	int i;
	struct io_extent *io_ext = hybridswap_malloc(sizeof(struct io_extent),
						     fast, nofail);

	if (!io_ext) {
		log_err("alloc io_ext failed\n");
		return NULL;
	}

	io_ext->ext_id = -EINVAL;
	io_ext->pool = pool;
	for (i = 0; i < (int)EXTENT_PG_CNT; i++) {
		io_ext->pages[i] = hybridswap_alloc_page(pool, GFP_ATOMIC,
							 fast, nofail);
		if (!io_ext->pages[i]) {
			log_err("alloc page[%d] failed\n", i);
			goto page_free;
		}
	}
	return io_ext;
page_free:
	for (i = 0; i < (int)EXTENT_PG_CNT; i++)
		if (io_ext->pages[i])
			hybridswap_page_recycle(io_ext->pages[i], pool);
	hybridswap_free(io_ext);

	return NULL;
}

static void discard_io_extent(struct io_extent *io_ext, unsigned int op)
{
	struct zram *zram = NULL;
	int i;

	if (!io_ext) {
		log_err("NULL io_ext\n");
		return;
	}
	if (!io_ext->mcg)
		zram = io_ext->zram;
	else
		zram = MEMCGRP_ITEM(io_ext->mcg, zram);
	if (!zram) {
		log_err("NULL zram\n");
		goto out;
	}
	for (i = 0; i < (int)EXTENT_PG_CNT; i++)
		if (io_ext->pages[i])
			hybridswap_page_recycle(io_ext->pages[i], io_ext->pool);
	if (io_ext->ext_id < 0)
		goto out;
	log_dbg("ext = %d, op = %d\n", io_ext->ext_id, op);
	if (op == REQ_OP_READ) {
		put_extent(zram->hs_swap, io_ext->ext_id);
		goto out;
	}
	for (i = 0; i < io_ext->cnt; i++) {
		u32 index = io_ext->index[i];

		zram_slot_lock(zram, index);
		if (io_ext->mcg)
			zram_lru_add_tail(zram, index, io_ext->mcg);
		zram_clear_flag(zram, index, ZRAM_UNDER_WB);
		zram_slot_unlock(zram, index);
	}
	hybridswap_free_extent(zram->hs_swap, io_ext->ext_id);
out:
	hybridswap_free(io_ext);
}

static void copy_to_pages(u8 *src, struct page *pages[],
			  unsigned long eswpentry, int size)
{
	u8 *dst = NULL;
	int pg_id = esentry_pgid(eswpentry);
	int offset = esentry_pgoff(eswpentry);

	if (!src) {
		log_err("NULL src\n");
		return;
	}
	if (!pages) {
		log_err("NULL pages\n");
		return;
	}
	if (size < 0 || size > (int)PAGE_SIZE) {
		log_err("size = %d invalid\n", size);
		return;
	}
	dst = page_to_virt(pages[pg_id]);
	if (offset + size <= (int)PAGE_SIZE) {
		memcpy(dst + offset, src, size);
		return;
	}
	if (pg_id == EXTENT_PG_CNT - 1) {
		log_err("ext overflow, addr = %lx, size = %d\n",
			eswpentry, size);
		return;
	}
	memcpy(dst + offset, src, PAGE_SIZE - offset);
	dst = page_to_virt(pages[pg_id + 1]);
	memcpy(dst, src + PAGE_SIZE - offset, offset + size - PAGE_SIZE);
}

static void copy_from_pages(u8 *dst, struct page *pages[],
			    unsigned long eswpentry, int size)
{
	u8 *src = NULL;
	int pg_id = esentry_pgid(eswpentry);
	int offset = esentry_pgoff(eswpentry);

	if (!dst) {
		log_err("NULL dst\n");
		return;
	}
	if (!pages) {
		log_err("NULL pages\n");
		return;
	}
	if (size < 0 || size > (int)PAGE_SIZE) {
		log_err("size = %d invalid\n", size);
		return;
	}

	src = page_to_virt(pages[pg_id]);
	if (offset + size <= (int)PAGE_SIZE) {
		memcpy(dst, src + offset, size);
		return;
	}
	if (pg_id == EXTENT_PG_CNT - 1) {
		log_err("ext overflow, addr = %lx, size = %d\n",
			eswpentry, size);
		return;
	}
	memcpy(dst, src + offset, PAGE_SIZE - offset);
	src = page_to_virt(pages[pg_id + 1]);
	memcpy(dst + PAGE_SIZE - offset, src, offset + size - PAGE_SIZE);
}

static bool zram_test_skip(struct zram *zram, u32 index, struct mem_cgroup *mcg)
{
	if (zram_test_flag(zram, index, ZRAM_WB))
		return true;
	if (zram_test_flag(zram, index, ZRAM_UNDER_WB))
		return true;
	if (zram_test_flag(zram, index, ZRAM_BATCHING_OUT))
		return true;
	if (zram_test_flag(zram, index, ZRAM_SAME))
		return true;
	if (mcg != zram_get_memcg(zram, index))
		return true;
	if (!zram_get_obj_size(zram, index))
		return true;

	return false;
}

static bool zram_test_overwrite(struct zram *zram, u32 index, int ext_id)
{
	if (!zram_test_flag(zram, index, ZRAM_WB))
		return true;
	if (ext_id != esentry_extid(zram_get_handle(zram, index)))
		return true;

	return false;
}

static void update_size_info(struct zram *zram, u32 index)
{
	struct hybridswap_stat *stat;
	int size = zram_get_obj_size(zram, index);
	struct mem_cgroup *mcg;
	memcg_hybs_t *hybs;
	int ext_id;

	if (!zram_test_flag(zram, index, ZRAM_IN_BD))
		return;

	ext_id = esentry_extid(zram_get_handle(zram, index));
	log_info("ext_id %d index %d\n", ext_id, index);

	if (ext_id >= 0 && ext_id < zram->hs_swap->nr_exts)
		atomic_dec(&zram->hs_swap->ext_stored_pages[ext_id]);
	else {
		log_err("ext = %d invalid\n", ext_id);
		ext_id = -1;
	}

	stat = hybridswap_get_stat_obj();
	if (stat) {
		atomic64_add(size, &stat->dropped_ext_size);
		atomic64_sub(size, &stat->stored_size);
		atomic64_dec(&stat->stored_pages);
	} else
		log_err("NULL stat\n");

	mcg = zram_get_memcg(zram, index);
	if (mcg) {
		hybs = MEMCGRP_ITEM_DATA(mcg);

		if (hybs) {
			atomic64_sub(size, &hybs->hybridswap_stored_size);
			atomic64_dec(&hybs->hybridswap_stored_pages);
		} else
			log_err("NULL hybs\n");
	} else
		log_err("NULL mcg\n");
	zram_clear_flag(zram, index, ZRAM_IN_BD);
}

static void move_to_hybridswap(struct zram *zram, u32 index,
			       unsigned long eswpentry, struct mem_cgroup *mcg)
{
	int size;
	struct hybridswap_stat *stat = hybridswap_get_stat_obj();

	if (!stat) {
		log_err("NULL stat\n");
		return;
	}
	if (!zram) {
		log_err("NULL zram\n");
		return;
	}
	if (index >= (u32)zram->hs_swap->nr_objs) {
		log_err("index = %d invalid\n", index);
		return;
	}
	if (!mcg) {
		log_err("NULL mcg\n");
		return;
	}

	size = zram_get_obj_size(zram, index);

	zram_clear_flag(zram, index, ZRAM_UNDER_WB);

	zs_free_oplus(zram->mem_pool, zram_get_handle(zram, index));
	atomic64_sub(size, &zram->stats.compr_data_size);
	atomic64_dec(&zram->stats.pages_stored);

	zram_set_memcg(zram, index, mcg->id.id);
	zram_set_flag(zram, index, ZRAM_IN_BD);
	zram_set_flag(zram, index, ZRAM_WB);
	zram_set_obj_size(zram, index, size);
	if (size == PAGE_SIZE)
		zram_set_flag(zram, index, ZRAM_HUGE);
	zram_set_handle(zram, index, eswpentry);
	zram_rmap_insert(zram, index);

	atomic64_add(size, &stat->stored_size);
	atomic64_add(size, &MEMCGRP_ITEM(mcg, hybridswap_stored_size));
	atomic64_inc(&stat->stored_pages);
	atomic_inc(&zram->hs_swap->ext_stored_pages[esentry_extid(eswpentry)]);
	atomic64_inc(&MEMCGRP_ITEM(mcg, hybridswap_stored_pages));
}

static void __move_to_zram(struct zram *zram, u32 index, unsigned long handle,
			   struct io_extent *io_ext)
{
	struct hybridswap_stat *stat = hybridswap_get_stat_obj();
	struct mem_cgroup *mcg = io_ext->mcg;
	int size = zram_get_obj_size(zram, index);

	if (!stat) {
		log_err("NULL stat\n");
		return;
	}

	zram_slot_lock(zram, index);
	if (zram_test_overwrite(zram, index, io_ext->ext_id)) {
		zram_slot_unlock(zram, index);
		zs_free_oplus(zram->mem_pool, handle);
		return;
	}
	zram_rmap_erase(zram, index);
	zram_set_handle(zram, index, handle);
	zram_clear_flag(zram, index, ZRAM_WB);
	if (mcg)
		zram_lru_add_tail(zram, index, mcg);
	zram_set_flag(zram, index, ZRAM_FROM_HYBRIDSWAP);
	atomic64_add(size, &zram->stats.compr_data_size);
	atomic64_inc(&zram->stats.pages_stored);
	zram_clear_flag(zram, index, ZRAM_IN_BD);
	zram_slot_unlock(zram, index);

	atomic64_inc(&stat->batchout_pages);
	atomic64_sub(size, &stat->stored_size);
	atomic64_dec(&stat->stored_pages);
	atomic64_add(size, &stat->batchout_real_load);
	atomic_dec(&zram->hs_swap->ext_stored_pages[io_ext->ext_id]);
	if (mcg) {
		atomic64_sub(size, &MEMCGRP_ITEM(mcg, hybridswap_stored_size));
		atomic64_dec(&MEMCGRP_ITEM(mcg, hybridswap_stored_pages));
	}
}

static int move_to_zram(struct zram *zram, u32 index, struct io_extent *io_ext)
{
	unsigned long handle, eswpentry;
	struct mem_cgroup *mcg = NULL;
	int size, i;
	u8 *dst = NULL;

	if (!zram) {
		log_err("NULL zram\n");
		return -EINVAL;
	}
	if (index >= (u32)zram->hs_swap->nr_objs) {
		log_err("index = %d invalid\n", index);
		return -EINVAL;
	}
	if (!io_ext) {
		log_err("NULL io_ext\n");
		return -EINVAL;
	}

	mcg = io_ext->mcg;
	zram_slot_lock(zram, index);
	eswpentry = zram_get_handle(zram, index);
	if (zram_test_overwrite(zram, index, io_ext->ext_id)) {
		zram_slot_unlock(zram, index);
		return 0;
	}
	size = zram_get_obj_size(zram, index);
	zram_slot_unlock(zram, index);

	for (i = esentry_pgid(eswpentry) - 1; i >= 0 && io_ext->pages[i]; i--) {
		hybridswap_page_recycle(io_ext->pages[i], io_ext->pool);
		io_ext->pages[i] = NULL;
	}
	handle = hybridswap_zsmalloc(zram->mem_pool, size, io_ext->pool);
	if (!handle) {
		log_err("alloc handle failed, size = %d\n", size);
		return -ENOMEM;
	}
	dst = zs_map_object_oplus(zram->mem_pool, handle, ZS_MM_WO);
	copy_from_pages(dst, io_ext->pages, eswpentry, size);
	zs_unmap_object_oplus(zram->mem_pool, handle);
	__move_to_zram(zram, index, handle, io_ext);

	return 0;
}

static int extent_unlock(struct io_extent *io_ext)
{
	int ext_id;
	struct mem_cgroup *mcg = NULL;
	struct zram *zram = NULL;
	int k;
	unsigned long eswpentry;
	int real_load = 0, size;

	if (!io_ext) {
		log_err("NULL io_ext\n");
		goto out;
	}

	mcg = io_ext->mcg;
	if (!mcg) {
		log_err("NULL mcg\n");
		goto out;
	}
	zram = MEMCGRP_ITEM(mcg, zram);
	if (!zram) {
		log_err("NULL zram\n");
		goto out;
	}
	ext_id = io_ext->ext_id;
	if (ext_id < 0)
		goto out;

	ext_id = io_ext->ext_id;
	if (MEMCGRP_ITEM(mcg, in_swapin))
		goto out;
	log_dbg("add ext_id = %d, cnt = %d.\n",
		ext_id, io_ext->cnt);
	eswpentry = ((unsigned long)ext_id) << EXTENT_SHIFT;
	for (k = 0; k < io_ext->cnt; k++)
		zram_slot_lock(zram, io_ext->index[k]);
	for (k = 0; k < io_ext->cnt; k++) {
		move_to_hybridswap(zram, io_ext->index[k], eswpentry, mcg);
		size = zram_get_obj_size(zram, io_ext->index[k]);
		eswpentry += size;
		real_load += size;
	}
	put_extent(zram->hs_swap, ext_id);
	io_ext->ext_id = -EINVAL;
	for (k = 0; k < io_ext->cnt; k++)
		zram_slot_unlock(zram, io_ext->index[k]);
	log_dbg("add extent OK.\n");
out:
	discard_io_extent(io_ext, REQ_OP_WRITE);
	if (mcg)
		css_put(&mcg->css);

	return real_load;
}

static void extent_add(struct io_extent *io_ext,
		       enum hybridswap_scene scene)
{
	struct mem_cgroup *mcg = NULL;
	struct zram *zram = NULL;
	int ext_id;
	int k;

	if (!io_ext) {
		log_err("NULL io_ext\n");
		return;
	}

	mcg = io_ext->mcg;
	if (!mcg)
		zram = io_ext->zram;
	else
		zram = MEMCGRP_ITEM(mcg, zram);
	if (!zram) {
		log_err("NULL zram\n");
		goto out;
	}

	ext_id = io_ext->ext_id;
	if (ext_id < 0)
		goto out;

	io_ext->cnt = zram_rmap_get_extent_index(zram->hs_swap,
						 ext_id,
						 io_ext->index);
	log_dbg("ext_id = %d, cnt = %d.\n", ext_id, io_ext->cnt);
	for (k = 0; k < io_ext->cnt; k++) {
		int ret = move_to_zram(zram, io_ext->index[k], io_ext);

		if (ret < 0)
			goto out;
	}
	log_dbg("extent add OK, free ext_id = %d.\n", ext_id);
	hybridswap_free_extent(zram->hs_swap, io_ext->ext_id);
	io_ext->ext_id = -EINVAL;
	if (mcg) {
		atomic64_inc(&MEMCGRP_ITEM(mcg, hybridswap_inextcnt));
		atomic_dec(&MEMCGRP_ITEM(mcg, hybridswap_extcnt));
	}
out:
	discard_io_extent(io_ext, REQ_OP_READ);
	if (mcg)
		css_put(&mcg->css);
}

static void extent_clear(struct zram *zram, int ext_id)
{
	int *index = NULL;
	int cnt;
	int k;
	struct hybridswap_stat *stat = hybridswap_get_stat_obj();

	if (!stat) {
		log_err("NULL stat\n");
		return;
	}

	index = kzalloc(sizeof(int) * EXTENT_MAX_OBJ_CNT, GFP_NOIO);
	if (!index)
		index = kzalloc(sizeof(int) * EXTENT_MAX_OBJ_CNT,
				GFP_NOIO | __GFP_NOFAIL);

	cnt = zram_rmap_get_extent_index(zram->hs_swap, ext_id, index);

	for (k = 0; k < cnt; k++) {
		zram_slot_lock(zram, index[k]);
		if (zram_test_overwrite(zram, index[k], ext_id)) {
			zram_slot_unlock(zram, index[k]);
			continue;
		}
		zram_set_memcg(zram, index[k], 0);
		zram_set_flag(zram, index[k], ZRAM_MCGID_CLEAR);
		atomic64_inc(&stat->mcgid_clear);
		zram_slot_unlock(zram, index[k]);
	}

	kfree(index);
}

static int shrink_entry(struct zram *zram, u32 index, struct io_extent *io_ext,
			unsigned long ext_off)
{
	unsigned long handle;
	int size;
	u8 *src = NULL;
	struct hybridswap_stat *stat = hybridswap_get_stat_obj();

	if (!stat) {
		log_err("NULL stat\n");
		return -EINVAL;
	}
	if (!zram) {
		log_err("NULL zram\n");
		return -EINVAL;
	}
	if (index >= (u32)zram->hs_swap->nr_objs) {
		log_err("index = %d invalid\n", index);
		return -EINVAL;
	}

	zram_slot_lock(zram, index);
	handle = zram_get_handle(zram, index);
	if (!handle || zram_test_skip(zram, index, io_ext->mcg)) {
		zram_slot_unlock(zram, index);
		return 0;
	}
	size = zram_get_obj_size(zram, index);
	if (ext_off + size > EXTENT_SIZE) {
		zram_slot_unlock(zram, index);
		return -ENOSPC;
	}

	src = zs_map_object_oplus(zram->mem_pool, handle, ZS_MM_RO);
	copy_to_pages(src, io_ext->pages, ext_off, size);
	zs_unmap_object_oplus(zram->mem_pool, handle);
	io_ext->index[io_ext->cnt++] = index;

	zram_lru_del(zram, index);
	zram_set_flag(zram, index, ZRAM_UNDER_WB);
	if (zram_test_flag(zram, index, ZRAM_FROM_HYBRIDSWAP)) {
		atomic64_inc(&stat->reout_pages);
		atomic64_add(size, &stat->reout_bytes);
	}
	zram_slot_unlock(zram, index);
	atomic64_inc(&stat->reclaimin_pages);

	return size;
}

static int shrink_entry_list(struct io_extent *io_ext)
{
	struct mem_cgroup *mcg = NULL;
	struct zram *zram = NULL;
	unsigned long stored_size;
	int *swap_index = NULL;
	int swap_cnt, k;
	int swap_size = 0;

	if (!io_ext) {
		log_err("NULL io_ext\n");
		return -EINVAL;
	}

	mcg = io_ext->mcg;
	zram = MEMCGRP_ITEM(mcg, zram);
	log_dbg("mcg = %d\n", mcg->id.id);
	stored_size = atomic64_read(&MEMCGRP_ITEM(mcg, zram_stored_size));
	log_dbg("zram_stored_size = %ld\n", stored_size);
	if (stored_size < EXTENT_SIZE) {
		log_info("%lu is smaller than EXTENT_SIZE\n", stored_size);
		return -ENOENT;
	}

	swap_index = kzalloc(sizeof(int) * EXTENT_MAX_OBJ_CNT, GFP_NOIO);
	if (!swap_index)
		return -ENOMEM;
	io_ext->ext_id = hybridswap_alloc_extent(zram->hs_swap, mcg);
	if (io_ext->ext_id < 0) {
		kfree(swap_index);
		return io_ext->ext_id;
	}
	swap_cnt = zram_get_memcg_coldest_index(zram->hs_swap, mcg, swap_index,
						EXTENT_MAX_OBJ_CNT);
	io_ext->cnt = 0;
	for (k = 0; k < swap_cnt && swap_size < (int)EXTENT_SIZE; k++) {
		int size = shrink_entry(zram, swap_index[k], io_ext, swap_size);

		if (size < 0)
			break;
		swap_size += size;
	}
	kfree(swap_index);
	log_dbg("fill extent = %d, cnt = %d, overhead = %ld.\n",
		io_ext->ext_id, io_ext->cnt, EXTENT_SIZE - swap_size);
	if (swap_size == 0) {
		log_err("swap_size = 0, free ext_id = %d.\n",
			io_ext->ext_id);
		hybridswap_free_extent(zram->hs_swap, io_ext->ext_id);
		io_ext->ext_id = -EINVAL;
		return -ENOENT;
	}

	return swap_size;
}

void hybridswap_mgr_deinit(struct zram *zram)
{
	if (!zram) {
		log_err("NULL zram\n");
		return;
	}

	free_hybridswap(zram->hs_swap);
	zram->hs_swap = NULL;
}

int hybridswap_mgr_init(struct zram *zram)
{
	int ret;

	if (!zram) {
		log_err("NULL zram\n");
		ret = -EINVAL;
		goto out;
	}

	zram->hs_swap = alloc_hybridswap(zram->disksize,
					 zram->nr_pages << PAGE_SHIFT);
	if (!zram->hs_swap) {
		ret = -ENOMEM;
		goto out;
	}
	return 0;
out:
	hybridswap_mgr_deinit(zram);

	return ret;
}

void hybridswap_memcg_init(struct zram *zram,
			   struct mem_cgroup *memcg)
{
	memcg_hybs_t *hybs;

	if (!memcg || !zram || !zram->hs_swap) {
		log_err("invalid zram or mcg_hyb\n");
		return;
	}

	hs_list_init(mcg_idx(zram->hs_swap, memcg->id.id), zram->hs_swap->obj_table);
	hs_list_init(mcg_idx(zram->hs_swap, memcg->id.id), zram->hs_swap->ext_table);

	hybs = MEMCGRP_ITEM_DATA(memcg);
	hybs->in_swapin = false;
	atomic64_set(&hybs->zram_stored_size, 0);
	atomic64_set(&hybs->zram_page_size, 0);
	atomic64_set(&hybs->hybridswap_stored_pages, 0);
	atomic64_set(&hybs->hybridswap_stored_size, 0);
	atomic64_set(&hybs->hybridswap_allfaultcnt, 0);
	atomic64_set(&hybs->hybridswap_outcnt, 0);
	atomic64_set(&hybs->hybridswap_incnt, 0);
	atomic64_set(&hybs->hybridswap_faultcnt, 0);
	atomic64_set(&hybs->hybridswap_outextcnt, 0);
	atomic64_set(&hybs->hybridswap_inextcnt, 0);
	atomic_set(&hybs->hybridswap_extcnt, 0);
	atomic_set(&hybs->hybridswap_peakextcnt, 0);
	mutex_init(&hybs->swap_lock);

	smp_wmb();
	hybs->zram = zram;
	log_dbg("new memcg in zram, id = %d.\n", memcg->id.id);
}

void hybridswap_memcg_deinit(struct mem_cgroup *mcg)
{
	struct zram *zram = NULL;
	struct hybridswap *hs_swap = NULL;
	struct hybridswap_stat *stat = hybridswap_get_stat_obj();
	int last_index = -1;
	memcg_hybs_t *hybs;

	if (!stat) {
		log_err("NULL stat\n");
		return;
	}

	hybs = MEMCGRP_ITEM_DATA(mcg);
	if (!hybs->zram)
		return;

	zram = hybs->zram;
	if (!zram->hs_swap) {
		log_warn("mcg %p name %s id %d zram %p hs_swap is NULL\n",
			 mcg, hybs->name,   mcg->id.id, zram);
		return;
	}

	log_dbg("deinit mcg %d %s\n", mcg->id.id, hybs->name);
	if (mcg->id.id == 0)
		return;

	hs_swap = zram->hs_swap;
	while (1) {
		int index = get_memcg_zram_entry(hs_swap, mcg);

		if (index == -ENOENT)
			break;

		if (index < 0) {
			log_err("invalid index\n");
			return;
		}

		if (last_index == index) {
			log_err("dup index %d\n", index);
			dump_stack();
		}

		zram_slot_lock(zram, index);
		if (index == last_index || mcg == zram_get_memcg(zram, index)) {
			hs_list_del(obj_idx(zram->hs_swap, index),
				    mcg_idx(zram->hs_swap, mcg->id.id),
				    zram->hs_swap->obj_table);
			zram_set_memcg(zram, index, 0);
			zram_set_flag(zram, index, ZRAM_MCGID_CLEAR);
			atomic64_inc(&stat->mcgid_clear);
		}
		zram_slot_unlock(zram, index);
		last_index = index;
	}

	log_dbg("deinit mcg %d %s, entry done\n", mcg->id.id, hybs->name);
	while (1) {
		int ext_id = get_memcg_extent(hs_swap, mcg);

		if (ext_id == -ENOENT)
			break;

		extent_clear(zram, ext_id);
		hs_list_set_mcgid(ext_idx(hs_swap, ext_id), hs_swap->ext_table, 0);
		put_extent(hs_swap, ext_id);
	}
	log_dbg("deinit mcg %d %s, extent done\n", mcg->id.id, hybs->name);
	hybs->zram = NULL;
}
void hybridswap_zram_lru_add(struct zram *zram,
			     u32 index, struct mem_cgroup *memcg)
{
	if (!zram || !zram->hs_swap) {
		log_err("NULL zram or hs_swap\n");
		return;
	}
	if (index >= (u32)zram->hs_swap->nr_objs) {
		log_err("index = %d invalid\n", index);
		return;
	}

	zram_lru_add(zram, index, memcg);
}

void hybridswap_zram_lru_del(struct zram *zram, u32 index)
{
	struct hybridswap_stat *stat = hybridswap_get_stat_obj();

	if (!stat) {
		log_err("NULL stat\n");
		return;
	}
	if (!zram || !zram->hs_swap) {
		log_err("NULL zram or hs_swap\n");
		return;
	}
	if (index >= (u32)zram->hs_swap->nr_objs) {
		log_err("index = %d invalid\n", index);
		return;
	}

	zram_clear_flag(zram, index, ZRAM_FROM_HYBRIDSWAP);
	if (zram_test_flag(zram, index, ZRAM_MCGID_CLEAR)) {
		zram_clear_flag(zram, index, ZRAM_MCGID_CLEAR);
		atomic64_dec(&stat->mcgid_clear);
	}

	if (zram_test_flag(zram, index, ZRAM_WB)) {
		update_size_info(zram, index);
		zram_rmap_erase(zram, index);
		zram_clear_flag(zram, index, ZRAM_WB);
		zram_set_memcg(zram, index, 0);
		zram_set_handle(zram, index, 0);
	} else {
		zram_lru_del(zram, index);
	}
}

unsigned long hybridswap_extent_create(struct mem_cgroup *mcg,
				       int *ext_id,
				       struct hybridswap_buffer *buf,
				       void **private)
{
	struct io_extent *io_ext = NULL;
	int reclaim_size;

	if (!mcg) {
		log_err("NULL mcg\n");
		return 0;
	}
	if (!ext_id) {
		log_err("NULL ext_id\n");
		return 0;
	}
	(*ext_id) = -EINVAL;
	if (!buf) {
		log_err("NULL buf\n");
		return 0;
	}
	if (!private) {
		log_err("NULL private\n");
		return 0;
	}

	io_ext = alloc_io_extent(buf->pool, false, true);
	if (!io_ext)
		return 0;
	io_ext->mcg = mcg;
	reclaim_size = shrink_entry_list(io_ext);
	if (reclaim_size < 0) {
		discard_io_extent(io_ext, REQ_OP_WRITE);
		(*ext_id) = reclaim_size;
		return 0;
	}
	io_ext->real_load = reclaim_size;
	css_get(&mcg->css);
	(*ext_id) = io_ext->ext_id;
	buf->dest_pages = io_ext->pages;
	(*private) = io_ext;
	log_dbg("mcg = %d, ext_id = %d\n", mcg->id.id, io_ext->ext_id);

	return reclaim_size;
}

void hybridswap_extent_register(void *private, struct hybridswap_io_req *req)
{
	struct io_extent *io_ext = private;

	if (!io_ext) {
		log_err("NULL io_ext\n");
		return;
	}
	log_dbg("ext_id = %d\n", io_ext->ext_id);
	atomic64_add(extent_unlock(io_ext), &req->real_load);
}

void hybridswap_extent_objs_del(struct zram *zram, u32 index)
{
	int ext_id;
	struct mem_cgroup *mcg = NULL;
	unsigned long eswpentry;
	int size;
	struct hybridswap_stat *stat = hybridswap_get_stat_obj();

	if (!stat) {
		log_err("NULL stat\n");
		return;
	}
	if (!zram || !zram->hs_swap) {
		log_err("NULL zram\n");
		return;
	}
	if (index >= (u32)zram->hs_swap->nr_objs) {
		log_err("index = %d invalid\n", index);
		return;
	}
	if (!zram_test_flag(zram, index, ZRAM_WB)) {
		log_err("not WB object\n");
		return;
	}

	eswpentry = zram_get_handle(zram, index);
	size = zram_get_obj_size(zram, index);
	atomic64_sub(size, &stat->stored_size);
	atomic64_dec(&stat->stored_pages);
	atomic64_add(size, &stat->dropped_ext_size);
	mcg = zram_get_memcg(zram, index);
	if (mcg) {
		atomic64_sub(size, &MEMCGRP_ITEM(mcg, hybridswap_stored_size));
		atomic64_dec(&MEMCGRP_ITEM(mcg, hybridswap_stored_pages));
	}

	zram_clear_flag(zram, index, ZRAM_IN_BD);
	if (!atomic_dec_and_test(&zram->hs_swap->ext_stored_pages[esentry_extid(eswpentry)]))
		return;
	ext_id = get_extent(zram->hs_swap, esentry_extid(eswpentry));
	if (ext_id < 0)
		return;

	atomic64_inc(&stat->notify_free);
	if (mcg)
		atomic64_inc(&MEMCGRP_ITEM(mcg, hybridswap_ext_notify_free));
	log_dbg("free ext_id = %d\n", ext_id);
	hybridswap_free_extent(zram->hs_swap, ext_id);
}

int hybridswap_find_extent_by_idx(unsigned long eswpentry,
				  struct hybridswap_buffer *buf,
				  void **private)
{
	int ext_id;
	struct io_extent *io_ext = NULL;
	struct zram *zram = NULL;

	if (!buf) {
		log_err("NULL buf\n");
		return -EINVAL;
	}
	if (!private) {
		log_err("NULL private\n");
		return -EINVAL;
	}

	zram = buf->zram;
	ext_id = get_extent(zram->hs_swap, esentry_extid(eswpentry));
	if (ext_id < 0)
		return ext_id;
	io_ext = alloc_io_extent(buf->pool, true, true);
	if (!io_ext) {
		log_err("io_ext alloc failed\n");
		put_extent(zram->hs_swap, ext_id);
		return -ENOMEM;
	}

	io_ext->ext_id = ext_id;
	io_ext->zram = zram;
	io_ext->mcg = get_mem_cgroup(hs_list_get_mcgid(ext_idx(zram->hs_swap, ext_id),
						       zram->hs_swap->ext_table));
	if (io_ext->mcg)
		css_get(&io_ext->mcg->css);
	buf->dest_pages = io_ext->pages;
	(*private) = io_ext;
	log_dbg("get entry = %lx ext = %d\n", eswpentry, ext_id);

	return ext_id;
}

int hybridswap_find_extent_by_memcg(struct mem_cgroup *mcg,
				    struct hybridswap_buffer *buf,
				    void **private)
{
	int ext_id;
	struct io_extent *io_ext = NULL;

	if (!mcg) {
		log_err("NULL mcg\n");
		return -EINVAL;
	}
	if (!buf) {
		log_err("NULL buf\n");
		return -EINVAL;
	}
	if (!private) {
		log_err("NULL private\n");
		return -EINVAL;
	}

	ext_id = get_memcg_extent(MEMCGRP_ITEM(mcg, zram)->hs_swap, mcg);
	if (ext_id < 0)
		return ext_id;
	io_ext = alloc_io_extent(buf->pool, true, false);
	if (!io_ext) {
		log_err("io_ext alloc failed\n");
		put_extent(MEMCGRP_ITEM(mcg, zram)->hs_swap, ext_id);
		return -ENOMEM;
	}
	io_ext->ext_id = ext_id;
	io_ext->mcg = mcg;
	css_get(&mcg->css);
	buf->dest_pages = io_ext->pages;
	(*private) = io_ext;
	log_dbg("get mcg = %d, ext = %d\n", mcg->id.id, ext_id);

	return ext_id;
}

void hybridswap_extent_destroy(void *private, enum hybridswap_scene scene)
{
	struct io_extent *io_ext = private;

	if (!io_ext) {
		log_err("NULL io_ext\n");
		return;
	}

	log_dbg("ext_id = %d\n", io_ext->ext_id);
	extent_add(io_ext, scene);
}

void hybridswap_extent_exception(enum hybridswap_scene scene,
				 void *private)
{
	struct io_extent *io_ext = private;
	struct mem_cgroup *mcg = NULL;
	unsigned int op = (scene == SCENE_RECLAIM_IN) ?
		REQ_OP_WRITE : REQ_OP_READ;

	if (!io_ext) {
		log_err("NULL io_ext\n");
		return;
	}

	log_dbg("ext_id = %d, op = %d\n", io_ext->ext_id, op);
	mcg = io_ext->mcg;
	discard_io_extent(io_ext, op);
	if (mcg)
		css_put(&mcg->css);
}

struct mem_cgroup *hybridswap_zram_get_memcg(struct zram *zram, u32 index)
{
	return zram_get_memcg(zram, index);
}

#define HYBRIDSWAP_KEY_INDEX 0
#define HYBRIDSWAP_KEY_SIZE 64
#define HYBRIDSWAP_KEY_INDEX_SHIFT 3

#define HYBRIDSWAP_MAX_INFILGHT_NUM 256

#define HYBRIDSWAP_SECTOR_SHIFT 9
#define HYBRIDSWAP_PAGE_SIZE_SECTOR (PAGE_SIZE >> HYBRIDSWAP_SECTOR_SHIFT)

#define HYBRIDSWAP_READ_TIME 10
#define HYBRIDSWAP_WRITE_TIME 100
#define SCENE_FAULT_OUT_TIME 10

struct hybridswap_segment_time {
	ktime_t submit_bio;
	ktime_t end_io;
};

struct hybridswap_segment {
	sector_t segment_sector;
	int extent_cnt;
	int page_cnt;
	struct list_head io_entries;
	struct hybridswap_entry *io_entries_fifo[BIO_MAX_VECS];
	struct work_struct endio_work;
	struct hybridswap_io_req *req;
	struct hybridswap_segment_time time;
	u32 bio_result;
};

static u8 hybridswap_io_key[HYBRIDSWAP_KEY_SIZE];
#ifdef CONFIG_SCSI_UFS_ENHANCED_INLINE_CRYPTO_V3
static u8 hybridswap_io_metadata[METADATA_BYTE_IN_KDF];
#endif
static struct workqueue_struct *hybridswap_proc_read_workqueue;
static struct workqueue_struct *hybridswap_proc_write_workqueue;
bool hybridswap_schedule_init_flag;

static void hybridswap_stat_io_bytes(struct hybridswap_io_req *req)
{
	struct hybridswap_stat *stat = hybridswap_get_stat_obj();

	if (!stat || !req->page_cnt)
		return;

	if (req->io_para.scene == SCENE_RECLAIM_IN) {
		atomic64_add(req->page_cnt * PAGE_SIZE, &stat->reclaimin_bytes);
		atomic64_add(req->page_cnt * PAGE_SIZE, &stat->reclaimin_bytes_daily);
		atomic64_add(atomic64_read(&req->real_load), &stat->reclaimin_real_load);
		atomic64_inc(&stat->reclaimin_cnt);
	} else {
		atomic64_add(req->page_cnt * PAGE_SIZE, &stat->batchout_bytes);
		atomic64_inc(&stat->batchout_cnt);
	}
}

static void hybridswap_key_init(void)
{
	get_random_bytes(hybridswap_io_key, HYBRIDSWAP_KEY_SIZE);
#ifdef CONFIG_SCSI_UFS_ENHANCED_INLINE_CRYPTO_V3
	get_random_bytes(hybridswap_io_metadata, METADATA_BYTE_IN_KDF);
#endif
}

static void hybridswap_io_req_release(struct kref *ref)
{
	struct hybridswap_io_req *req =
		container_of(ref, struct hybridswap_io_req, refcount);

	if (req->io_para.complete_notify && req->io_para.private)
		req->io_para.complete_notify(req->io_para.private);

	kfree(req);
}

static void hybridswap_segment_free(struct hybridswap_io_req *req,
				    struct hybridswap_segment *segment)
{
	int i;

	for (i = 0; i < segment->extent_cnt; ++i) {
		INIT_LIST_HEAD(&segment->io_entries_fifo[i]->list);
		req->io_para.done_callback(segment->io_entries_fifo[i], -EIO, req);
	}
	kfree(segment);
}

static void hybridswap_limit_inflight(struct hybridswap_io_req *req)
{
	int ret;

	if (!req->limit_inflight_flag)
		return;

	if (atomic_read(&req->extent_inflight) >= HYBRIDSWAP_MAX_INFILGHT_NUM) {
		do {
			log_dbg("wait inflight start\n");
			ret = wait_event_timeout(req->io_wait,
						 atomic_read(&req->extent_inflight) <
						 HYBRIDSWAP_MAX_INFILGHT_NUM,
						 msecs_to_jiffies(100));
		} while (!ret);
	}
}

static void hybridswap_wait_io_finish(struct hybridswap_io_req *req)
{
	int ret;
	unsigned int wait_time;

	if (!req->wait_io_finish_flag || !req->page_cnt)
		return;

	if (req->io_para.scene == SCENE_FAULT_OUT) {
		log_dbg("fault out wait finish start\n");
		wait_for_completion_io_timeout(&req->io_end_flag,
					       MAX_SCHEDULE_TIMEOUT);

		return;
	}

	wait_time = (req->io_para.scene == SCENE_RECLAIM_IN) ?
		HYBRIDSWAP_WRITE_TIME : HYBRIDSWAP_READ_TIME;

	do {
		log_dbg("wait finish start\n");
		ret = wait_event_timeout(req->io_wait,
					 (!atomic_read(&req->extent_inflight)),
					 msecs_to_jiffies(wait_time));
	} while (!ret);
}

static void inc_hybridswap_inflight(struct hybridswap_segment *segment)
{
	mutex_lock(&segment->req->refmutex);
	kref_get(&segment->req->refcount);
	mutex_unlock(&segment->req->refmutex);
	atomic_add(segment->page_cnt, &segment->req->extent_inflight);
}

static void dec_hybridswap_inflight(struct hybridswap_io_req *req,
				    int num)
{
	if ((atomic_sub_return(num, &req->extent_inflight) <
	     HYBRIDSWAP_MAX_INFILGHT_NUM) && req->limit_inflight_flag &&
	    wq_has_sleeper(&req->io_wait))
		wake_up(&req->io_wait);
}

static void hybridswap_io_end_wake_up(struct hybridswap_io_req *req)
{
	if (req->io_para.scene == SCENE_FAULT_OUT) {
		complete(&req->io_end_flag);
		return;
	}

	if (wq_has_sleeper(&req->io_wait))
		wake_up(&req->io_wait);
}

static void hybridswap_io_entry_proc(struct hybridswap_segment *segment)
{
	int i;
	struct hybridswap_io_req *req = segment->req;
	struct hybridswap_record_stage *record = req->io_para.record;
	int page_num;
	ktime_t callback_start;
	unsigned long long callback_start_ravg_sum;

	for (i = 0; i < segment->extent_cnt; ++i) {
		INIT_LIST_HEAD(&segment->io_entries_fifo[i]->list);
		page_num = segment->io_entries_fifo[i]->pages_sz;
		log_dbg("extent_id[%d] %d page_num %d\n",
			i, segment->io_entries_fifo[i]->ext_id, page_num);
		callback_start = ktime_get();
		callback_start_ravg_sum = hybridswap_get_ravg_sum();
		if (req->io_para.done_callback)
			req->io_para.done_callback(segment->io_entries_fifo[i],
						   0, req);
		perf_async_set(record, STAGE_CALL_BACK,
			       callback_start, callback_start_ravg_sum);
		dec_hybridswap_inflight(req, page_num);
	}
}

static void hybridswap_io_err_record(enum hybridswap_stage_err point,
				     struct hybridswap_io_req *req, int ext_id)
{
	if (req->io_para.scene == SCENE_FAULT_OUT)
		hybridswap_error_record(point, 0, ext_id,
					req->io_para.record->task_comm);
}

static void hybridswap_stat_io_fail(enum hybridswap_scene scene)
{
	struct hybridswap_stat *stat = hybridswap_get_stat_obj();

	if (!stat || (scene >= SCENE_MAX))
		return;

	atomic64_inc(&stat->io_fail_cnt[scene]);
}

static void hybridswap_io_err_proc(struct hybridswap_io_req *req,
				   struct hybridswap_segment *segment)
{
	log_err("segment sector 0x%llx, extent_cnt %d\n",
		segment->segment_sector, segment->extent_cnt);
	log_err("scene %u, bio_result %u\n",
		req->io_para.scene, segment->bio_result);
	hybridswap_stat_io_fail(req->io_para.scene);
	hybridswap_io_err_record(SCENE_FAULT_OUT_IO_FAIL, req,
				 segment->io_entries_fifo[0]->ext_id);
	dec_hybridswap_inflight(req, segment->page_cnt);
	hybridswap_io_end_wake_up(req);
	hybridswap_segment_free(req, segment);
	kref_put_mutex(&req->refcount, hybridswap_io_req_release,
		       &req->refmutex);
}

static void hybridswap_io_end_work(struct work_struct *work)
{
	struct hybridswap_segment *segment =
		container_of(work, struct hybridswap_segment, endio_work);
	struct hybridswap_io_req *req = segment->req;
	struct hybridswap_record_stage *record = req->io_para.record;
	int old_nice = task_nice(current);
	ktime_t work_start;
	unsigned long long work_start_ravg_sum;

	if (unlikely(segment->bio_result)) {
		hybridswap_io_err_proc(req, segment);
		return;
	}

	log_dbg("segment sector 0x%llx, extent_cnt %d passed\n",
		segment->segment_sector, segment->extent_cnt);
	log_dbg("scene %u, bio_result %u passed\n",
		req->io_para.scene, segment->bio_result);

	set_user_nice(current, req->nice);

	perf_async_set(record, STAGE_SCHED_WORK,
		       segment->time.end_io, 0);
	work_start = ktime_get();
	work_start_ravg_sum = hybridswap_get_ravg_sum();

	hybridswap_io_entry_proc(segment);

	perf_async_set(record, STAGE_END_WORK, work_start,
		       work_start_ravg_sum);

	hybridswap_io_end_wake_up(req);

	kref_put_mutex(&req->refcount, hybridswap_io_req_release,
		       &req->refmutex);
	kfree(segment);

	set_user_nice(current, old_nice);
}

static void hybridswap_end_io(struct bio *bio)
{
	struct hybridswap_segment *segment = bio->bi_private;
	struct hybridswap_io_req *req = NULL;
	struct workqueue_struct *workqueue = NULL;
	struct hybridswap_record_stage *record = NULL;

	if (unlikely(!segment || !(segment->req))) {
		log_err("segment or req null\n");
		bio_put(bio);

		return;
	}

	req = segment->req;
	record = req->io_para.record;

	perf_async_set(record, STAGE_END_IO,
		       segment->time.submit_bio, 0);

	workqueue = (req->io_para.scene == SCENE_RECLAIM_IN) ?
		hybridswap_proc_write_workqueue : hybridswap_proc_read_workqueue;
	segment->time.end_io = ktime_get();
	segment->bio_result = bio->bi_status;

	queue_work(workqueue, &segment->endio_work);
	bio_put(bio);
}

static bool hybridswap_ext_merge_back(
				      struct hybridswap_segment *segment,
				      struct hybridswap_entry *io_entry)
{
	struct hybridswap_entry *tail_io_entry =
		list_last_entry(&segment->io_entries,
				struct hybridswap_entry, list);

	return ((tail_io_entry->addr +
		 tail_io_entry->pages_sz * HYBRIDSWAP_PAGE_SIZE_SECTOR) ==
		io_entry->addr);
}

static bool hybridswap_ext_merge_front(struct hybridswap_segment *segment,
				       struct hybridswap_entry *io_entry)
{
	struct hybridswap_entry *head_io_entry =
		list_first_entry(&segment->io_entries,
				 struct hybridswap_entry, list);

	return (head_io_entry->addr ==
		(io_entry->addr +
		 io_entry->pages_sz * HYBRIDSWAP_PAGE_SIZE_SECTOR));
}

static bool hybridswap_ext_merge(struct hybridswap_io_req *req,
				 struct hybridswap_entry *io_entry)
{
	struct hybridswap_segment *segment = req->segment;

	if (segment == NULL)
		return false;

	if ((segment->page_cnt + io_entry->pages_sz) > BIO_MAX_VECS)
		return false;

	if (hybridswap_ext_merge_front(segment, io_entry)) {
		list_add(&io_entry->list, &segment->io_entries);
		segment->io_entries_fifo[segment->extent_cnt++] = io_entry;
		segment->segment_sector = io_entry->addr;
		segment->page_cnt += io_entry->pages_sz;
		return true;
	}

	if (hybridswap_ext_merge_back(segment, io_entry)) {
		list_add_tail(&io_entry->list, &segment->io_entries);
		segment->io_entries_fifo[segment->extent_cnt++] = io_entry;
		segment->page_cnt += io_entry->pages_sz;
		return true;
	}

	return false;
}

static struct bio *hybridswap_bio_alloc(struct hybridswap_segment *segment)
{
	gfp_t gfp = (segment->req->io_para.scene != SCENE_RECLAIM_IN) ? GFP_ATOMIC : GFP_NOIO;
	struct bio *bio = bio_alloc(segment->req->io_para.bdev, BIO_MAX_VECS,
							REQ_OP_READ,
							gfp);

	if (!bio && (segment->req->io_para.scene == SCENE_FAULT_OUT))
		bio = bio_alloc(segment->req->io_para.bdev, BIO_MAX_VECS,
							REQ_OP_WRITE,
							GFP_NOIO);

	return bio;
}

static int hybridswap_bio_add_page(struct bio *bio,
				   struct hybridswap_segment *segment)
{
	int i;
	int k = 0;
	struct hybridswap_entry *io_entry = NULL;
	struct hybridswap_entry *tmp = NULL;

	list_for_each_entry_safe(io_entry, tmp, &segment->io_entries, list)  {
		for (i = 0; i < io_entry->pages_sz; i++) {
			io_entry->dest_pages[i]->index =
				bio->bi_iter.bi_sector + k;
			if (unlikely(!bio_add_page(bio,
						   io_entry->dest_pages[i], PAGE_SIZE, 0))) {
				return -EIO;
			}
			k += HYBRIDSWAP_PAGE_SIZE_SECTOR;
		}
	}

	return 0;
}

static void hybridswap_set_bio_opf(struct bio *bio,
				   struct hybridswap_segment *segment)
{
	if (segment->req->io_para.scene == SCENE_RECLAIM_IN) {
		bio->bi_opf |= REQ_BACKGROUND;
		return;
	}

	bio->bi_opf |= REQ_SYNC;
}

int hybridswap_submit_bio(struct hybridswap_segment *segment)
{
	unsigned int op =
		(segment->req->io_para.scene == SCENE_RECLAIM_IN) ?
		REQ_OP_WRITE : REQ_OP_READ;
	struct hybridswap_entry *head_io_entry =
		list_first_entry(&segment->io_entries,
				 struct hybridswap_entry, list);
	struct hybridswap_record_stage *record =
		segment->req->io_para.record;
	struct bio *bio = NULL;

	perf_latency_begin(record, STAGE_BIO_ALLOC);
	bio = hybridswap_bio_alloc(segment);
	perf_latency_end(record, STAGE_BIO_ALLOC);
	if (unlikely(!bio)) {
		log_err("bio is null.\n");
		hybridswap_io_err_record(SCENE_FAULT_OUT_BIO_ALLOC_FAIL,
					 segment->req, segment->io_entries_fifo[0]->ext_id);

		return -ENOMEM;
	}

	bio->bi_iter.bi_sector = segment->segment_sector;
	bio_set_dev(bio, segment->req->io_para.bdev);
	bio->bi_private = segment;
	bio->bi_opf = op | 0;
	bio->bi_end_io = hybridswap_end_io;
	hybridswap_set_bio_opf(bio, segment);

	if (unlikely(hybridswap_bio_add_page(bio, segment))) {
		bio_put(bio);
		log_err("bio_add_page fail\n");
		hybridswap_io_err_record(SCENE_FAULT_OUT_BIO_ADD_FAIL,
					 segment->req, segment->io_entries_fifo[0]->ext_id);

		return -EIO;
	}

	inc_hybridswap_inflight(segment);
	log_dbg("submit bio sector %llu ext_id %d\n",
		segment->segment_sector, head_io_entry->ext_id);
	log_dbg("extent_cnt %d scene %u\n",
		segment->extent_cnt, segment->req->io_para.scene);

	segment->req->page_cnt += segment->page_cnt;
	segment->req->segment_cnt++;
	segment->time.submit_bio = ktime_get();

	perf_latency_begin(record, STAGE_SUBMIT_BIO);
	submit_bio(bio);
	perf_latency_end(record, STAGE_SUBMIT_BIO);

	return 0;
}

static int hybridswap_new_segment_init(struct hybridswap_io_req *req,
				       struct hybridswap_entry *io_entry)
{
	gfp_t gfp = (req->io_para.scene != SCENE_RECLAIM_IN) ?
		GFP_ATOMIC : GFP_NOIO;
	struct hybridswap_segment *segment = NULL;
	struct hybridswap_record_stage *record = req->io_para.record;

	perf_latency_begin(record, STAGE_SEGMENT_ALLOC);
	segment = kzalloc(sizeof(struct hybridswap_segment), gfp);
	if (!segment && (req->io_para.scene == SCENE_FAULT_OUT))
		segment = kzalloc(sizeof(struct hybridswap_segment), GFP_NOIO);
	perf_latency_end(record, STAGE_SEGMENT_ALLOC);
	if (unlikely(!segment)) {
		hybridswap_io_err_record(SCENE_FAULT_OUT_SEGMENT_ALLOC_FAIL,
					 req, io_entry->ext_id);

		return -ENOMEM;
	}

	segment->req = req;
	INIT_LIST_HEAD(&segment->io_entries);
	list_add_tail(&io_entry->list, &segment->io_entries);
	segment->io_entries_fifo[segment->extent_cnt++] = io_entry;
	segment->page_cnt = io_entry->pages_sz;
	INIT_WORK(&segment->endio_work, hybridswap_io_end_work);
	segment->segment_sector = io_entry->addr;
	req->segment = segment;

	return 0;
}

static int hybridswap_io_submit(struct hybridswap_io_req *req,
				bool merge_flag)
{
	int ret;
	struct hybridswap_segment *segment = req->segment;

	if (!segment || ((merge_flag) && (segment->page_cnt < BIO_MAX_VECS)))
		return 0;

	hybridswap_limit_inflight(req);

	ret = hybridswap_submit_bio(segment);
	if (unlikely(ret)) {
		log_warn("submit bio failed, ret %d\n", ret);
		hybridswap_segment_free(req, segment);
	}
	req->segment = NULL;

	return ret;
}

static bool hybridswap_check_io_para_err(struct hybridswap_io *io_para)
{
	if (unlikely(!io_para)) {
		log_err("io_para null\n");

		return true;
	}

	if (unlikely(!io_para->bdev ||
		     (io_para->scene >= SCENE_MAX))) {
		log_err("io_para err, scene %u\n",
			io_para->scene);

		return true;
	}

	if (unlikely(!io_para->done_callback)) {
		log_err("done_callback err\n");

		return true;
	}

	return false;
}

static bool hybridswap_check_entry_err(struct hybridswap_entry *io_entry)
{
	int i;

	if (unlikely(!io_entry)) {
		log_err("io_entry null\n");

		return true;
	}

	if (unlikely((!io_entry->dest_pages) ||
		     (io_entry->ext_id < 0) ||
		     (io_entry->pages_sz > BIO_MAX_VECS) ||
		     (io_entry->pages_sz <= 0))) {
		log_err("ext_id %d, page_sz %d\n", io_entry->ext_id,
			io_entry->pages_sz);

		return true;
	}

	for (i = 0; i < io_entry->pages_sz; ++i) {
		if (!io_entry->dest_pages[i]) {
			log_err("dest_pages[%d] is null\n", i);
			return true;
		}
	}

	return false;
}

static int hybridswap_io_extent(void *io_handler,
				struct hybridswap_entry *io_entry)
{
	int ret;
	struct hybridswap_io_req *req = (struct hybridswap_io_req *)io_handler;

	if (unlikely(hybridswap_check_entry_err(io_entry))) {
		hybridswap_io_err_record(SCENE_FAULT_OUT_IO_ENTRY_PARA_FAIL,
					 req, io_entry ? io_entry->ext_id : -EINVAL);
		req->io_para.done_callback(io_entry, -EIO, req);

		return -EFAULT;
	}

	log_dbg("ext id %d, pages_sz %d, addr %llx\n",
		io_entry->ext_id, io_entry->pages_sz,
		io_entry->addr);

	if (hybridswap_ext_merge(req, io_entry))
		return hybridswap_io_submit(req, true);

	ret = hybridswap_io_submit(req, false);
	if (unlikely(ret)) {
		log_err("submit fail %d\n", ret);
		req->io_para.done_callback(io_entry, -EIO, req);

		return ret;
	}

	ret = hybridswap_new_segment_init(req, io_entry);
	if (unlikely(ret)) {
		log_err("new_segment_init fail %d\n", ret);
		req->io_para.done_callback(io_entry, -EIO, req);

		return ret;
	}

	return 0;
}

int hybridswap_schedule_init(void)
{
	if (hybridswap_schedule_init_flag)
		return 0;

	hybridswap_proc_read_workqueue = alloc_workqueue("proc_hybridswap_read",
							 WQ_HIGHPRI | WQ_UNBOUND, 0);
	if (unlikely(!hybridswap_proc_read_workqueue))
		return -EFAULT;

	hybridswap_proc_write_workqueue = alloc_workqueue("proc_hybridswap_write",
							  WQ_CPU_INTENSIVE, 0);
	if (unlikely(!hybridswap_proc_write_workqueue)) {
		destroy_workqueue(hybridswap_proc_read_workqueue);

		return -EFAULT;
	}

	hybridswap_key_init();

	hybridswap_schedule_init_flag = true;

	return 0;
}

void *hybridswap_plug_start(struct hybridswap_io *io_para)
{
	gfp_t gfp;
	struct hybridswap_io_req *req = NULL;

	if (unlikely(hybridswap_check_io_para_err(io_para)))
		return NULL;

	gfp = (io_para->scene != SCENE_RECLAIM_IN) ?
		GFP_ATOMIC : GFP_NOIO;
	req = kzalloc(sizeof(struct hybridswap_io_req), gfp);
	if (!req && (io_para->scene == SCENE_FAULT_OUT))
		req = kzalloc(sizeof(struct hybridswap_io_req), GFP_NOIO);

	if (unlikely(!req)) {
		log_err("io_req null\n");

		return NULL;
	}

	kref_init(&req->refcount);
	mutex_init(&req->refmutex);
	atomic_set(&req->extent_inflight, 0);
	init_waitqueue_head(&req->io_wait);
	req->io_para.bdev = io_para->bdev;
	req->io_para.scene = io_para->scene;
	req->io_para.done_callback = io_para->done_callback;
	req->io_para.complete_notify = io_para->complete_notify;
	req->io_para.private = io_para->private;
	req->io_para.record = io_para->record;
	req->limit_inflight_flag =
		(io_para->scene == SCENE_RECLAIM_IN) ||
		(io_para->scene == SCENE_PRE_OUT);
	req->wait_io_finish_flag =
		(io_para->scene == SCENE_RECLAIM_IN) ||
		(io_para->scene == SCENE_FAULT_OUT);
	req->nice = task_nice(current);
	init_completion(&req->io_end_flag);

	return (void *)req;
}

int hybridswap_read_extent(void *io_handler, struct hybridswap_entry *io_entry)
{
	return hybridswap_io_extent(io_handler, io_entry);
}

int hybridswap_write_extent(void *io_handler, struct hybridswap_entry *io_entry)
{
	return hybridswap_io_extent(io_handler, io_entry);
}

int hybridswap_plug_finish(void *io_handler)
{
	int ret;
	struct hybridswap_io_req *req = (struct hybridswap_io_req *)io_handler;

	perf_latency_begin(req->io_para.record, STAGE_IO_EXTENT);
	ret = hybridswap_io_submit(req, false);
	if (unlikely(ret))
		log_err("submit fail %d\n", ret);

	perf_latency_end(req->io_para.record, STAGE_IO_EXTENT);
	hybridswap_wait_io_finish(req);
	hybridswap_perf_lat_point(req->io_para.record, STAGE_WAKE_UP);

	hybridswap_stat_io_bytes(req);
	perf_stat_io(req->io_para.record, req->page_cnt,
		     req->segment_cnt);

	kref_put_mutex(&req->refcount, hybridswap_io_req_release,
		       &req->refmutex);

	log_dbg("io schedule finish succ\n");

	return ret;
}


struct async_req {
	struct mem_cgroup *mcg;
	unsigned long size;
	unsigned long out_size;
	unsigned long reclaimined_sz;
	struct work_struct work;
	int nice;
	bool preload;
};

struct io_priv {
	struct zram *zram;
	enum hybridswap_scene scene;
	struct hybridswap_page_pool page_pool;
};

struct schedule_para {
	void *io_handler;
	struct hybridswap_entry *io_entry;
	struct hybridswap_buffer io_buf;
	struct io_priv priv;
	struct hybridswap_record_stage record;
};

#define MIN_RECLAIM_ZRAM_SZ	(1024 * 1024)

static void hybridswap_memcg_iter(int (*iter)(struct mem_cgroup *, void *), void *data)
{
	struct mem_cgroup *mcg = get_next_memcg(NULL);
	int ret;

	while (mcg) {
		ret = iter(mcg, data);
		log_dbg("%pS mcg %d %s %s, ret %d\n",
			iter, mcg->id.id,
			MEMCGRP_ITEM(mcg, name),
			ret ? "failed" : "pass",
			ret);
		if (ret) {
			get_next_memcg_break(mcg);
			return;
		}
		mcg = get_next_memcg(mcg);
	}
}

void hybridswap_track(struct zram *zram, u32 index,
		      struct mem_cgroup *memcg)
{
	memcg_hybs_t *hybs;
	struct hybridswap_stat *stat;

	if (!hybridswap_core_enabled())
		return;

	if (!memcg || !memcg->id.id) {
		stat = hybridswap_get_stat_obj();
		if (stat)
			atomic64_inc(&stat->null_memcg_skip_track_cnt);
		return;
	}

	hybs = MEMCGRP_ITEM_DATA(memcg);
	if (!hybs) {
		hybs = hybridswap_cache_alloc(memcg, false);
		if (!hybs) {
			stat = hybridswap_get_stat_obj();
			if (stat)
				atomic64_inc(&stat->skip_track_cnt);
			return;
		}
	}

	if (unlikely(!hybs->zram)) {
		spin_lock(&hybs->zram_init_lock);
		if (!hybs->zram)
			hybridswap_memcg_init(zram, memcg);
		spin_unlock(&hybs->zram_init_lock);
	}

	hybridswap_zram_lru_add(zram, index, memcg);

#ifdef CONFIG_HYBRIDSWAP_SWAPD
	zram_slot_unlock(zram, index);
	if (!hybridswapd_ops->zram_watermark_ok())
		hybridswapd_ops->wakeup_kthreads();
	zram_slot_lock(zram, index);
#endif
}

void hybridswap_untrack(struct zram *zram, u32 index)
{
	if (!hybridswap_core_enabled())
		return;

	while (zram_test_flag(zram, index, ZRAM_UNDER_WB) ||
	       zram_test_flag(zram, index, ZRAM_BATCHING_OUT)) {
		zram_slot_unlock(zram, index);
		udelay(50);
		zram_slot_lock(zram, index);
	}

	hybridswap_zram_lru_del(zram, index);
}

static unsigned long memcg_reclaim_size(struct mem_cgroup *memcg)
{
	memcg_hybs_t *hybs = MEMCGRP_ITEM_DATA(memcg);
	unsigned long zram_size, cur_size, new_size;

	if (!hybs)
		return 0;

	zram_size = atomic64_read(&hybs->zram_stored_size);
	if (hybs->force_swapout) {
		hybs->can_eswaped = zram_size;
		return zram_size;
	}

	cur_size = atomic64_read(&hybs->hybridswap_stored_size);
	new_size = (zram_size + cur_size) *
		atomic_read(&hybs->ub_zram2ufs_ratio) / 100;

	hybs->can_eswaped = (new_size > cur_size) ? (new_size - cur_size) : 0;
	return hybs->can_eswaped;
}

static int hybridswap_permcg_sz(struct mem_cgroup *memcg, void *data)
{
	unsigned long *out_size = (unsigned long *)data;

	*out_size += memcg_reclaim_size(memcg);
	return 0;
}

static void hybridswap_flush_cb(enum hybridswap_scene scene,
				void *pri, struct hybridswap_io_req *req)
{
	switch (scene) {
	case SCENE_FAULT_OUT:
	case SCENE_PRE_OUT:
	case SCENE_BATCH_OUT:
		hybridswap_extent_destroy(pri, scene);
		break;
	case SCENE_RECLAIM_IN:
		hybridswap_extent_register(pri, req);
		break;
	default:
		break;
	}
}

static void hybridswap_flush_done(struct hybridswap_entry *io_entry,
				  int err, struct hybridswap_io_req *req)
{
	struct io_priv *priv;

	if (unlikely(!io_entry))
		return;

	priv = (struct io_priv *)(io_entry->private);
	if (likely(!err)) {
		hybridswap_flush_cb(priv->scene,
				    io_entry->manager_private, req);
#ifdef CONFIG_HYBRIDSWAP_SWAPD
		if (!hybridswapd_ops->zram_watermark_ok())
			hybridswapd_ops->wakeup_kthreads();
#endif
	} else {
		hybridswap_extent_exception(priv->scene,
					    io_entry->manager_private);
	}
	hybridswap_free(io_entry);
}

static void hybridswap_free_pagepool(struct schedule_para *sched)
{
	struct page *free_page = NULL;

	spin_lock(&sched->priv.page_pool.page_pool_lock);
	while (!list_empty(&sched->priv.page_pool.page_pool_list)) {
		free_page = list_first_entry(
					     &sched->priv.page_pool.page_pool_list,
					     struct page, lru);
		list_del_init(&free_page->lru);
		__free_page(free_page);
	}
	spin_unlock(&sched->priv.page_pool.page_pool_lock);
}

static void hybridswap_plug_complete(void *data)
{
	struct schedule_para *sched  = (struct schedule_para *)data;

	hybridswap_free_pagepool(sched);

	perf_end(&sched->record);

	hybridswap_free(sched);
}

static void *hybridswap_init_plug(struct zram *zram,
				  enum hybridswap_scene scene,
				  struct schedule_para *sched)
{
	struct hybridswap_io io_para;

	io_para.bdev = zram->bdev;
	io_para.scene = scene;
	io_para.private = (void *)sched;
	io_para.record = &sched->record;
	INIT_LIST_HEAD(&sched->priv.page_pool.page_pool_list);
	spin_lock_init(&sched->priv.page_pool.page_pool_lock);
	io_para.done_callback = hybridswap_flush_done;
	switch (io_para.scene) {
	case SCENE_RECLAIM_IN:
		io_para.complete_notify = hybridswap_plug_complete;
		sched->io_buf.pool = NULL;
		break;
	case SCENE_BATCH_OUT:
	case SCENE_PRE_OUT:
		io_para.complete_notify = hybridswap_plug_complete;
		sched->io_buf.pool = &sched->priv.page_pool;
		break;
	case SCENE_FAULT_OUT:
		io_para.complete_notify = NULL;
		sched->io_buf.pool = NULL;
		break;
	default:
		break;
	}
	sched->io_buf.zram = zram;
	sched->priv.zram = zram;
	sched->priv.scene = io_para.scene;
	return hybridswap_plug_start(&io_para);
}

static void hybridswap_fill_entry(struct hybridswap_entry *io_entry,
				  struct hybridswap_buffer *io_buf,
				  void *private)
{
	io_entry->addr = io_entry->ext_id * EXTENT_SECTOR_SIZE;
	io_entry->dest_pages = io_buf->dest_pages;
	io_entry->pages_sz = EXTENT_PG_CNT;
	io_entry->private = private;
}

static int hybridswap_reclaim_check(struct mem_cgroup *memcg,
				    unsigned long *require_size)
{
	memcg_hybs_t *hybs = MEMCGRP_ITEM_DATA(memcg);

	if (unlikely(!hybs) || unlikely(!hybs->zram))
		return -EAGAIN;
	if (unlikely(hybs->in_swapin))
		return -EAGAIN;
	if (!hybs->force_swapout && *require_size < MIN_RECLAIM_ZRAM_SZ)
		return -EAGAIN;

	return 0;
}

static int hybridswap_update_reclaim_sz(unsigned long *require_size,
					unsigned long *mcg_reclaimed_sz,
					unsigned long reclaim_size)
{
	*mcg_reclaimed_sz += reclaim_size;

	if (*require_size > reclaim_size)
		*require_size -= reclaim_size;
	else
		*require_size = 0;

	return 0;
}

static void hybridswap_stat_alloc_fail(enum hybridswap_scene scene,
				       int err)
{
	struct hybridswap_stat *stat = hybridswap_get_stat_obj();

	if (!stat || (err != -ENOMEM) || (scene >= SCENE_MAX))
		return;

	atomic64_inc(&stat->alloc_fail_cnt[scene]);
}

static int hybridswap_reclaim_extent(struct mem_cgroup *memcg,
				     struct schedule_para *sched,
				     unsigned long *require_size,
				     unsigned long *mcg_reclaimed_sz,
				     int *io_err)
{
	int ret;
	unsigned long reclaim_size;

	perf_latency_begin(&sched->record, STAGE_IOENTRY_ALLOC);
	sched->io_entry = hybridswap_malloc(
					    sizeof(struct hybridswap_entry), false, true);
	perf_latency_end(&sched->record, STAGE_IOENTRY_ALLOC);
	if (unlikely(!sched->io_entry)) {
		log_err("alloc io entry failed!\n");
		*require_size = 0;
		*io_err = -ENOMEM;
		hybridswap_stat_alloc_fail(SCENE_RECLAIM_IN, -ENOMEM);

		return *io_err;
	}

	perf_latency_begin(&sched->record, STAGE_FIND_EXTENT);
	reclaim_size = hybridswap_extent_create(memcg, &sched->io_entry->ext_id,
						&sched->io_buf, &sched->io_entry->manager_private);
	perf_latency_end(&sched->record, STAGE_FIND_EXTENT);
	if (unlikely(!reclaim_size)) {
		if (sched->io_entry->ext_id != -ENOENT)
			*require_size = 0;
		hybridswap_free(sched->io_entry);
		return -EAGAIN;
	}

	hybridswap_fill_entry(sched->io_entry, &sched->io_buf,
			      (void *)(&sched->priv));

	perf_latency_begin(&sched->record, STAGE_IO_EXTENT);
	ret = hybridswap_write_extent(sched->io_handler, sched->io_entry);
	perf_latency_end(&sched->record, STAGE_IO_EXTENT);
	if (unlikely(ret)) {
		log_err("hybridswap write failed! %d\n", ret);
		*require_size = 0;
		*io_err = ret;
		hybridswap_stat_alloc_fail(SCENE_RECLAIM_IN, ret);

		return *io_err;
	}

	ret = hybridswap_update_reclaim_sz(require_size, mcg_reclaimed_sz,
					   reclaim_size);
	if (MEMCGRP_ITEM(memcg, force_swapout))
		return 0;
	return ret;
}

static int hybridswap_permcg_reclaim(struct mem_cgroup *memcg,
				     unsigned long require_size, unsigned long *mcg_reclaimed_sz)
{
	int ret, extcnt;
	int io_err = 0;
	unsigned long require_size_before = 0;
	struct schedule_para *sched = NULL;
	ktime_t start = ktime_get();
	unsigned long long start_ravg_sum = hybridswap_get_ravg_sum();
	memcg_hybs_t *hybs = MEMCGRP_ITEM_DATA(memcg);

	ret = hybridswap_reclaim_check(memcg, &require_size);
	if (ret)
		return ret == -EAGAIN ? 0 : ret;

	sched = hybridswap_malloc(sizeof(struct schedule_para), false, true);
	if (unlikely(!sched)) {
		log_err("alloc sched failed!\n");
		hybridswap_stat_alloc_fail(SCENE_RECLAIM_IN, -ENOMEM);

		return -ENOMEM;
	}

	perf_begin(&sched->record, start, start_ravg_sum, SCENE_RECLAIM_IN);
	perf_latency_begin(&sched->record, STAGE_INIT);
	sched->io_handler = hybridswap_init_plug(hybs->zram,
						 SCENE_RECLAIM_IN, sched);
	perf_latency_end(&sched->record, STAGE_INIT);
	if (unlikely(!sched->io_handler)) {
		log_err("plug start failed!\n");
		perf_end(&sched->record);
		hybridswap_free(sched);
		hybridswap_stat_alloc_fail(SCENE_RECLAIM_IN, -ENOMEM);
		ret = -EIO;
		goto out;
	}

	require_size_before = require_size;
	while (require_size) {
		if (hybridswap_reclaim_extent(memcg, sched, &require_size,
					      mcg_reclaimed_sz, &io_err))
			break;

		atomic64_inc(&hybs->hybridswap_outextcnt);
		extcnt = atomic_inc_return(&hybs->hybridswap_extcnt);
		if (extcnt > atomic_read(&hybs->hybridswap_peakextcnt))
			atomic_set(&hybs->hybridswap_peakextcnt, extcnt);
	}

	ret = hybridswap_plug_finish(sched->io_handler);
	if (unlikely(ret)) {
		log_err("hybridswap write flush failed! %d\n", ret);
		hybridswap_stat_alloc_fail(SCENE_RECLAIM_IN, ret);
		require_size = 0;
	} else {
		ret = io_err;
	}
	atomic64_inc(&hybs->hybridswap_outcnt);

out:
	log_info("memcg %s %lu %lu reclaim_in %lu KB eswap %lld zram %lld %d\n",
		 hybs->name, require_size_before, require_size,
		 (require_size_before - require_size) >> 10,
		 atomic64_read(&hybs->hybridswap_stored_size),
		 atomic64_read(&hybs->zram_stored_size), ret);
	return ret;
}

static void hybridswap_reclaimin_inc(void)
{
	struct hybridswap_stat *stat;

	stat = hybridswap_get_stat_obj();
	if (unlikely(!stat))
		return;
	atomic64_inc(&stat->reclaimin_infight);
}

static void hybridswap_reclaimin_dec(void)
{
	struct hybridswap_stat *stat;

	stat = hybridswap_get_stat_obj();
	if (unlikely(!stat))
		return;
	atomic64_dec(&stat->reclaimin_infight);
}

static int hybridswap_permcg_reclaimin(struct mem_cgroup *memcg,
				       void *data)
{
	struct async_req *rq = (struct async_req *)data;
	unsigned long mcg_reclaimed_size = 0, require_size;
	int ret;
	memcg_hybs_t *hybs;

	hybs = MEMCGRP_ITEM_DATA(memcg);
	if (!hybs)
		return 0;

	require_size = hybs->can_eswaped * rq->size / rq->out_size;
	if (require_size < MIN_RECLAIM_ZRAM_SZ)
		return 0;

	if (!mutex_trylock(&hybs->swap_lock))
		return 0;

	ret = hybridswap_permcg_reclaim(memcg, require_size,
					&mcg_reclaimed_size);
	rq->reclaimined_sz += mcg_reclaimed_size;
	mutex_unlock(&hybs->swap_lock);

	log_info("memcg %s mcg_reclaimed_size %lu rq->reclaimined_sz %lu rq->size %lu rq->out_size %lu ret %d\n",
		 hybs->name, mcg_reclaimed_size, rq->reclaimined_sz,
		 rq->size, rq->out_size, ret);

	if (!ret && rq->reclaimined_sz >= rq->size)
		return -EINVAL;

	return ret;
}

static void hybridswap_reclaim_work(struct work_struct *work)
{
	struct async_req *rq = container_of(work, struct async_req, work);
	int old_nice = task_nice(current);

	set_user_nice(current, rq->nice);
	hybridswap_reclaimin_inc();
	hybridswap_memcg_iter(hybridswap_permcg_reclaimin, rq);
	hybridswap_reclaimin_dec();
	set_user_nice(current, old_nice);
	log_info("SWAPOUT want %lu MB real %lu Mb\n", rq->size >> 20,
		 rq->reclaimined_sz >> 20);
	hybridswap_free(rq);
}

unsigned long hybridswap_reclaim_in(unsigned long size)
{
	struct async_req *rq = NULL;
	unsigned long out_size = 0;

	if (!hybridswap_core_enabled() || !hybridswap_reclaim_in_enable()
	    || hybridswap_reach_life_protect() || !size)
		return 0;

	hybridswap_memcg_iter(hybridswap_permcg_sz, &out_size);
	if (!out_size)
		return 0;

	rq = hybridswap_malloc(sizeof(struct async_req), false, true);
	if (unlikely(!rq)) {
		log_err("alloc async req fail!\n");
		hybridswap_stat_alloc_fail(SCENE_RECLAIM_IN, -ENOMEM);
		return 0;
	}

	if (out_size < size)
		size = out_size;
	rq->size = size;
	rq->out_size = out_size;
	rq->reclaimined_sz = 0;
	rq->nice = task_nice(current);
	INIT_WORK(&rq->work, hybridswap_reclaim_work);
	queue_work(hybridswap_get_reclaim_workqueue(), &rq->work);

	return out_size > size ? size : out_size;
}

static int hybridswap_batch_out_extent(struct schedule_para *sched,
				       struct mem_cgroup *mcg,
				       bool preload,
				       int *io_err)
{
	int ret;

	perf_latency_begin(&sched->record, STAGE_IOENTRY_ALLOC);
	sched->io_entry = hybridswap_malloc(sizeof(struct hybridswap_entry), !preload, preload);
	perf_latency_end(&sched->record, STAGE_IOENTRY_ALLOC);
	if (unlikely(!sched->io_entry)) {
		log_err("alloc io entry failed!\n");
		*io_err = -ENOMEM;
		hybridswap_stat_alloc_fail(SCENE_BATCH_OUT, -ENOMEM);

		return *io_err;
	}

	perf_latency_begin(&sched->record, STAGE_FIND_EXTENT);
	sched->io_entry->ext_id = hybridswap_find_extent_by_memcg(mcg, &sched->io_buf,
								  &sched->io_entry->manager_private);
	perf_latency_end(&sched->record, STAGE_FIND_EXTENT);
	if (sched->io_entry->ext_id < 0) {
		hybridswap_stat_alloc_fail(SCENE_BATCH_OUT,
					   sched->io_entry->ext_id);
		hybridswap_free(sched->io_entry);
		return -EAGAIN;
	}

	hybridswap_fill_entry(sched->io_entry, &sched->io_buf,
			      (void *)(&sched->priv));

	perf_latency_begin(&sched->record, STAGE_IO_EXTENT);
	ret = hybridswap_read_extent(sched->io_handler, sched->io_entry);
	perf_latency_end(&sched->record, STAGE_IO_EXTENT);
	if (unlikely(ret)) {
		log_err("hybridswap read failed! %d\n", ret);
		hybridswap_stat_alloc_fail(SCENE_BATCH_OUT, ret);
		*io_err = ret;

		return *io_err;
	}

	return 0;
}

static int hybridswap_do_batch_out_init(struct schedule_para **out_sched,
					struct mem_cgroup *mcg, bool preload)
{
	struct schedule_para *sched = NULL;
	ktime_t start = ktime_get();
	unsigned long long start_ravg_sum = hybridswap_get_ravg_sum();

	sched = hybridswap_malloc(sizeof(struct schedule_para),
				  !preload, preload);
	if (unlikely(!sched)) {
		log_err("alloc sched failed!\n");
		hybridswap_stat_alloc_fail(SCENE_BATCH_OUT, -ENOMEM);

		return -ENOMEM;
	}

	perf_begin(&sched->record, start, start_ravg_sum,
		   preload ? SCENE_PRE_OUT : SCENE_BATCH_OUT);

	perf_latency_begin(&sched->record, STAGE_INIT);
	sched->io_handler = hybridswap_init_plug(MEMCGRP_ITEM(mcg, zram),
						 preload ? SCENE_PRE_OUT : SCENE_BATCH_OUT,
						 sched);
	perf_latency_end(&sched->record, STAGE_INIT);
	if (unlikely(!sched->io_handler)) {
		log_err("plug start failed!\n");
		perf_end(&sched->record);
		hybridswap_free(sched);
		hybridswap_stat_alloc_fail(SCENE_BATCH_OUT, -ENOMEM);

		return -EIO;
	}

	*out_sched = sched;

	return 0;
}

static int hybridswap_do_batch_out(struct mem_cgroup *mcg,
				   unsigned long size, bool preload)
{
	int ret = 0;
	int io_err = 0;
	struct schedule_para *sched = NULL;

	if (unlikely(!mcg || !MEMCGRP_ITEM(mcg, zram))) {
		log_warn("no zram in mcg!\n");
		ret = -EINVAL;
		goto out;
	}

	ret = hybridswap_do_batch_out_init(&sched, mcg, preload);
	if (unlikely(ret))
		goto out;

	MEMCGRP_ITEM(mcg, in_swapin) = true;
	while (size) {
		if (hybridswap_batch_out_extent(sched, mcg, preload, &io_err))
			break;
		size -= EXTENT_SIZE;
	}

	ret = hybridswap_plug_finish(sched->io_handler);
	if (unlikely(ret)) {
		log_err("hybridswap read flush failed! %d\n", ret);
		hybridswap_stat_alloc_fail(SCENE_BATCH_OUT, ret);
	} else {
		ret = io_err;
	}

	if (atomic64_read(&MEMCGRP_ITEM(mcg, hybridswap_stored_size)) &&
	    hybridswap_loglevel() >= HS_LOG_INFO)
		hybridswap_check_extent((MEMCGRP_ITEM(mcg, zram)->hs_swap));

	atomic64_inc(&MEMCGRP_ITEM(mcg, hybridswap_incnt));
	MEMCGRP_ITEM(mcg, in_swapin) = false;
out:
	return ret;
}

static void hybridswap_batchout_inc(void)
{
	struct hybridswap_stat *stat;

	stat = hybridswap_get_stat_obj();
	if (unlikely(!stat))
		return;
	atomic64_inc(&stat->batchout_inflight);
}

static void hybridswap_batchout_dec(void)
{
	struct hybridswap_stat *stat;

	stat = hybridswap_get_stat_obj();
	if (unlikely(!stat))
		return;
	atomic64_dec(&stat->batchout_inflight);
}

int hybridswap_batch_out(struct mem_cgroup *mcg,
			 unsigned long size, bool preload)
{
	int ret;

	if (!hybridswap_core_enabled())
		return 0;

	hybridswap_batchout_inc();
	ret = hybridswap_do_batch_out(mcg, size, preload);
	hybridswap_batchout_dec();

	return ret;
}

static void hybridswap_fault_stat(struct zram *zram, u32 index)
{
	struct mem_cgroup *mcg = NULL;
	struct hybridswap_stat *stat = hybridswap_get_stat_obj();

	if (unlikely(!stat))
		return;

	atomic64_inc(&stat->fault_cnt);

	mcg = hybridswap_zram_get_memcg(zram, index);
	if (mcg)
		atomic64_inc(&MEMCGRP_ITEM(mcg, hybridswap_allfaultcnt));
}

static void hybridswap_fault2_stat(struct zram *zram, u32 index)
{
	struct mem_cgroup *mcg = NULL;
	struct hybridswap_stat *stat = hybridswap_get_stat_obj();

	if (unlikely(!stat))
		return;

	atomic64_inc(&stat->hybridswap_fault_cnt);

	mcg = hybridswap_zram_get_memcg(zram, index);
	if (mcg)
		atomic64_inc(&MEMCGRP_ITEM(mcg, hybridswap_faultcnt));
}

static bool hybridswap_fault_out_check(struct zram *zram,
				       u32 index, unsigned long *zentry)
{
	if (!hybridswap_core_enabled())
		return false;

	hybridswap_fault_stat(zram, index);

	if (!zram_test_flag(zram, index, ZRAM_WB))
		return false;

	zram_set_flag(zram, index, ZRAM_BATCHING_OUT);
	*zentry = zram_get_handle(zram, index);
	zram_slot_unlock(zram, index);
	return true;
}

static int hybridswap_fault_out_get_extent(struct zram *zram,
					   struct schedule_para *sched,
					   unsigned long zentry,
					   u32 index)
{
	int wait_cycle = 0;

	sched->io_buf.zram = zram;
	sched->priv.zram = zram;
	sched->io_buf.pool = NULL;
	perf_latency_begin(&sched->record, STAGE_FIND_EXTENT);
	sched->io_entry->ext_id = hybridswap_find_extent_by_idx(zentry,
								&sched->io_buf, &sched->io_entry->manager_private);
	perf_latency_end(&sched->record, STAGE_FIND_EXTENT);
	if (unlikely(sched->io_entry->ext_id == -EBUSY)) {
		while (1) {
			zram_slot_lock(zram, index);
			if (!zram_test_flag(zram, index, ZRAM_WB)) {
				zram_slot_unlock(zram, index);
				hybridswap_free(sched->io_entry);
#ifdef CONFIG_HYBRIDSWAP_SWAPD
				if (wait_cycle >= 1000)
					atomic_long_dec(hybridswapd_ops->fault_out_pause);
#endif
				return -EAGAIN;
			}
			zram_slot_unlock(zram, index);

			perf_latency_begin(&sched->record,
					   STAGE_FIND_EXTENT);
			sched->io_entry->ext_id =
				hybridswap_find_extent_by_idx(zentry,
							      &sched->io_buf,
							      &sched->io_entry->manager_private);
			perf_latency_end(&sched->record,
					 STAGE_FIND_EXTENT);
			if (likely(sched->io_entry->ext_id != -EBUSY))
				break;

			if (wait_cycle < 100)
				udelay(50);
			else
				usleep_range(50, 100);
			wait_cycle++;
#ifdef CONFIG_HYBRIDSWAP_SWAPD
			if (wait_cycle == 1000) {
				atomic_long_inc(hybridswapd_ops->fault_out_pause);
				atomic_long_inc(hybridswapd_ops->fault_out_pause_cnt);
			}
#endif
		}
	}
#ifdef CONFIG_HYBRIDSWAP_SWAPD
	if (wait_cycle >= 1000)
		atomic_long_dec(hybridswapd_ops->fault_out_pause);
#endif
	if (sched->io_entry->ext_id < 0) {
		hybridswap_stat_alloc_fail(SCENE_FAULT_OUT,
					   sched->io_entry->ext_id);

		return sched->io_entry->ext_id;
	}
	hybridswap_fault2_stat(zram, index);
	hybridswap_fill_entry(sched->io_entry, &sched->io_buf,
			      (void *)(&sched->priv));
	return 0;
}

static int hybridswap_fault_out_exit_check(struct zram *zram,
					   u32 index, int ret)
{
	zram_slot_lock(zram, index);
	if (likely(!ret)) {
		if (unlikely(zram_test_flag(zram, index, ZRAM_WB))) {
			log_err("still in WB status!\n");
			ret = -EIO;
		}
	}
	zram_clear_flag(zram, index, ZRAM_BATCHING_OUT);

	return ret;
}

static int hybridswap_fault_out_extent(struct zram *zram, u32 index,
				       struct schedule_para *sched, unsigned long zentry)
{
	int ret;

	perf_latency_begin(&sched->record, STAGE_IOENTRY_ALLOC);
	sched->io_entry = hybridswap_malloc(sizeof(struct hybridswap_entry),
					    true, true);
	perf_latency_end(&sched->record, STAGE_IOENTRY_ALLOC);
	if (unlikely(!sched->io_entry)) {
		log_err("alloc io entry failed!\n");
		hybridswap_stat_alloc_fail(SCENE_FAULT_OUT, -ENOMEM);
		hybridswap_error_record(SCENE_FAULT_OUT_ENTRY_ALLOC_FAIL,
					index, 0, sched->record.task_comm);
		return -ENOMEM;
	}

	ret = hybridswap_fault_out_get_extent(zram, sched, zentry, index);
	if (ret)
		return ret;

	perf_latency_begin(&sched->record, STAGE_IO_EXTENT);
	ret = hybridswap_read_extent(sched->io_handler, sched->io_entry);
	perf_latency_end(&sched->record, STAGE_IO_EXTENT);
	if (unlikely(ret)) {
		log_err("hybridswap read failed! %d\n", ret);
		hybridswap_stat_alloc_fail(SCENE_FAULT_OUT, ret);
	}

	return ret;
}

int hybridswap_fault_out(struct zram *zram, u32 index)
{
	int ret = 0;
	int io_err;
	struct schedule_para *psched;
	unsigned long zentry;
	ktime_t start = ktime_get();
	unsigned long long start_ravg_sum = hybridswap_get_ravg_sum();

	if (!hybridswap_fault_out_check(zram, index, &zentry))
		return ret;

	psched = kmalloc(sizeof(struct schedule_para), GFP_NOIO | __GFP_NOFAIL);
	memset(&psched->record, 0, sizeof(struct hybridswap_record_stage));
	perf_begin(&psched->record, start, start_ravg_sum, SCENE_FAULT_OUT);

	perf_latency_begin(&psched->record, STAGE_INIT);
	psched->io_handler = hybridswap_init_plug(zram, SCENE_FAULT_OUT, psched);
	perf_latency_end(&psched->record, STAGE_INIT);
	if (unlikely(!psched->io_handler)) {
		log_err("plug start failed!\n");
		hybridswap_stat_alloc_fail(SCENE_FAULT_OUT, -ENOMEM);
		ret = -EIO;
		hybridswap_error_record(SCENE_FAULT_OUT_INIT_FAIL,
					index, 0, psched->record.task_comm);

		goto out;
	}

	io_err = hybridswap_fault_out_extent(zram, index, psched, zentry);
	ret = hybridswap_plug_finish(psched->io_handler);
	if (unlikely(ret)) {
		log_err("hybridswap flush failed! %d\n", ret);
		hybridswap_stat_alloc_fail(SCENE_FAULT_OUT, ret);
	} else {
		ret = (io_err != -EAGAIN) ? io_err : 0;
	}
out:
	perf_latency_begin(&psched->record, STAGE_ZRAM_LOCK);
	ret = hybridswap_fault_out_exit_check(zram, index, ret);
	perf_latency_end(&psched->record, STAGE_ZRAM_LOCK);
	perf_end(&psched->record);
	kfree(psched);
	return ret;
}

bool hybridswap_delete(struct zram *zram, u32 index)
{
	if (!hybridswap_core_enabled())
		return true;

	if (zram_test_flag(zram, index, ZRAM_UNDER_WB)
	    || zram_test_flag(zram, index, ZRAM_BATCHING_OUT)) {
		struct hybridswap_stat *stat = hybridswap_get_stat_obj();

		if (stat)
			atomic64_inc(&stat->miss_free);
		return false;
	}

	if (!zram_test_flag(zram, index, ZRAM_WB))
		return true;

	hybridswap_extent_objs_del(zram, index);

	return true;
}

void hybridswap_mem_cgroup_deinit(struct mem_cgroup *memcg)
{
	if (!hybridswap_core_enabled())
		return;

	hybridswap_memcg_deinit(memcg);
}

void hybridswap_force_reclaim(struct mem_cgroup *mcg)
{
	unsigned long mcg_reclaimed_size = 0, require_size;
	memcg_hybs_t *hybs;

	if (!hybridswap_core_enabled() || !hybridswap_reclaim_in_enable()
	    || hybridswap_reach_life_protect())
		return;

	if (!mcg)
		return;

	hybs = MEMCGRP_ITEM_DATA(mcg);
	if (!hybs || !hybs->zram)
		return;

	mutex_lock(&hybs->swap_lock);
	require_size = atomic64_read(&hybs->zram_stored_size);
	hybs->force_swapout = true;
	hybridswap_permcg_reclaim(mcg, require_size, &mcg_reclaimed_size);
	hybs->force_swapout = false;
	mutex_unlock(&hybs->swap_lock);
}

void mem_cgroup_id_remove_hook(void *data, struct mem_cgroup *memcg)
{
	if (!memcg->android_oem_data1[0])
		return;

	hybridswap_mem_cgroup_deinit(memcg);
	log_dbg("hybridswap remove mcg id = %d\n", memcg->id.id);
}
