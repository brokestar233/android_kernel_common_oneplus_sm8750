/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _ZRAM_WRITEBACK_H_
#define _ZRAM_WRITEBACK_H_

#include <linux/bio.h>
#include "zram_drv.h"

struct zram_wb_request {
	struct zram *zram;
	unsigned long blk_idx;
	struct zram_pp_slot *pps;
	struct zram_pp_ctl *ppctl;
	struct bio *bio;
	struct list_head node;
};

struct zram_wb_request_list {
	struct list_head head;
	int count;
	spinlock_t lock;
};

/* GC constants */
#define ZRAM_GC_MAX_BATCH		16
#define ZRAM_GC_PERIODIC_INTERVAL	(15 * HZ)
#define ZRAM_GC_NORMAL_MAX_PAGES	4
#define ZRAM_GC_AGGRESSIVE_MAX_PAGES	16
#define ZRAM_GC_BUDGET_MS		20
#define ZRAM_GC_MAX_SCAN_ENTRIES	8192

enum zram_gc_mode {
	ZRAM_GC_MODE_NORMAL = 0,
	ZRAM_GC_MODE_AGGRESSIVE,
};

#if IS_ENABLED(CONFIG_ZRAM_WRITEBACK)
unsigned long alloc_block_bdev(struct zram *zram);
unsigned long alloc_block_bdev_batch(struct zram *zram, int req_count, int *act_count);
void free_block_bdev(struct zram *zram, unsigned long blk_idx);
void free_block_bdev_range(struct zram *zram, unsigned long blk_idx, int count);
int setup_zram_writeback(void);
void destroy_zram_writeback(void);
struct zram_wb_request *alloc_wb_request(struct zram *zram,
					 struct zram_pp_slot *pps,
					 struct zram_pp_ctl *ppctl,
					 unsigned long blk_idx);
void free_wb_request(struct zram_wb_request *req);
int zram_read_wb_pages_sync(struct zram *zram, struct page **pages,
			    unsigned long start_entry, unsigned int nr_pages);
int zram_wb_write_pages_sync(struct zram *zram, struct page **pages,
			     unsigned long start_blk, unsigned int nr_pages);

/* GC functions */
void zram_init_gc(struct zram *zram);
void zram_cancel_gc(struct zram *zram);
void zram_schedule_gc(struct zram *zram, int target_pages);
#else
inline unsigned long alloc_block_bdev(struct zram *zram) { return 0; }
inline unsigned long alloc_block_bdev_batch(struct zram *zram, int req_count, int *act_count) { return 0; }
inline void free_block_bdev(struct zram *zram, unsigned long blk_idx) {};
inline void free_block_bdev_range(struct zram *zram, unsigned long blk_idx, int count) {};
inline int setup_zram_writeback(void) { return 0; }
inline void destroy_zram_writeback(void) {}
inline void zram_init_gc(struct zram *zram) {}
inline void zram_cancel_gc(struct zram *zram) {}
inline void zram_schedule_gc(struct zram *zram, int target_pages) {}
#endif

#endif /* _ZRAM_WRITEBACK_H_ */
