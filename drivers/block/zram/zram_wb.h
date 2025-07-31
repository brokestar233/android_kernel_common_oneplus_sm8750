/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _ZRAM_WRITEBACK_H_
#define _ZRAM_WRITEBACK_H_

#include <linux/bio.h>
#include "zram_drv.h"

#if IS_ENABLED(CONFIG_ZRAM_WRITEBACK)
unsigned long alloc_block_bdev(struct zram *zram);
void free_block_bdev(struct zram *zram, unsigned long blk_idx);
#else
inline unsigned long alloc_block_bdev(struct zram *zram) { return 0; }
inline void free_block_bdev(struct zram *zram, unsigned long blk_idx) {};
#endif

#endif /* _ZRAM_WRITEBACK_H_ */

