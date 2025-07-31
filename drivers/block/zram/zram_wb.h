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

#if IS_ENABLED(CONFIG_ZRAM_WRITEBACK)
unsigned long alloc_block_bdev(struct zram *zram);
void free_block_bdev(struct zram *zram, unsigned long blk_idx);
int setup_zram_writeback(void);
void destroy_zram_writeback(void);
struct zram_wb_request *alloc_wb_request(struct zram *zram,
					 struct zram_pp_slot *pps,
					 struct zram_pp_ctl *ppctl,
					 unsigned long blk_idx);
void free_wb_request(struct zram_wb_request *req);
#else
inline unsigned long alloc_block_bdev(struct zram *zram) { return 0; }
inline void free_block_bdev(struct zram *zram, unsigned long blk_idx) {};
inline int setup_zram_writeback(void) { return 0; }
inline void destroy_zram_writeback(void) {}
#endif

#endif /* _ZRAM_WRITEBACK_H_ */

