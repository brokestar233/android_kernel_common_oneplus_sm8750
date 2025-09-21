/*
 * Compressed RAM block device
 *
 * Copyright (C) 2008, 2009, 2010  Nitin Gupta
 *               2012, 2013 Minchan Kim
 *
 * This code is released using a dual license strategy: BSD/GPL
 * You can choose the licence that better fits your requirements.
 *
 * Released under the terms of 3-clause BSD License
 * Released under the terms of GNU General Public License Version 2.0
 *
 */

#define KMSG_COMPONENT "zram"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/bio.h>
#include <linux/bitops.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/device.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/backing-dev.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/sysfs.h>
#include <linux/debugfs.h>
#include <linux/cpuhotplug.h>
#include <linux/part_stat.h>
#include <linux/mm.h>
#include <linux/kthread.h>
#include <linux/game_pid.h>

#ifdef CONFIG_ZRAM_AUTO_SIZE
#include <linux/math64.h>
#include <linux/cpu.h>
#include <linux/reciprocal_div.h>
#include <linux/minmax.h>
#include <linux/intfp.h>

#ifndef FP_BITS
#define FP_BITS 16
#endif

#endif

#include "zram_drv.h"

#define CHECK_INTERVAL (30 * HZ) // 每30秒检查一次
#define MEM_THRESHOLD 80

static u64 batch_size = 512;

static struct task_struct *monitor_thread;

static DEFINE_IDR(zram_index_idr);
/* idr index must be protected */
static DEFINE_MUTEX(zram_index_mutex);

static int zram_major;
static const char *default_compressor = CONFIG_ZRAM_DEF_COMP;

/* Module params (documentation at end) */
static unsigned int num_devices = 1;
/*
 * Pages that compress to sizes equals or greater than this are stored
 * uncompressed in memory.
 */
static size_t huge_class_size;

static const struct block_device_operations zram_devops;

static void zram_free_page(struct zram *zram, size_t index);
static int zram_read_page(struct zram *zram, struct page *page, u32 index,
			  struct bio *parent);
static void zram_writeback(struct zram *zram);
static int zram_writeback_entry(struct zram *zram, struct zram_table_entry *entry);

#ifdef CONFIG_ZRAM_WRITEBACK
static void zram_init_shrinker(struct zram *zram);
#endif

#ifdef CONFIG_ZRAM_MULTI_COMP
u8 __read_mostly sysctl_zram_recomp_immediate = 1;

static struct ctl_table zram_sysctl_table[] = {
	{
		.procname	= "zram_recomp_immediate",
		.data		= &sysctl_zram_recomp_immediate,
		.maxlen		= sizeof(u8),
		.mode		= 0644,
		.proc_handler	= proc_dou8vec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_THREE,
	},
};
static struct ctl_table_header *zram_sysctl_table_header;
#endif //CONFIG_ZRAM_MULTI_COMP


static int zram_slot_trylock(struct zram *zram, u32 index)
{
	return bit_spin_trylock(ZRAM_LOCK, &zram->table[index].flags);
}

static void zram_slot_lock(struct zram *zram, u32 index)
{
	bit_spin_lock(ZRAM_LOCK, &zram->table[index].flags);
}

static void zram_slot_unlock(struct zram *zram, u32 index)
{
	bit_spin_unlock(ZRAM_LOCK, &zram->table[index].flags);
}

static inline bool init_done(struct zram *zram)
{
	return zram->disksize;
}

static inline struct zram *dev_to_zram(struct device *dev)
{
	return (struct zram *)dev_to_disk(dev)->private_data;
}

static unsigned long zram_get_handle(struct zram *zram, u32 index)
{
	return zram->table[index].handle;
}

static void zram_set_handle(struct zram *zram, u32 index, unsigned long handle)
{
	zram->table[index].handle = handle;
}

/* flag operations require table entry bit_spin_lock() being held */
static bool zram_test_flag(struct zram *zram, u32 index,
			enum zram_pageflags flag)
{
	return zram->table[index].flags & BIT(flag);
}

static void zram_set_flag(struct zram *zram, u32 index,
			enum zram_pageflags flag)
{
	zram->table[index].flags |= BIT(flag);
}

static void zram_clear_flag(struct zram *zram, u32 index,
			enum zram_pageflags flag)
{
	zram->table[index].flags &= ~BIT(flag);
}

static size_t zram_get_obj_size(struct zram *zram, u32 index)
{
	return zram->table[index].flags & (BIT(ZRAM_FLAG_SHIFT) - 1);
}

static void zram_set_obj_size(struct zram *zram,
					u32 index, size_t size)
{
	unsigned long flags = zram->table[index].flags >> ZRAM_FLAG_SHIFT;

	zram->table[index].flags = (flags << ZRAM_FLAG_SHIFT) | size;
}

static inline bool zram_allocated(struct zram *zram, u32 index)
{
	return zram_get_obj_size(zram, index) ||
			zram_test_flag(zram, index, ZRAM_SAME) ||
			zram_test_flag(zram, index, ZRAM_WB);
}

static inline void update_used_max(struct zram *zram,
					const unsigned long pages)
{
	unsigned long cur_max = atomic_long_read(&zram->stats.max_used_pages);

	do {
		if (cur_max >= pages)
			return;
	} while (!atomic_long_try_cmpxchg(&zram->stats.max_used_pages,
					  &cur_max, pages));
}

static bool zram_can_store_page(struct zram *zram)
{
	unsigned long alloced_pages;

	alloced_pages = zs_get_total_pages(zram->mem_pool);
	update_used_max(zram, alloced_pages);

	return !zram->limit_pages || alloced_pages <= zram->limit_pages;
}


#if PAGE_SIZE != 4096
static inline bool is_partial_io(struct bio_vec *bvec)
{
	return bvec->bv_len != PAGE_SIZE;
}
#define ZRAM_PARTIAL_IO		1
#else
static inline bool is_partial_io(struct bio_vec *bvec)
{
	return false;
}
#endif

#ifdef	CONFIG_ZRAM_WRITEBACK
static void zram_lru_add(struct zram *zram, struct zram_table_entry *entry)
{
    rcu_read_lock();
	entry->referenced = true;
    list_lru_add(&zram->zram_list_lru, &entry->lru);
    rcu_read_unlock();
}

static void zram_lru_del(struct zram *zram, struct zram_table_entry *entry)
{
    rcu_read_lock();
    list_lru_del(&zram->zram_list_lru, &entry->lru);
    rcu_read_unlock();
}
#endif

static inline void zram_set_priority(struct zram *zram, u32 index, u32 prio)
{
	prio &= ZRAM_COMP_PRIORITY_MASK;
	/*
	 * Clear previous priority value first, in case if we recompress
	 * further an already recompressed page
	 */
	zram->table[index].flags &= ~(ZRAM_COMP_PRIORITY_MASK <<
				      ZRAM_COMP_PRIORITY_BIT1);
	zram->table[index].flags |= (prio << ZRAM_COMP_PRIORITY_BIT1);
}

static inline u32 zram_get_priority(struct zram *zram, u32 index)
{
	u32 prio = zram->table[index].flags >> ZRAM_COMP_PRIORITY_BIT1;

	return prio & ZRAM_COMP_PRIORITY_MASK;
}

static void zram_accessed(struct zram *zram, u32 index)
{
	/* Remove from LRU list if present */
	zram_lru_del(zram, &zram->table[index]);
	zram_clear_flag(zram, index, ZRAM_IDLE);
#ifdef CONFIG_ZRAM_TRACK_ENTRY_ACTIME
	zram->table[index].ac_time = ktime_get_boottime();
#endif
}

static inline void zram_fill_page(void *ptr, unsigned long len,
					unsigned long value)
{
	WARN_ON_ONCE(!IS_ALIGNED(len, sizeof(unsigned long)));
	memset_l(ptr, value, len / sizeof(unsigned long));
}

static bool page_same_filled(void *ptr, unsigned long *element)
{
	unsigned long *page;
	unsigned long val;
	unsigned int pos, last_pos = PAGE_SIZE / sizeof(*page) - 1;

	page = (unsigned long *)ptr;
	val = page[0];

	if (val != page[last_pos])
		return false;

	for (pos = 1; pos < last_pos; pos++) {
		if (val != page[pos])
			return false;
	}

	*element = val;

	return true;
}

static ssize_t initstate_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	u32 val;
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	val = init_done(zram);
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%u\n", val);
}

static ssize_t disksize_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return scnprintf(buf, PAGE_SIZE, "%llu\n", zram->disksize);
}

static ssize_t mem_limit_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	u64 limit;
	char *tmp;
	struct zram *zram = dev_to_zram(dev);

	limit = memparse(buf, &tmp);
	if (buf == tmp) /* no chars parsed, invalid input */
		return -EINVAL;

	down_write(&zram->init_lock);
	zram->limit_pages = PAGE_ALIGN(limit) >> PAGE_SHIFT;
	up_write(&zram->init_lock);

	return len;
}

static ssize_t mem_used_max_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	int err;
	unsigned long val;
	struct zram *zram = dev_to_zram(dev);

	err = kstrtoul(buf, 10, &val);
	if (err || val != 0)
		return -EINVAL;

	down_read(&zram->init_lock);
	if (init_done(zram)) {
		atomic_long_set(&zram->stats.max_used_pages,
				zs_get_total_pages(zram->mem_pool));
	}
	up_read(&zram->init_lock);

	return len;
}

/*
 * Mark all pages which are older than or equal to cutoff as IDLE.
 * Callers should hold the zram init lock in read mode
 */
static void mark_idle(struct zram *zram, ktime_t cutoff)
{
	unsigned long nr_pages = zram->disksize >> PAGE_SHIFT;
	int index;

	for (index = 0; index < nr_pages; index++) {
		/*
		 * Do not mark ZRAM_UNDER_WB slot as ZRAM_IDLE to close race.
		 * See the comment in writeback_store.
		 *
		 * Also do not mark ZRAM_SAME slots as ZRAM_IDLE, because no
		 * post-processing (recompress, writeback) happens to the
		 * ZRAM_SAME slot.
		 *
		 * And ZRAM_WB slots simply cannot be ZRAM_IDLE.
		 */
		zram_slot_lock(zram, index);
		if (!zram_allocated(zram, index) ||
		    zram_test_flag(zram, index, ZRAM_WB) ||
		    zram_test_flag(zram, index, ZRAM_UNDER_WB) ||
		    zram_test_flag(zram, index, ZRAM_SAME)) {
			zram_slot_unlock(zram, index);
			continue;
		}

#ifdef CONFIG_ZRAM_TRACK_ENTRY_ACTIME
		if (!cutoff || ktime_after(cutoff, zram->table[index].ac_time)) {
			if (!zram_test_flag(zram, index, ZRAM_IDLE)) {
				zram_set_flag(zram, index, ZRAM_IDLE);
				/* Add to LRU list for shrinker */
				zram_lru_add(zram, &zram->table[index]);
			}
		} else {
			/* Page was accessed recently, make sure it's not marked as IDLE */
			if (zram_test_flag(zram, index, ZRAM_IDLE)) {
				zram_clear_flag(zram, index, ZRAM_IDLE);
				zram_lru_del(zram, &zram->table[index]);
			}
		}
#else
		/* When access time tracking is disabled, we only mark pages as IDLE
		 * if they are not already marked. This prevents re-adding pages
		 * to the LRU list unnecessarily.
		 */
		if (!zram_test_flag(zram, index, ZRAM_IDLE)) {
			zram_set_flag(zram, index, ZRAM_IDLE);
			/* Add to LRU list for shrinker */
			zram_lru_add(zram, &zram->table[index]);
		}
#endif
		zram_slot_unlock(zram, index);
	}
}

static ssize_t idle_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	ktime_t cutoff_time = 0;
	ssize_t rv = -EINVAL;

	if (!sysfs_streq(buf, "all")) {
		/*
		 * If it did not parse as 'all' try to treat it as an integer
		 * when we have memory tracking enabled.
		 */
		u64 age_sec;

		if (IS_ENABLED(CONFIG_ZRAM_TRACK_ENTRY_ACTIME) && !kstrtoull(buf, 0, &age_sec))
			cutoff_time = ktime_sub(ktime_get_boottime(),
					ns_to_ktime(age_sec * NSEC_PER_SEC));
		else
			goto out;
	}

	down_read(&zram->init_lock);
	if (!init_done(zram))
		goto out_unlock;

	/*
	 * A cutoff_time of 0 marks everything as idle, this is the
	 * "all" behavior.
	 */
	mark_idle(zram, cutoff_time);
	rv = len;

out_unlock:
	up_read(&zram->init_lock);
out:
	return rv;
}

#ifdef CONFIG_ZRAM_WRITEBACK
static ssize_t writeback_limit_enable_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	u64 val;
	ssize_t ret = -EINVAL;

	if (kstrtoull(buf, 10, &val))
		return ret;

	down_read(&zram->init_lock);
	spin_lock(&zram->wb_limit_lock);
	zram->wb_limit_enable = val;
	spin_unlock(&zram->wb_limit_lock);
	up_read(&zram->init_lock);
	ret = len;

	return ret;
}

static ssize_t writeback_limit_enable_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	bool val;
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	spin_lock(&zram->wb_limit_lock);
	val = zram->wb_limit_enable;
	spin_unlock(&zram->wb_limit_lock);
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%d\n", val);
}

static ssize_t writeback_limit_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	u64 val;
	ssize_t ret = -EINVAL;

	if (kstrtoull(buf, 10, &val))
		return ret;

	down_read(&zram->init_lock);
	spin_lock(&zram->wb_limit_lock);
	zram->bd_wb_limit = val;
	spin_unlock(&zram->wb_limit_lock);
	up_read(&zram->init_lock);
	ret = len;

	return ret;
}

static ssize_t writeback_limit_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	u64 val;
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	spin_lock(&zram->wb_limit_lock);
	val = zram->bd_wb_limit;
	spin_unlock(&zram->wb_limit_lock);
	up_read(&zram->init_lock);

	return scnprintf(buf, PAGE_SIZE, "%llu\n", val);
}

static void reset_bdev(struct zram *zram)
{
	struct block_device *bdev;

	if (!zram->backing_dev)
		return;

	bdev = zram->bdev;
	blkdev_put(bdev, zram);
	/* hope filp_close flush all of IO */
	filp_close(zram->backing_dev, NULL);
	zram->backing_dev = NULL;
	zram->bdev = NULL;
	zram->disk->fops = &zram_devops;
	kvfree(zram->bitmap);
	zram->bitmap = NULL;
}

static ssize_t backing_dev_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct file *file;
	struct zram *zram = dev_to_zram(dev);
	char *p;
	ssize_t ret;

	down_read(&zram->init_lock);
	file = zram->backing_dev;
	if (!file) {
		memcpy(buf, "none\n", 5);
		up_read(&zram->init_lock);
		return 5;
	}

	p = file_path(file, buf, PAGE_SIZE - 1);
	if (IS_ERR(p)) {
		ret = PTR_ERR(p);
		goto out;
	}

	ret = strlen(p);
	memmove(buf, p, ret);
	buf[ret++] = '\n';
out:
	up_read(&zram->init_lock);
	return ret;
}

static ssize_t backing_dev_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	char *file_name;
	size_t sz;
	struct file *backing_dev = NULL;
	struct inode *inode;
	struct address_space *mapping;
	unsigned int bitmap_sz;
	unsigned long nr_pages, *bitmap = NULL;
	struct block_device *bdev = NULL;
	int err;
	struct zram *zram = dev_to_zram(dev);

	file_name = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!file_name)
		return -ENOMEM;

	down_write(&zram->init_lock);
	if (init_done(zram)) {
		pr_info("Can't setup backing device for initialized device\n");
		err = -EBUSY;
		goto out;
	}

	strscpy(file_name, buf, PATH_MAX);
	/* ignore trailing newline */
	sz = strlen(file_name);
	if (sz > 0 && file_name[sz - 1] == '\n')
		file_name[sz - 1] = 0x00;

	backing_dev = filp_open_block(file_name, O_RDWR|O_LARGEFILE, 0);
	if (IS_ERR(backing_dev)) {
		err = PTR_ERR(backing_dev);
		backing_dev = NULL;
		goto out;
	}

	mapping = backing_dev->f_mapping;
	inode = mapping->host;

	/* Support only block device in this moment */
	if (!S_ISBLK(inode->i_mode)) {
		err = -ENOTBLK;
		goto out;
	}

	bdev = blkdev_get_by_dev(inode->i_rdev, BLK_OPEN_READ | BLK_OPEN_WRITE,
				 zram, NULL);
	if (IS_ERR(bdev)) {
		err = PTR_ERR(bdev);
		bdev = NULL;
		goto out;
	}

	nr_pages = i_size_read(inode) >> PAGE_SHIFT;
	/* Refuse to use zero sized device (also prevents self reference) */
	if (!nr_pages) {
		err = -EINVAL;
		goto out;
	}

	bitmap_sz = BITS_TO_LONGS(nr_pages) * sizeof(long);
	bitmap = kvzalloc(bitmap_sz, GFP_KERNEL);
	if (!bitmap) {
		err = -ENOMEM;
		goto out;
	}

	reset_bdev(zram);

	zram->bdev = bdev;
	zram->backing_dev = backing_dev;
	zram->bitmap = bitmap;
	zram->nr_pages = nr_pages;
	up_write(&zram->init_lock);

	pr_info("setup backing device %s\n", file_name);
	kfree(file_name);

	return len;
out:
	kvfree(bitmap);

	if (bdev)
		blkdev_put(bdev, zram);

	if (backing_dev)
		filp_close(backing_dev, NULL);

	up_write(&zram->init_lock);

	kfree(file_name);

	return err;
}

static unsigned long alloc_block_bdev(struct zram *zram)
{
	unsigned long blk_idx = 1;
retry:
	/* skip 0 bit to confuse zram.handle = 0 */
	blk_idx = find_next_zero_bit(zram->bitmap, zram->nr_pages, blk_idx);
	if (blk_idx == zram->nr_pages)
		return 0;

	if (test_and_set_bit(blk_idx, zram->bitmap))
		goto retry;

	atomic64_inc(&zram->stats.bd_count);
	return blk_idx;
}

static void free_block_bdev(struct zram *zram, unsigned long blk_idx)
{
	int was_set;

	was_set = test_and_clear_bit(blk_idx, zram->bitmap);
	WARN_ON_ONCE(!was_set);
	atomic64_dec(&zram->stats.bd_count);
}

static void read_from_bdev_async(struct zram *zram, struct page *page,
			unsigned long entry, struct bio *parent)
{
	struct bio *bio;

	bio = bio_alloc(zram->bdev, 1, parent->bi_opf, GFP_NOIO);
	bio->bi_iter.bi_sector = entry * (PAGE_SIZE >> 9);
	__bio_add_page(bio, page, PAGE_SIZE, 0);
	bio_chain(bio, parent);
	submit_bio(bio);
}

#define PAGE_WB_SIG "page_index="

#define PAGE_WRITEBACK			0
#define HUGE_WRITEBACK			(1<<0)
#define IDLE_WRITEBACK			(1<<1)
#define INCOMPRESSIBLE_WRITEBACK	(1<<2)

static ssize_t writeback_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	unsigned long nr_pages = zram->disksize >> PAGE_SHIFT;
	unsigned long index = 0;
	struct bio bio;
	struct bio_vec bio_vec;
	struct page *page;
	ssize_t ret = len;
	int mode, err;
	unsigned long blk_idx = 0;

	if (sysfs_streq(buf, "idle"))
		mode = IDLE_WRITEBACK;
	else if (sysfs_streq(buf, "huge"))
		mode = HUGE_WRITEBACK;
	else if (sysfs_streq(buf, "huge_idle"))
		mode = IDLE_WRITEBACK | HUGE_WRITEBACK;
	else if (sysfs_streq(buf, "incompressible"))
		mode = INCOMPRESSIBLE_WRITEBACK;
	else {
		if (strncmp(buf, PAGE_WB_SIG, sizeof(PAGE_WB_SIG) - 1))
			return -EINVAL;

		if (kstrtol(buf + sizeof(PAGE_WB_SIG) - 1, 10, &index) ||
				index >= nr_pages)
			return -EINVAL;

		nr_pages = 1;
		mode = PAGE_WRITEBACK;
	}

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		ret = -EINVAL;
		goto release_init_lock;
	}

	if (!zram->backing_dev) {
		ret = -ENODEV;
		goto release_init_lock;
	}

	page = alloc_page(GFP_KERNEL);
	if (!page) {
		ret = -ENOMEM;
		goto release_init_lock;
	}

	for (; nr_pages != 0; index++, nr_pages--) {
		spin_lock(&zram->wb_limit_lock);
		if (zram->wb_limit_enable && !zram->bd_wb_limit) {
			spin_unlock(&zram->wb_limit_lock);
			ret = -EIO;
			break;
		}
		spin_unlock(&zram->wb_limit_lock);

		if (!blk_idx) {
			blk_idx = alloc_block_bdev(zram);
			if (!blk_idx) {
				ret = -ENOSPC;
				break;
			}
		}

		zram_slot_lock(zram, index);
		if (!zram_allocated(zram, index))
			goto next;

		if (zram_test_flag(zram, index, ZRAM_WB) ||
				zram_test_flag(zram, index, ZRAM_SAME) ||
				zram_test_flag(zram, index, ZRAM_UNDER_WB))
			goto next;

		if (mode & IDLE_WRITEBACK &&
		    !zram_test_flag(zram, index, ZRAM_IDLE))
			goto next;
		if (mode & HUGE_WRITEBACK &&
		    !zram_test_flag(zram, index, ZRAM_HUGE))
			goto next;
		if (mode & INCOMPRESSIBLE_WRITEBACK &&
		    !zram_test_flag(zram, index, ZRAM_INCOMPRESSIBLE))
			goto next;

		/*
		 * Clearing ZRAM_UNDER_WB is duty of caller.
		 * IOW, zram_free_page never clear it.
		 */
		zram_set_flag(zram, index, ZRAM_UNDER_WB);
		/* Need for hugepage writeback racing */
		zram_set_flag(zram, index, ZRAM_IDLE);
		zram_slot_unlock(zram, index);
		if (zram_read_page(zram, page, index, NULL)) {
			zram_slot_lock(zram, index);
			zram_clear_flag(zram, index, ZRAM_UNDER_WB);
			zram_clear_flag(zram, index, ZRAM_IDLE);
			zram_slot_unlock(zram, index);
			continue;
		}

		bio_init(&bio, zram->bdev, &bio_vec, 1,
			 REQ_OP_WRITE | REQ_SYNC);
		bio.bi_iter.bi_sector = blk_idx * (PAGE_SIZE >> 9);
		__bio_add_page(&bio, page, PAGE_SIZE, 0);

		/*
		 * XXX: A single page IO would be inefficient for write
		 * but it would be not bad as starter.
		 */
		err = submit_bio_wait(&bio);
		if (err) {
			zram_slot_lock(zram, index);
			zram_clear_flag(zram, index, ZRAM_UNDER_WB);
			zram_clear_flag(zram, index, ZRAM_IDLE);
			zram_slot_unlock(zram, index);
			/*
			 * BIO errors are not fatal, we continue and simply
			 * attempt to writeback the remaining objects (pages).
			 * At the same time we need to signal user-space that
			 * some writes (at least one, but also could be all of
			 * them) were not successful and we do so by returning
			 * the most recent BIO error.
			 */
			ret = err;
			continue;
		}

		atomic64_inc(&zram->stats.bd_writes);
		/*
		 * We released zram_slot_lock so need to check if the slot was
		 * changed. If there is freeing for the slot, we can catch it
		 * easily by zram_allocated.
		 * A subtle case is the slot is freed/reallocated/marked as
		 * ZRAM_IDLE again. To close the race, idle_store doesn't
		 * mark ZRAM_IDLE once it found the slot was ZRAM_UNDER_WB.
		 * Thus, we could close the race by checking ZRAM_IDLE bit.
		 */
		zram_slot_lock(zram, index);
		if (!zram_allocated(zram, index) ||
			  !zram_test_flag(zram, index, ZRAM_IDLE)) {
			zram_clear_flag(zram, index, ZRAM_UNDER_WB);
			zram_clear_flag(zram, index, ZRAM_IDLE);
			goto next;
		}

		zram_free_page(zram, index);
		zram_clear_flag(zram, index, ZRAM_UNDER_WB);
		zram_set_flag(zram, index, ZRAM_WB);
		zram_set_handle(zram, index, blk_idx);
		blk_idx = 0;
		atomic64_inc(&zram->stats.pages_stored);
		spin_lock(&zram->wb_limit_lock);
		if (zram->wb_limit_enable && zram->bd_wb_limit > 0)
			zram->bd_wb_limit -=  1UL << (PAGE_SHIFT - 12);
		spin_unlock(&zram->wb_limit_lock);
next:
		zram_slot_unlock(zram, index);
	}

	if (blk_idx)
		free_block_bdev(zram, blk_idx);
	__free_page(page);
release_init_lock:
	up_read(&zram->init_lock);

	return ret;
}

struct zram_work {
	struct work_struct work;
	struct zram *zram;
	unsigned long entry;
	struct page *page;
	int error;
};

static void zram_sync_read(struct work_struct *work)
{
	struct zram_work *zw = container_of(work, struct zram_work, work);
	struct bio_vec bv;
	struct bio bio;

	bio_init(&bio, zw->zram->bdev, &bv, 1, REQ_OP_READ);
	bio.bi_iter.bi_sector = zw->entry * (PAGE_SIZE >> 9);
	__bio_add_page(&bio, zw->page, PAGE_SIZE, 0);
	zw->error = submit_bio_wait(&bio);
}

/*
 * Block layer want one ->submit_bio to be active at a time, so if we use
 * chained IO with parent IO in same context, it's a deadlock. To avoid that,
 * use a worker thread context.
 */
static int read_from_bdev_sync(struct zram *zram, struct page *page,
				unsigned long entry)
{
	struct zram_work work;

	work.page = page;
	work.zram = zram;
	work.entry = entry;

	INIT_WORK_ONSTACK(&work.work, zram_sync_read);
	queue_work(system_unbound_wq, &work.work);
	flush_work(&work.work);
	destroy_work_on_stack(&work.work);

	return work.error;
}

static int read_from_bdev(struct zram *zram, struct page *page,
			unsigned long entry, struct bio *parent)
{
	atomic64_inc(&zram->stats.bd_reads);
	if (!parent) {
		if (WARN_ON_ONCE(!IS_ENABLED(ZRAM_PARTIAL_IO)))
			return -EIO;
		return read_from_bdev_sync(zram, page, entry);
	}
	read_from_bdev_async(zram, page, entry, parent);
	return 0;
}
#else
static inline void reset_bdev(struct zram *zram) {};
static int read_from_bdev(struct zram *zram, struct page *page,
			unsigned long entry, struct bio *parent)
{
	return -EIO;
}

static void free_block_bdev(struct zram *zram, unsigned long blk_idx) {};
#endif

#ifdef CONFIG_ZRAM_MEMORY_TRACKING

static struct dentry *zram_debugfs_root;

static void zram_debugfs_create(void)
{
	zram_debugfs_root = debugfs_create_dir("zram", NULL);
}

static void zram_debugfs_destroy(void)
{
	debugfs_remove_recursive(zram_debugfs_root);
}

static ssize_t read_block_state(struct file *file, char __user *buf,
				size_t count, loff_t *ppos)
{
	char *kbuf;
	ssize_t index, written = 0;
	struct zram *zram = file->private_data;
	unsigned long nr_pages = zram->disksize >> PAGE_SHIFT;
	struct timespec64 ts;

	kbuf = kvmalloc(count, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		up_read(&zram->init_lock);
		kvfree(kbuf);
		return -EINVAL;
	}

	for (index = *ppos; index < nr_pages; index++) {
		int copied;

		zram_slot_lock(zram, index);
		if (!zram_allocated(zram, index))
			goto next;

		ts = ktime_to_timespec64(zram->table[index].ac_time);
		copied = snprintf(kbuf + written, count,
			"%12zd %12lld.%06lu %c%c%c%c%c%c\n",
			index, (s64)ts.tv_sec,
			ts.tv_nsec / NSEC_PER_USEC,
			zram_test_flag(zram, index, ZRAM_SAME) ? 's' : '.',
			zram_test_flag(zram, index, ZRAM_WB) ? 'w' : '.',
			zram_test_flag(zram, index, ZRAM_HUGE) ? 'h' : '.',
			zram_test_flag(zram, index, ZRAM_IDLE) ? 'i' : '.',
			zram_get_priority(zram, index) ? 'r' : '.',
			zram_test_flag(zram, index,
				       ZRAM_INCOMPRESSIBLE) ? 'n' : '.');

		if (count <= copied) {
			zram_slot_unlock(zram, index);
			break;
		}
		written += copied;
		count -= copied;
next:
		zram_slot_unlock(zram, index);
		*ppos += 1;
	}

	up_read(&zram->init_lock);
	if (copy_to_user(buf, kbuf, written))
		written = -EFAULT;
	kvfree(kbuf);

	return written;
}

static const struct file_operations proc_zram_block_state_op = {
	.open = simple_open,
	.read = read_block_state,
	.llseek = default_llseek,
};

static void zram_debugfs_register(struct zram *zram)
{
	if (!zram_debugfs_root)
		return;

	zram->debugfs_dir = debugfs_create_dir(zram->disk->disk_name,
						zram_debugfs_root);
	debugfs_create_file("block_state", 0400, zram->debugfs_dir,
				zram, &proc_zram_block_state_op);
}

static void zram_debugfs_unregister(struct zram *zram)
{
	debugfs_remove_recursive(zram->debugfs_dir);
}
#else
static void zram_debugfs_create(void) {};
static void zram_debugfs_destroy(void) {};
static void zram_debugfs_register(struct zram *zram) {};
static void zram_debugfs_unregister(struct zram *zram) {};
#endif

/*
 * We switched to per-cpu streams and this attr is not needed anymore.
 * However, we will keep it around for some time, because:
 * a) we may revert per-cpu streams in the future
 * b) it's visible to user space and we need to follow our 2 years
 *    retirement rule; but we already have a number of 'soon to be
 *    altered' attrs, so max_comp_streams need to wait for the next
 *    layoff cycle.
 */
static ssize_t max_comp_streams_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%d\n", num_online_cpus());
}

static ssize_t max_comp_streams_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	return len;
}

static void comp_algorithm_set(struct zram *zram, u32 prio, const char *alg)
{
	/* Do not free statically defined compression algorithms */
	if (zram->comp_algs[prio] != default_compressor)
		kfree(zram->comp_algs[prio]);

	zram->comp_algs[prio] = alg;
}

static ssize_t __comp_algorithm_show(struct zram *zram, u32 prio, char *buf)
{
	ssize_t sz;

	down_read(&zram->init_lock);
	sz = zcomp_available_show(zram->comp_algs[prio], buf);
	up_read(&zram->init_lock);

	return sz;
}

static int __comp_algorithm_store(struct zram *zram, u32 prio, const char *buf)
{
	char *compressor;
	size_t sz;

	sz = strlen(buf);
	if (sz >= CRYPTO_MAX_ALG_NAME)
		return -E2BIG;

	compressor = kstrdup(buf, GFP_KERNEL);
	if (!compressor)
		return -ENOMEM;

	/* ignore trailing newline */
	if (sz > 0 && compressor[sz - 1] == '\n')
		compressor[sz - 1] = 0x00;

	if (!zcomp_available_algorithm(compressor)) {
		kfree(compressor);
		return -EINVAL;
	}

	down_write(&zram->init_lock);
	if (init_done(zram)) {
		up_write(&zram->init_lock);
		kfree(compressor);
		pr_info("Can't change algorithm for initialized device\n");
		return -EBUSY;
	}

	comp_algorithm_set(zram, prio, compressor);
	up_write(&zram->init_lock);
	return 0;
}

static ssize_t comp_algorithm_show(struct device *dev,
				   struct device_attribute *attr,
				   char *buf)
{
	struct zram *zram = dev_to_zram(dev);

	return __comp_algorithm_show(zram, ZRAM_PRIMARY_COMP, buf);
}

static ssize_t comp_algorithm_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf,
				    size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	int ret;

	ret = __comp_algorithm_store(zram, ZRAM_PRIMARY_COMP, buf);
	return ret ? ret : len;
}

#ifdef CONFIG_ZRAM_MULTI_COMP
static ssize_t recomp_algorithm_show(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t sz = 0;
	u32 prio;

	for (prio = ZRAM_SECONDARY_COMP; prio < ZRAM_MAX_COMPS; prio++) {
		if (!zram->comp_algs[prio])
			continue;

		sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2, "#%d: ", prio);
		sz += __comp_algorithm_show(zram, prio, buf + sz);
	}

	return sz;
}

static ssize_t recomp_algorithm_store(struct device *dev,
				      struct device_attribute *attr,
				      const char *buf,
				      size_t len)
{
	struct zram *zram = dev_to_zram(dev);
	int prio = ZRAM_SECONDARY_COMP;
	char *args, *param, *val;
	char *alg = NULL;
	int ret;

	args = skip_spaces(buf);
	while (*args) {
		args = next_arg(args, &param, &val);

		if (!val || !*val)
			return -EINVAL;

		if (!strcmp(param, "algo")) {
			alg = val;
			continue;
		}

		if (!strcmp(param, "priority")) {
			ret = kstrtoint(val, 10, &prio);
			if (ret)
				return ret;
			continue;
		}
	}

	if (!alg)
		return -EINVAL;

	if (prio < ZRAM_SECONDARY_COMP || prio >= ZRAM_MAX_COMPS)
		return -EINVAL;

	ret = __comp_algorithm_store(zram, prio, alg);
	return ret ? ret : len;
}
#endif

static ssize_t compact_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	struct zram *zram = dev_to_zram(dev);

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		up_read(&zram->init_lock);
		return -EINVAL;
	}

	zs_compact(zram->mem_pool);
	up_read(&zram->init_lock);

	return len;
}

static ssize_t io_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t ret;

	down_read(&zram->init_lock);
	ret = scnprintf(buf, PAGE_SIZE,
			"%8llu %8llu 0 %8llu\n",
			(u64)atomic64_read(&zram->stats.failed_reads),
			(u64)atomic64_read(&zram->stats.failed_writes),
			(u64)atomic64_read(&zram->stats.notify_free));
	up_read(&zram->init_lock);

	return ret;
}

static ssize_t mm_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	struct zs_pool_stats pool_stats;
	u64 orig_size, mem_used = 0;
	long max_used;
	ssize_t ret;

	memset(&pool_stats, 0x00, sizeof(struct zs_pool_stats));

	down_read(&zram->init_lock);
	if (init_done(zram)) {
		mem_used = zs_get_total_pages(zram->mem_pool);
		zs_pool_stats(zram->mem_pool, &pool_stats);
	}

	orig_size = atomic64_read(&zram->stats.pages_stored);
	max_used = atomic_long_read(&zram->stats.max_used_pages);

	ret = scnprintf(buf, PAGE_SIZE,
			"%8llu %8llu %8llu %8lu %8ld %8llu %8lu %8llu %8llu\n",
			orig_size << PAGE_SHIFT,
			(u64)atomic64_read(&zram->stats.compr_data_size),
			mem_used << PAGE_SHIFT,
			zram->limit_pages << PAGE_SHIFT,
			max_used << PAGE_SHIFT,
			(u64)atomic64_read(&zram->stats.same_pages),
			atomic_long_read(&pool_stats.pages_compacted),
			(u64)atomic64_read(&zram->stats.huge_pages),
			(u64)atomic64_read(&zram->stats.huge_pages_since));
	up_read(&zram->init_lock);

	return ret;
}

#ifdef CONFIG_ZRAM_WRITEBACK
#define FOUR_K(x) ((x) * (1 << (PAGE_SHIFT - 12)))
static ssize_t bd_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct zram *zram = dev_to_zram(dev);
	ssize_t ret;

	down_read(&zram->init_lock);
	ret = scnprintf(buf, PAGE_SIZE,
		"%8llu %8llu %8llu\n",
			FOUR_K((u64)atomic64_read(&zram->stats.bd_count)),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_reads)),
			FOUR_K((u64)atomic64_read(&zram->stats.bd_writes)));
	up_read(&zram->init_lock);

	return ret;
}
#endif

static ssize_t debug_stat_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int version = 1;
	struct zram *zram = dev_to_zram(dev);
	ssize_t ret;

	down_read(&zram->init_lock);
	ret = scnprintf(buf, PAGE_SIZE,
			"version: %d\n%8llu %8llu\n",
			version,
			(u64)atomic64_read(&zram->stats.writestall),
			(u64)atomic64_read(&zram->stats.miss_free));
	up_read(&zram->init_lock);

	return ret;
}

static DEVICE_ATTR_RO(io_stat);
static DEVICE_ATTR_RO(mm_stat);
#ifdef CONFIG_ZRAM_WRITEBACK
static DEVICE_ATTR_RO(bd_stat);
#endif
static DEVICE_ATTR_RO(debug_stat);

static void zram_meta_free(struct zram *zram, u64 disksize)
{
	size_t num_pages = disksize >> PAGE_SHIFT;
	size_t index;

	if (!zram->table)
		return;

	/* Free all pages that are still in this zram device */
	for (index = 0; index < num_pages; index++)
		zram_free_page(zram, index);

	zs_destroy_pool(zram->mem_pool);
	
	/* Destroy the per-device idle LRU */
	list_lru_destroy(&zram->zram_list_lru);
	
	vfree(zram->table);
	zram->table = NULL;
}

static bool zram_meta_alloc(struct zram *zram, u64 disksize)
{
	size_t num_pages;
	u32 index;

	num_pages = disksize >> PAGE_SHIFT;
	zram->table = vzalloc(array_size(num_pages, sizeof(*zram->table)));
	if (!zram->table)
		return false;

	/* Initialize the LRU list heads for each table entry */
	for (index = 0; index < num_pages; index++)
		INIT_LIST_HEAD(&zram->table[index].lru);

	zram->mem_pool = zs_create_pool(zram->disk->disk_name);
	if (!zram->mem_pool) {
		vfree(zram->table);
		zram->table = NULL;
		return false;
	}

	if (!huge_class_size)
		huge_class_size = zs_huge_class_size(zram->mem_pool);
	
	/* Initialize the per-device idle LRU */
	if (list_lru_init(&zram->zram_list_lru))
		return false;
	
	return true;
}

/*
 * To protect concurrent access to the same index entry,
 * caller should hold this table index entry's bit_spinlock to
 * indicate this index entry is accessing.
 */
static void zram_free_page(struct zram *zram, size_t index)
{
	unsigned long handle;

#ifdef CONFIG_ZRAM_TRACK_ENTRY_ACTIME
	zram->table[index].ac_time = 0;
#endif
	/* Remove from LRU list if present */
	if (zram_test_flag(zram, index, ZRAM_IDLE)) {
		zram_lru_del(zram, &zram->table[index]);
		zram_clear_flag(zram, index, ZRAM_IDLE);
	}

	if (zram_test_flag(zram, index, ZRAM_HUGE)) {
		zram_clear_flag(zram, index, ZRAM_HUGE);
		atomic64_dec(&zram->stats.huge_pages);
	}

	if (zram_test_flag(zram, index, ZRAM_INCOMPRESSIBLE))
		zram_clear_flag(zram, index, ZRAM_INCOMPRESSIBLE);

	zram_set_priority(zram, index, 0);

	if (zram_test_flag(zram, index, ZRAM_WB)) {
		zram_clear_flag(zram, index, ZRAM_WB);
		free_block_bdev(zram, zram_get_handle(zram, index));
		goto out;
	}

	/*
	 * No memory is allocated for same element filled pages.
	 * Simply clear same page flag.
	 */
	if (zram_test_flag(zram, index, ZRAM_SAME)) {
		zram_clear_flag(zram, index, ZRAM_SAME);
		atomic64_dec(&zram->stats.same_pages);
		goto out;
	}

	handle = zram_get_handle(zram, index);
	if (!handle)
		return;

	zs_free(zram->mem_pool, handle);

	atomic64_sub(zram_get_obj_size(zram, index),
			&zram->stats.compr_data_size);
out:
	atomic64_dec(&zram->stats.pages_stored);
	zram_set_handle(zram, index, 0);
	zram_set_obj_size(zram, index, 0);
	WARN_ON_ONCE(zram->table[index].flags &
		~(1UL << ZRAM_LOCK | 1UL << ZRAM_UNDER_WB));
}

static int read_same_filled_page(struct zram *zram, struct page *page,
				 u32 index)
{
	void *mem;

	mem = kmap_local_page(page);
	zram_fill_page(mem, PAGE_SIZE, zram_get_handle(zram, index));
	kunmap_local(mem);
	return 0;
}

static int read_incompressible_page(struct zram *zram, struct page *page,
				    u32 index)
{
	unsigned long handle;
	void *src, *dst;

	handle = zram_get_handle(zram, index);
	src = zs_map_object(zram->mem_pool, handle, ZS_MM_RO);
	dst = kmap_local_page(page);
	copy_page(dst, src);
	kunmap_local(dst);
	zs_unmap_object(zram->mem_pool, handle);

	return 0;
}

static int read_compressed_page(struct zram *zram, struct page *page, u32 index)
{
	struct zcomp_strm *zstrm;
	unsigned long handle;
	unsigned int size;
	void *src, *dst;
	int ret, prio;

	handle = zram_get_handle(zram, index);
	size = zram_get_obj_size(zram, index);
	prio = zram_get_priority(zram, index);

	zstrm = zcomp_stream_get(zram->comps[prio]);
	src = zs_map_object(zram->mem_pool, handle, ZS_MM_RO);
	dst = kmap_local_page(page);
	ret = zcomp_decompress(zstrm, src, size, dst);
	kunmap_local(dst);
	zs_unmap_object(zram->mem_pool, handle);
	zcomp_stream_put(zram->comps[prio]);

	return ret;
}

/*
 * Reads (decompresses if needed) a page from zspool (zsmalloc).
 * Corresponding ZRAM slot should be locked.
 */
static int zram_read_from_zspool(struct zram *zram, struct page *page,
				 u32 index)
{
	if (zram_test_flag(zram, index, ZRAM_SAME) ||
	    !zram_get_handle(zram, index))
		return read_same_filled_page(zram, page, index);

	if (!zram_test_flag(zram, index, ZRAM_HUGE))
		return read_compressed_page(zram, page, index);
	else
		return read_incompressible_page(zram, page, index);
}

static int zram_read_page(struct zram *zram, struct page *page, u32 index,
			  struct bio *parent)
{
	int ret;

	zram_slot_lock(zram, index);
	if (!zram_test_flag(zram, index, ZRAM_WB)) {
		/* Slot should be locked through out the function call */
		ret = zram_read_from_zspool(zram, page, index);
		zram_slot_unlock(zram, index);
	} else {
		/*
		 * The slot should be unlocked before reading from the backing
		 * device.
		 */
		zram_slot_unlock(zram, index);

		ret = read_from_bdev(zram, page, zram_get_handle(zram, index),
				     parent);
	}

	/* Should NEVER happen. Return bio error if it does. */
	if (WARN_ON(ret < 0))
		pr_err("Decompression failed! err=%d, page=%u\n", ret, index);

	return ret;
}

/*
 * Use a temporary buffer to decompress the page, as the decompressor
 * always expects a full page for the output.
 */
static int zram_bvec_read_partial(struct zram *zram, struct bio_vec *bvec,
				  u32 index, int offset)
{
	struct page *page = alloc_page(GFP_NOIO);
	int ret;

	if (!page)
		return -ENOMEM;
	ret = zram_read_page(zram, page, index, NULL);
	if (likely(!ret))
		memcpy_to_bvec(bvec, page_address(page) + offset);
	__free_page(page);
	return ret;
}

static int zram_bvec_read(struct zram *zram, struct bio_vec *bvec,
			  u32 index, int offset, struct bio *bio)
{
	if (is_partial_io(bvec))
		return zram_bvec_read_partial(zram, bvec, index, offset);
	return zram_read_page(zram, bvec->bv_page, index, bio);
}

static int write_same_filled_page(struct zram *zram, unsigned long fill,
				  u32 index)
{
	zram_slot_lock(zram, index);
	zram_set_flag(zram, index, ZRAM_SAME);
	zram_set_handle(zram, index, fill);
	zram_slot_unlock(zram, index);

	atomic64_inc(&zram->stats.same_pages);
	atomic64_inc(&zram->stats.pages_stored);

	return 0;
}

static int write_incompressible_page(struct zram *zram, struct page *page,
				     u32 index, u8 prio)
{
	unsigned long handle;
	void *src, *dst;

	/*
	 * This function is called from preemptible context so we don't need
	 * to do optimistic and fallback to pessimistic handle allocation,
	 * like we do for compressible pages.
	 */
	handle = zs_malloc(zram->mem_pool, PAGE_SIZE,
			   GFP_NOIO | __GFP_HIGHMEM | __GFP_MOVABLE);
	if (IS_ERR_VALUE(handle))
		return PTR_ERR((void *)handle);

	if (!zram_can_store_page(zram)) {
		zcomp_stream_put(zram->comps[ZRAM_PRIMARY_COMP]);
		zs_free(zram->mem_pool, handle);
		return -ENOMEM;
	}

	dst = zs_map_object(zram->mem_pool, handle, ZS_MM_WO);
	src = kmap_local_page(page);
	memcpy(dst, src, PAGE_SIZE);
	kunmap_local(src);
	zs_unmap_object(zram->mem_pool, handle);

	zram_slot_lock(zram, index);
	zram_set_flag(zram, index, ZRAM_HUGE);
	zram_set_handle(zram, index, handle);
	zram_set_obj_size(zram, index, PAGE_SIZE);
	zram_set_priority(zram, index, prio);
	zram_slot_unlock(zram, index);

	atomic64_add(PAGE_SIZE, &zram->stats.compr_data_size);
	atomic64_inc(&zram->stats.huge_pages);
	atomic64_inc(&zram->stats.huge_pages_since);
	atomic64_inc(&zram->stats.pages_stored);

	return 0;
}

static int zram_write_page(struct zram *zram, struct page *page, u32 index)
{
	int ret = 0;
	unsigned long handle;
	unsigned int comp_len;
	void *dst, *mem;
	struct zcomp_strm *zstrm = NULL;
	unsigned long element;
	bool same_filled;
	u8 prio, prio_max = zram->num_active_comps;
#ifdef CONFIG_ZRAM_MULTI_COMP
	prio_max = min(prio_max, sysctl_zram_recomp_immediate + 1);
#endif //CONFIG_ZRAM_MULTI_COMP

	/* First, free memory allocated to this slot (if any) */
	zram_slot_lock(zram, index);
	zram_free_page(zram, index);
	zram_slot_unlock(zram, index);


	mem = kmap_atomic(page);
	same_filled = page_same_filled(mem, &element);
	kunmap_atomic(mem);
	if (same_filled)
		return write_same_filled_page(zram, element, index);

	for (prio = ZRAM_PRIMARY_COMP; prio < prio_max; prio++) {
		if (!zram->comps[prio])
			continue;

		zstrm = zcomp_stream_get(zram->comps[prio]);
		mem = kmap_local_page(page);
		ret = zcomp_compress(zstrm, mem, &comp_len);
		kunmap_local(mem);

		if (unlikely(ret)) {
			pr_err("Compression failed! err=%d\n", ret);
			goto out;
		}

		if (comp_len < huge_class_size)
			break;

		zcomp_stream_put(zram->comps[prio]);
		zstrm = NULL;
	}

	if (!zstrm) {
		if (prio >= zram->num_active_comps) {
			zram_slot_lock(zram, index);
			zram_set_flag(zram, index, ZRAM_INCOMPRESSIBLE);
			zram_slot_unlock(zram, index);
		}

		ret = write_incompressible_page(zram, page, index, prio - 1);
		goto out;
	}

	handle = zs_malloc(zram->mem_pool, comp_len,
			__GFP_KSWAPD_RECLAIM |
			__GFP_NOWARN |
			__GFP_HIGHMEM |
			__GFP_MOVABLE |
			__GFP_CMA);
	if (IS_ERR_VALUE(handle)) {
		ret = PTR_ERR((void *)handle);
		goto out;
	}

	if (!zram_can_store_page(zram)) {
		zs_free(zram->mem_pool, handle);
		ret = -ENOMEM;
		goto out;
	}

	dst = zs_map_object(zram->mem_pool, handle, ZS_MM_WO);

	memcpy(dst, zstrm->buffer, comp_len);
	zs_unmap_object(zram->mem_pool, handle);

	zram_slot_lock(zram, index);
	zram_set_handle(zram, index, handle);
	zram_set_obj_size(zram, index, comp_len);
	zram_set_priority(zram, index, prio);
	zram_slot_unlock(zram, index);

	/* Update stats */
	atomic64_inc(&zram->stats.pages_stored);
	atomic64_add(comp_len, &zram->stats.compr_data_size);

out:
	if (zstrm)
		zcomp_stream_put(zram->comps[prio]);
	return ret;
}

/*
 * This is a partial IO. Read the full page before writing the changes.
 */
static int zram_bvec_write_partial(struct zram *zram, struct bio_vec *bvec,
				   u32 index, int offset, struct bio *bio)
{
	struct page *page = alloc_page(GFP_NOIO);
	int ret;

	if (!page)
		return -ENOMEM;

	ret = zram_read_page(zram, page, index, bio);
	if (!ret) {
		memcpy_from_bvec(page_address(page) + offset, bvec);
		ret = zram_write_page(zram, page, index);
	}
	__free_page(page);
	return ret;
}

static int zram_bvec_write(struct zram *zram, struct bio_vec *bvec,
			   u32 index, int offset, struct bio *bio)
{
	if (is_partial_io(bvec))
		return zram_bvec_write_partial(zram, bvec, index, offset, bio);
	return zram_write_page(zram, bvec->bv_page, index);
}

#ifdef CONFIG_ZRAM_MULTI_COMP
/*
 * This function will decompress (unless it's ZRAM_HUGE) the page and then
 * attempt to compress it using provided compression algorithm priority
 * (which is potentially more effective).
 *
 * Corresponding ZRAM slot should be locked.
 */
static int zram_recompress(struct zram *zram, u32 index, struct page *page,
			   u32 threshold, u32 prio, u32 prio_max)
{
	struct zcomp_strm *zstrm = NULL;
	unsigned long handle_old;
	unsigned long handle_new;
	unsigned int comp_len_old;
	unsigned int comp_len_new;
	unsigned int class_index_old;
	unsigned int class_index_new;
	u32 num_recomps = 0;
	void *src, *dst;
	int ret;

	handle_old = zram_get_handle(zram, index);
	if (!handle_old)
		return -EINVAL;

	comp_len_old = zram_get_obj_size(zram, index);
	/*
	 * Do not recompress objects that are already "small enough".
	 */
	if (comp_len_old < threshold)
		return 0;

	ret = zram_read_from_zspool(zram, page, index);
	if (ret)
		return ret;

	/*
	 * We touched this entry so mark it as non-IDLE. This makes sure that
	 * we don't preserve IDLE flag and don't incorrectly pick this entry
	 * for different post-processing type (e.g. writeback).
	 */
	zram_clear_flag(zram, index, ZRAM_IDLE);

	class_index_old = zs_lookup_class_index(zram->mem_pool, comp_len_old);
	/*
	 * Iterate the secondary comp algorithms list (in order of priority)
	 * and try to recompress the page.
	 */
	for (; prio < prio_max; prio++) {
		if (!zram->comps[prio])
			continue;

		/*
		 * Skip if the object is already re-compressed with a higher
		 * priority algorithm (or same algorithm).
		 */
		if (prio <= zram_get_priority(zram, index))
			continue;

		num_recomps++;
		zstrm = zcomp_stream_get(zram->comps[prio]);
		src = kmap_atomic(page);
		ret = zcomp_compress(zstrm, src, &comp_len_new);
		kunmap_atomic(src);

		if (ret) {
			zcomp_stream_put(zram->comps[prio]);
			return ret;
		}

		class_index_new = zs_lookup_class_index(zram->mem_pool,
							comp_len_new);

		/* Continue until we make progress */
		if (class_index_new >= class_index_old ||
		    (threshold && comp_len_new >= threshold)) {
			zcomp_stream_put(zram->comps[prio]);
			continue;
		}

		/* Recompression was successful so break out */
		break;
	}

	/*
	 * We did not try to recompress, e.g. when we have only one
	 * secondary algorithm and the page is already recompressed
	 * using that algorithm
	 */
	if (!zstrm)
		return 0;

	if (class_index_new >= class_index_old) {
		/*
		 * Secondary algorithms failed to re-compress the page
		 * in a way that would save memory, mark the object as
		 * incompressible so that we will not try to compress
		 * it again.
		 *
		 * We need to make sure that all secondary algorithms have
		 * failed, so we test if the number of recompressions matches
		 * the number of active secondary algorithms.
		 */
		if (num_recomps == zram->num_active_comps - 1)
			zram_set_flag(zram, index, ZRAM_INCOMPRESSIBLE);
		return 0;
	}

	/* Successful recompression but above threshold */
	if (threshold && comp_len_new >= threshold)
		return 0;

	/*
	 * No direct reclaim (slow path) for handle allocation and no
	 * re-compression attempt (unlike in zram_write_bvec()) since
	 * we already have stored that object in zsmalloc. If we cannot
	 * alloc memory for recompressed object then we bail out and
	 * simply keep the old (existing) object in zsmalloc.
	 */
	handle_new = zs_malloc(zram->mem_pool, comp_len_new,
			       __GFP_KSWAPD_RECLAIM |
			       __GFP_NOWARN |
			       __GFP_HIGHMEM |
			       __GFP_MOVABLE);
	if (IS_ERR_VALUE(handle_new)) {
		zcomp_stream_put(zram->comps[prio]);
		return PTR_ERR((void *)handle_new);
	}

	dst = zs_map_object(zram->mem_pool, handle_new, ZS_MM_WO);
	memcpy(dst, zstrm->buffer, comp_len_new);
	zcomp_stream_put(zram->comps[prio]);

	zs_unmap_object(zram->mem_pool, handle_new);

	zram_free_page(zram, index);
	zram_set_handle(zram, index, handle_new);
	zram_set_obj_size(zram, index, comp_len_new);
	zram_set_priority(zram, index, prio);

	atomic64_add(comp_len_new, &zram->stats.compr_data_size);
	atomic64_inc(&zram->stats.pages_stored);

	return 0;
}

#define RECOMPRESS_IDLE		(1 << 0)
#define RECOMPRESS_HUGE		(1 << 1)

static ssize_t recompress_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t len)
{
	u32 prio = ZRAM_SECONDARY_COMP, prio_max = ZRAM_MAX_COMPS;
	struct zram *zram = dev_to_zram(dev);
	unsigned long nr_pages = zram->disksize >> PAGE_SHIFT;
	char *args, *param, *val, *algo = NULL;
	u32 mode = 0, threshold = 0;
	unsigned long index;
	struct page *page;
	ssize_t ret;

	args = skip_spaces(buf);
	while (*args) {
		args = next_arg(args, &param, &val);

		if (!val || !*val)
			return -EINVAL;

		if (!strcmp(param, "type")) {
			if (!strcmp(val, "idle"))
				mode = RECOMPRESS_IDLE;
			if (!strcmp(val, "huge"))
				mode = RECOMPRESS_HUGE;
			if (!strcmp(val, "huge_idle"))
				mode = RECOMPRESS_IDLE | RECOMPRESS_HUGE;
			continue;
		}

		if (!strcmp(param, "threshold")) {
			/*
			 * We will re-compress only idle objects equal or
			 * greater in size than watermark.
			 */
			ret = kstrtouint(val, 10, &threshold);
			if (ret)
				return ret;
			continue;
		}

		if (!strcmp(param, "algo")) {
			algo = val;
			continue;
		}
	}

	if (threshold >= huge_class_size)
		return -EINVAL;

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		ret = -EINVAL;
		goto release_init_lock;
	}

	if (algo) {
		bool found = false;

		for (; prio < ZRAM_MAX_COMPS; prio++) {
			if (!zram->comp_algs[prio])
				continue;

			if (!strcmp(zram->comp_algs[prio], algo)) {
				prio_max = min(prio + 1, ZRAM_MAX_COMPS);
				found = true;
				break;
			}
		}

		if (!found) {
			ret = -EINVAL;
			goto release_init_lock;
		}
	}

	page = alloc_page(GFP_KERNEL);
	if (!page) {
		ret = -ENOMEM;
		goto release_init_lock;
	}

	ret = len;
	for (index = 0; index < nr_pages; index++) {
		int err = 0;

		zram_slot_lock(zram, index);

		if (!zram_allocated(zram, index))
			goto next;

		if (mode & RECOMPRESS_IDLE &&
		    !zram_test_flag(zram, index, ZRAM_IDLE))
			goto next;

		if (mode & RECOMPRESS_HUGE &&
		    !zram_test_flag(zram, index, ZRAM_HUGE))
			goto next;

		if (zram_test_flag(zram, index, ZRAM_WB) ||
		    zram_test_flag(zram, index, ZRAM_UNDER_WB) ||
		    zram_test_flag(zram, index, ZRAM_SAME) ||
		    zram_test_flag(zram, index, ZRAM_INCOMPRESSIBLE))
			goto next;

		err = zram_recompress(zram, index, page, threshold,
				      prio, prio_max);
next:
		zram_slot_unlock(zram, index);
		if (err) {
			ret = err;
			break;
		}

		cond_resched();
	}

	__free_page(page);

release_init_lock:
	up_read(&zram->init_lock);
	return ret;
}
#endif

static void zram_bio_discard(struct zram *zram, struct bio *bio)
{
	size_t n = bio->bi_iter.bi_size;
	u32 index = bio->bi_iter.bi_sector >> SECTORS_PER_PAGE_SHIFT;
	u32 offset = (bio->bi_iter.bi_sector & (SECTORS_PER_PAGE - 1)) <<
			SECTOR_SHIFT;

	/*
	 * zram manages data in physical block size units. Because logical block
	 * size isn't identical with physical block size on some arch, we
	 * could get a discard request pointing to a specific offset within a
	 * certain physical block.  Although we can handle this request by
	 * reading that physiclal block and decompressing and partially zeroing
	 * and re-compressing and then re-storing it, this isn't reasonable
	 * because our intent with a discard request is to save memory.  So
	 * skipping this logical block is appropriate here.
	 */
	if (offset) {
		if (n <= (PAGE_SIZE - offset))
			return;

		n -= (PAGE_SIZE - offset);
		index++;
	}

	while (n >= PAGE_SIZE) {
		zram_slot_lock(zram, index);
		zram_free_page(zram, index);
		zram_slot_unlock(zram, index);
		atomic64_inc(&zram->stats.notify_free);
		index++;
		n -= PAGE_SIZE;
	}

	bio_endio(bio);
}

static void zram_bio_read(struct zram *zram, struct bio *bio)
{
	unsigned long start_time = bio_start_io_acct(bio);
	struct bvec_iter iter = bio->bi_iter;

	do {
		u32 index = iter.bi_sector >> SECTORS_PER_PAGE_SHIFT;
		u32 offset = (iter.bi_sector & (SECTORS_PER_PAGE - 1)) <<
				SECTOR_SHIFT;
		struct bio_vec bv = bio_iter_iovec(bio, iter);

		bv.bv_len = min_t(u32, bv.bv_len, PAGE_SIZE - offset);

		if (zram_bvec_read(zram, &bv, index, offset, bio) < 0) {
			atomic64_inc(&zram->stats.failed_reads);
			bio->bi_status = BLK_STS_IOERR;
			break;
		}
		flush_dcache_page(bv.bv_page);

		zram_slot_lock(zram, index);
		zram_accessed(zram, index);
		zram_slot_unlock(zram, index);

		bio_advance_iter_single(bio, &iter, bv.bv_len);
	} while (iter.bi_size);

	bio_end_io_acct(bio, start_time);
	bio_endio(bio);
}

static void zram_bio_write(struct zram *zram, struct bio *bio)
{
	unsigned long start_time = bio_start_io_acct(bio);
	struct bvec_iter iter = bio->bi_iter;

	do {
		u32 index = iter.bi_sector >> SECTORS_PER_PAGE_SHIFT;
		u32 offset = (iter.bi_sector & (SECTORS_PER_PAGE - 1)) <<
				SECTOR_SHIFT;
		struct bio_vec bv = bio_iter_iovec(bio, iter);

		bv.bv_len = min_t(u32, bv.bv_len, PAGE_SIZE - offset);

		if (zram_bvec_write(zram, &bv, index, offset, bio) < 0) {
			atomic64_inc(&zram->stats.failed_writes);
			bio->bi_status = BLK_STS_IOERR;
			break;
		}

		zram_slot_lock(zram, index);
		zram_accessed(zram, index);
		zram_slot_unlock(zram, index);

		bio_advance_iter_single(bio, &iter, bv.bv_len);
	} while (iter.bi_size);

	bio_end_io_acct(bio, start_time);
	bio_endio(bio);
}

/*
 * Handler function for all zram I/O requests.
 */
static void zram_submit_bio(struct bio *bio)
{
	struct zram *zram = bio->bi_bdev->bd_disk->private_data;

	switch (bio_op(bio)) {
	case REQ_OP_READ:
		zram_bio_read(zram, bio);
		break;
	case REQ_OP_WRITE:
		zram_bio_write(zram, bio);
		break;
	case REQ_OP_DISCARD:
	case REQ_OP_WRITE_ZEROES:
		zram_bio_discard(zram, bio);
		break;
	default:
		WARN_ON_ONCE(1);
		bio_endio(bio);
	}
}

static void zram_slot_free_notify(struct block_device *bdev,
				unsigned long index)
{
	struct zram *zram;

	zram = bdev->bd_disk->private_data;

	atomic64_inc(&zram->stats.notify_free);
	if (!zram_slot_trylock(zram, index)) {
		atomic64_inc(&zram->stats.miss_free);
		return;
	}

	zram_free_page(zram, index);
	zram_slot_unlock(zram, index);
}

static void zram_destroy_comps(struct zram *zram)
{
	u32 prio;

	for (prio = 0; prio < ZRAM_MAX_COMPS; prio++) {
		struct zcomp *comp = zram->comps[prio];

		zram->comps[prio] = NULL;
		if (!comp)
			continue;
		zcomp_destroy(comp);
		zram->num_active_comps--;
	}

	for (prio = ZRAM_PRIMARY_COMP; prio < ZRAM_MAX_COMPS; prio++) {
		/* Do not free statically defined compression algorithms */
		if (zram->comp_algs[prio] != default_compressor)
			kfree(zram->comp_algs[prio]);
		zram->comp_algs[prio] = NULL;
	}
}

static void zram_reset_device(struct zram *zram)
{
	down_write(&zram->init_lock);

	zram->limit_pages = 0;

	set_capacity_and_notify(zram->disk, 0);
	part_stat_set_all(zram->disk->part0, 0);

	/* I/O operation under all of CPU are done so let's free */
	zram_meta_free(zram, zram->disksize);
	zram->disksize = 0;
	zram_destroy_comps(zram);
	memset(&zram->stats, 0, sizeof(zram->stats));
	reset_bdev(zram);

	comp_algorithm_set(zram, ZRAM_PRIMARY_COMP, default_compressor);
	up_write(&zram->init_lock);
}

static inline u64 _round_up(u64 num, u64 multiple) {
    if (multiple == 0) return num;
    u64 remainder = num % multiple;
    if (remainder == 0) return num;
    return num + multiple - remainder;
}

#ifdef CONFIG_ZRAM_AUTO_SIZE
u64 calculate_pressure_factor_log_slow_to_fast_kernel(u64 mem_pressure, u64 zram_pressure, u64 min_num, u64 max_num) {
    s32 pressure_diff = 0;
    s32 pressure_increase_log;
    s32 base_factor_log;
    s32 combined_factor_log;
    s32 scaling_factor_log;
    u64 combined_pressure_factor_percent;

    if (mem_pressure > 60) {
        pressure_diff += (s32)(mem_pressure - 60);
    }
    if (zram_pressure > 50) {
        pressure_diff += (s32)(zram_pressure - 50);
    }

    if (pressure_diff <= 0) {
        return min_num;
    }

    // 1. 将压力差转换为 log 格式
    pressure_increase_log = u64_to_log32fpmax((u64)pressure_diff);

    // 2.添加一个缩放因子来减缓增长
    scaling_factor_log = u64_to_log32fpmax(15ULL);  // log(15)
    pressure_increase_log = pressure_increase_log - scaling_factor_log;  // log(x) - log(15) = log(x/15)

    // 3. 将基础因子（100）转换为 log 格式
    base_factor_log = u64_to_log32fpmax(100ULL);

    // 4. 在 log 空间中相加，模拟线性空间的乘法
    combined_factor_log = base_factor_log + pressure_increase_log;

    // 5. 将结果从 log 格式转换回线性整数
    combined_pressure_factor_percent = log32fpmax_to_u64(combined_factor_log);

    // 6. 限制结果的范围
    combined_pressure_factor_percent = min(combined_pressure_factor_percent, max_num);
    if (combined_pressure_factor_percent < min_num) {
        combined_pressure_factor_percent = min_num;
    }

    return combined_pressure_factor_percent;
}
#endif

static ssize_t disksize_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	u64 disksize;
	struct zcomp *comp;
	struct zram *zram = dev_to_zram(dev);
	int err;
	u32 prio;

#ifdef CONFIG_ZRAM_AUTO_SIZE
	if (sysfs_streq(buf, "auto")) {
        unsigned long total_mem = (u64)totalram_pages() << PAGE_SHIFT; // 总物理内存
        unsigned int num_cores = num_online_cpus(); // 在线 CPU 核心数
        u64 base_ratio;
		u64 target_size;
        u64 combined_pressure_factor_percent;

        // 1. 优化 base_ratio 的计算：确保 8 核时达到 100%
        // 每个核心贡献 13%，上限 100%
        base_ratio = min(num_cores * 13ULL, 100ULL); 
        
        // 计算基于内存和核心数的初始目标大小
        target_size = div64_ul(total_mem * base_ratio, 100ULL);

        // 2. 引入历史内存和 Zram 压力因子
        spin_lock(&zram->pressure_lock);
        unsigned int mem_pressure = zram->historical_mem_pressure;
        unsigned int zram_pressure = zram->historical_zram_pressure;
        spin_unlock(&zram->pressure_lock);

        combined_pressure_factor_percent = 100ULL; // 默认压力因子为 100% (不增加也不减少)

        // 根据压力数据调整因子
    	combined_pressure_factor_percent = calculate_pressure_factor_log_slow_to_fast_kernel(mem_pressure, zram_pressure, 100ULL, 200ULL);

        // 应用压力因子
        target_size = div64_ul(target_size * combined_pressure_factor_percent, 100ULL);

        // 3. 向上取整到最近的 GB
        target_size = _round_up(target_size, 1ULL << 30);

        // 4. 调整 clamp 范围：允许 Zram 大小超过物理内存
        // 最小 Zram 1GB，或总内存的 1/8 （取两者最大值）
        u64 min_allowed_size = max(1ULL * 1024 * 1024 * 1024ULL, div64_ul(total_mem, 8ULL)); 
        // 最大 Zram 可以是 64GB，或总内存的 2 倍（取两者最小值）
        u64 max_allowed_size = min(64ULL * 1024 * 1024 * 1024ULL, total_mem * 2ULL); 
        
        // 确保最小不会超过最大
        if (min_allowed_size > max_allowed_size) {
            min_allowed_size = max_allowed_size; 
        }

        target_size = clamp(target_size, min_allowed_size, max_allowed_size);
        
        pr_info("zram: auto-calculated disksize: %llu (mem: %luGB, cores: %u, pressure: %u:%u, factor: %llu%%)\n",
                target_size, total_mem >> 30, num_cores, mem_pressure, zram_pressure, combined_pressure_factor_percent);

		disksize = target_size;
    } else {
        // 用户手动设置 Zram 大小
        disksize  = memparse(buf, NULL);
	}
#else
	disksize = memparse(buf, NULL);
#endif

	if (!disksize)
		return -EINVAL;

	down_write(&zram->init_lock);
	if (init_done(zram)) {
		pr_info("Cannot change disksize for initialized device\n");
		err = -EBUSY;
		goto out_unlock;
	}

	disksize = PAGE_ALIGN(disksize);
	if (!zram_meta_alloc(zram, disksize)) {
		err = -ENOMEM;
		goto out_unlock;
	}

	for (prio = 0; prio < ZRAM_MAX_COMPS; prio++) {
		if (!zram->comp_algs[prio])
			continue;

		comp = zcomp_create(zram->comp_algs[prio]);
		if (IS_ERR(comp)) {
			pr_err("Cannot initialise %s compressing backend\n",
			       zram->comp_algs[prio]);
			err = PTR_ERR(comp);
			goto out_free_comps;
		}

		zram->comps[prio] = comp;
		zram->num_active_comps++;
	}
	zram->disksize = disksize;
	set_capacity_and_notify(zram->disk, zram->disksize >> SECTOR_SHIFT);
	up_write(&zram->init_lock);

	return len;

out_free_comps:
	zram_destroy_comps(zram);
	zram_meta_free(zram, disksize);
out_unlock:
	up_write(&zram->init_lock);
	return err;
}

static ssize_t reset_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t len)
{
	int ret;
	unsigned short do_reset;
	struct zram *zram;
	struct gendisk *disk;

	ret = kstrtou16(buf, 10, &do_reset);
	if (ret)
		return ret;

	if (!do_reset)
		return -EINVAL;

	zram = dev_to_zram(dev);
	disk = zram->disk;

	mutex_lock(&disk->open_mutex);
	/* Do not reset an active device or claimed device */
	if (disk_openers(disk) || zram->claim) {
		mutex_unlock(&disk->open_mutex);
		return -EBUSY;
	}

	/* From now on, anyone can't open /dev/zram[0-9] */
	zram->claim = true;
	mutex_unlock(&disk->open_mutex);

	/* Make sure all the pending I/O are finished */
	sync_blockdev(disk->part0);
	zram_reset_device(zram);

	mutex_lock(&disk->open_mutex);
	zram->claim = false;
	mutex_unlock(&disk->open_mutex);

	return len;
}

static int zram_open(struct gendisk *disk, blk_mode_t mode)
{
	struct zram *zram = disk->private_data;

	WARN_ON(!mutex_is_locked(&disk->open_mutex));

	/* zram was claimed to reset so open request fails */
	if (zram->claim)
		return -EBUSY;
	return 0;
}

static const struct block_device_operations zram_devops = {
	.open = zram_open,
	.submit_bio = zram_submit_bio,
	.swap_slot_free_notify = zram_slot_free_notify,
	.owner = THIS_MODULE
};

#ifdef CONFIG_ZRAM_AUTO_SIZE
static ssize_t pressure_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct zram *zram;
    unsigned int mem_pressure, zram_pressure;
    
    // 参数合法性检查
    if (!dev || !buf) {
        pr_err("zram: invalid parameters in pressure_show\n");
        return -EINVAL;
    }
    
    zram = dev_to_zram(dev);
    if (!zram) {
        pr_warn("zram: zram device data is NULL\n");
        return scnprintf(buf, PAGE_SIZE, "NULL:NULL\n");
    }
    
    // 在读取时也需要加锁，确保数据一致性
    spin_lock(&zram->pressure_lock);
    mem_pressure = zram->historical_mem_pressure;
    zram_pressure = zram->historical_zram_pressure;
    spin_unlock(&zram->pressure_lock);
    
    // 使用 scnprintf 而不是 sprintf，更安全
    return scnprintf(buf, PAGE_SIZE, "%u:%u\n", mem_pressure, zram_pressure);
}

static ssize_t pressure_store(struct device *dev, struct device_attribute *attr,
                               const char *buf, size_t len)
{
    struct zram *zram = dev_to_zram(dev);
    unsigned int mem_pressure, zram_pressure;
    char *colon_pos;
    char *local_buf;
    int ret = -EINVAL;

    pr_debug("zram: pressure_store called with len=%zu, buf='%.*s'\n", 
             len, (int)len, buf);

    // 参数合法性检查
    if (!buf || len == 0) {
        pr_err("zram: invalid buffer parameters\n");
        return -EINVAL;
    }
    
    if (!zram) {
        pr_err("zram: zram device data is NULL\n");
        return -ENODEV;
    }

    // 分配本地缓冲区，避免修改原始buf
    local_buf = kmalloc(len + 1, GFP_KERNEL);
    if (!local_buf) {
        pr_err("zram: memory allocation failed\n");
        return -ENOMEM;
    }

    // 复制数据并确保字符串终止
    memcpy(local_buf, buf, len);
    local_buf[len] = '\0';

    // 移除可能的换行符
    if (len > 0 && local_buf[len - 1] == '\n') {
        local_buf[len - 1] = '\0';
    }

    // 查找冒号分隔符
    colon_pos = strchr(local_buf, ':');
    if (!colon_pos) {
        pr_err("zram: invalid pressure format, expected 'mem_pressure:zram_pressure'\n");
        ret = -EINVAL;
        goto out;
    }

    // 分割字符串
    *colon_pos = '\0';

    // 解析内存压力
    ret = kstrtouint(local_buf, 10, &mem_pressure);
    if (ret) {
        pr_err("zram: invalid memory pressure value\n");
        goto out;
    }

    // 解析 Zram 压力
    ret = kstrtouint(colon_pos + 1, 10, &zram_pressure);
    if (ret) {
        pr_err("zram: invalid zram pressure value\n");
        goto out;
    }

    // 限制压力值在 0-100 之间
    mem_pressure = clamp(mem_pressure, 0U, 100U);
    zram_pressure = clamp(zram_pressure, 0U, 100U);

    spin_lock(&zram->pressure_lock);
    zram->historical_mem_pressure = mem_pressure;
    zram->historical_zram_pressure = zram_pressure;
    spin_unlock(&zram->pressure_lock);

    pr_debug("zram: updated historical pressure to %u:%u\n", mem_pressure, zram_pressure);
    
    ret = len;

out:
    kfree(local_buf);
    return ret;
}
#endif

static DEVICE_ATTR_WO(compact);
static DEVICE_ATTR_RW(disksize);
static DEVICE_ATTR_RO(initstate);
static DEVICE_ATTR_WO(reset);
static DEVICE_ATTR_WO(mem_limit);
static DEVICE_ATTR_WO(mem_used_max);
static DEVICE_ATTR_WO(idle);
static DEVICE_ATTR_RW(max_comp_streams);
static DEVICE_ATTR_RW(comp_algorithm);
#ifdef CONFIG_ZRAM_WRITEBACK
static DEVICE_ATTR_RW(backing_dev);
static DEVICE_ATTR_WO(writeback);
static DEVICE_ATTR_RW(writeback_limit);
static DEVICE_ATTR_RW(writeback_limit_enable);
#endif
#ifdef CONFIG_ZRAM_MULTI_COMP
static DEVICE_ATTR_RW(recomp_algorithm);
static DEVICE_ATTR_WO(recompress);
#endif
#ifdef CONFIG_ZRAM_AUTO_SIZE
static DEVICE_ATTR_RW(pressure);
#endif

static struct attribute *zram_disk_attrs[] = {
	&dev_attr_disksize.attr,
	&dev_attr_initstate.attr,
	&dev_attr_reset.attr,
	&dev_attr_compact.attr,
	&dev_attr_mem_limit.attr,
	&dev_attr_mem_used_max.attr,
	&dev_attr_idle.attr,
	&dev_attr_max_comp_streams.attr,
	&dev_attr_comp_algorithm.attr,
#ifdef CONFIG_ZRAM_WRITEBACK
	&dev_attr_backing_dev.attr,
	&dev_attr_writeback.attr,
	&dev_attr_writeback_limit.attr,
	&dev_attr_writeback_limit_enable.attr,
#endif
	&dev_attr_io_stat.attr,
	&dev_attr_mm_stat.attr,
#ifdef CONFIG_ZRAM_WRITEBACK
	&dev_attr_bd_stat.attr,
#endif
	&dev_attr_debug_stat.attr,
#ifdef CONFIG_ZRAM_MULTI_COMP
	&dev_attr_recomp_algorithm.attr,
	&dev_attr_recompress.attr,
#endif
#ifdef CONFIG_ZRAM_AUTO_SIZE
	&dev_attr_pressure.attr,
#endif
	NULL,
};

ATTRIBUTE_GROUPS(zram_disk);

/*
 * Allocate and initialize new zram device. the function returns
 * '>= 0' device_id upon success, and negative value otherwise.
 */
static int zram_add(void)
{
	struct zram *zram;
	int ret, device_id;
	unsigned long total_mem = (u64)totalram_pages() << PAGE_SHIFT; // 总物理内存
	u64 default_disksize = _round_up(total_mem, 1ULL << 30);

	zram = kzalloc(sizeof(struct zram), GFP_KERNEL);
	if (!zram)
		return -ENOMEM;

	ret = idr_alloc(&zram_index_idr, zram, 0, 0, GFP_KERNEL);
	if (ret < 0)
		goto out_free_dev;
	device_id = ret;

	init_rwsem(&zram->init_lock);
#ifdef CONFIG_ZRAM_WRITEBACK
	spin_lock_init(&zram->wb_limit_lock);
#endif

	/* gendisk structure */
	zram->disk = blk_alloc_disk(NUMA_NO_NODE);
	if (!zram->disk) {
		pr_err("Error allocating disk structure for device %d\n",
			device_id);
		ret = -ENOMEM;
		goto out_free_idr;
	}

	zram->disk->major = zram_major;
	zram->disk->first_minor = device_id;
	zram->disk->minors = 1;
	zram->disk->flags |= GENHD_FL_NO_PART;
	zram->disk->fops = &zram_devops;
	zram->disk->private_data = zram;

#ifdef CONFIG_ZRAM_AUTO_SIZE
	spin_lock_init(&zram->pressure_lock);
	zram->historical_mem_pressure = 0; 
	zram->historical_zram_pressure = 0;
#endif

	snprintf(zram->disk->disk_name, 16, "zram%d", device_id);

	comp_algorithm_set(zram, ZRAM_PRIMARY_COMP, default_compressor);

	/* Actual capacity set using sysfs (/sys/block/zram<id>/disksize */
	set_capacity(zram->disk, 0);

	down_write(&zram->init_lock);
	if (!zram_meta_alloc(zram, default_disksize)) {
		up_write(&zram->init_lock);
		ret = -ENOMEM;
		goto out_cleanup_disk;
	}

	for (u32 prio = 0; prio < ZRAM_MAX_COMPS; prio++) {
		if (!zram->comp_algs[prio])
			continue;

		struct zcomp *comp = zcomp_create(zram->comp_algs[prio]);
		if (IS_ERR(comp)) {
			pr_err("Cannot initialise %s compressing backend\n",
				zram->comp_algs[prio]);
			zram_destroy_comps(zram); // 清理已创建的压缩器
			zram_meta_free(zram, default_disksize); // 释放元数据
			up_write(&zram->init_lock);
			ret = PTR_ERR(comp);
			goto out_cleanup_disk;
		}
		zram->comps[prio] = comp;
		zram->num_active_comps++;
	}

	zram->disksize = default_disksize;
	set_capacity_and_notify(zram->disk, zram->disksize >> SECTOR_SHIFT);
	#ifdef CONFIG_ZRAM_WRITEBACK
	zram_init_shrinker(zram);
	if (!zram->zram_shrinker)
		goto out_cleanup_disk;
	if (list_lru_init_memcg(&zram->zram_list_lru, zram->zram_shrinker))
		goto lru_fail;
	#endif /* CONFIG_ZRAM_WRITEBACK */
	up_write(&zram->init_lock);

	/* zram devices sort of resembles non-rotational disks */
	blk_queue_flag_set(QUEUE_FLAG_NONROT, zram->disk->queue);
	blk_queue_flag_set(QUEUE_FLAG_SYNCHRONOUS, zram->disk->queue);

	/*
	 * To ensure that we always get PAGE_SIZE aligned
	 * and n*PAGE_SIZED sized I/O requests.
	 */
	blk_queue_physical_block_size(zram->disk->queue, PAGE_SIZE);
	blk_queue_logical_block_size(zram->disk->queue,
					ZRAM_LOGICAL_BLOCK_SIZE);
	blk_queue_io_min(zram->disk->queue, PAGE_SIZE);
	blk_queue_io_opt(zram->disk->queue, PAGE_SIZE);
	zram->disk->queue->limits.discard_granularity = PAGE_SIZE;
	blk_queue_max_discard_sectors(zram->disk->queue, UINT_MAX);

	/*
	 * zram_bio_discard() will clear all logical blocks if logical block
	 * size is identical with physical block size(PAGE_SIZE). But if it is
	 * different, we will skip discarding some parts of logical blocks in
	 * the part of the request range which isn't aligned to physical block
	 * size.  So we can't ensure that all discarded logical blocks are
	 * zeroed.
	 */
	if (ZRAM_LOGICAL_BLOCK_SIZE == PAGE_SIZE)
		blk_queue_max_write_zeroes_sectors(zram->disk->queue, UINT_MAX);

	blk_queue_flag_set(QUEUE_FLAG_STABLE_WRITES, zram->disk->queue);
	ret = device_add_disk(NULL, zram->disk, zram_disk_groups);
	if (ret)
		goto out_cleanup_disk;

	zram_debugfs_register(zram);
	pr_info("Added device: %s with default size %llu bytes\n", zram->disk->disk_name, default_disksize);
	return device_id;
lru_fail:
	unregister_shrinker(zram->zram_shrinker);
	shrinker_free(zram->zram_shrinker);
out_cleanup_disk:
	put_disk(zram->disk);
out_free_idr:
	idr_remove(&zram_index_idr, device_id);
out_free_dev:
	kfree(zram);
	return ret;
}

static int zram_remove(struct zram *zram)
{
	bool claimed;

	mutex_lock(&zram->disk->open_mutex);
	if (disk_openers(zram->disk)) {
		mutex_unlock(&zram->disk->open_mutex);
		return -EBUSY;
	}

	claimed = zram->claim;
	if (!claimed)
		zram->claim = true;
	mutex_unlock(&zram->disk->open_mutex);

	zram_debugfs_unregister(zram);

	if (claimed) {
		/*
		 * If we were claimed by reset_store(), del_gendisk() will
		 * wait until reset_store() is done, so nothing need to do.
		 */
		;
	} else {
		/* Make sure all the pending I/O are finished */
		sync_blockdev(zram->disk->part0);
		zram_reset_device(zram);
	}

	pr_info("Removed device: %s\n", zram->disk->disk_name);

	del_gendisk(zram->disk);

	/* del_gendisk drains pending reset_store */
	WARN_ON_ONCE(claimed && zram->claim);

	/*
	 * disksize_store() may be called in between zram_reset_device()
	 * and del_gendisk(), so run the last reset to avoid leaking
	 * anything allocated with disksize_store()
	 */
	zram_reset_device(zram);

	// 释放zram_shrinker和相关资源
	#ifdef CONFIG_ZRAM_WRITEBACK
	if (zram->zram_shrinker) {
        unregister_shrinker(zram->zram_shrinker);
        shrinker_free(zram->zram_shrinker);
    }
    list_lru_destroy(&zram->zram_list_lru);
	#endif

	put_disk(zram->disk);
	kfree(zram);
	return 0;
}

/* zram-control sysfs attributes */

/*
 * NOTE: hot_add attribute is not the usual read-only sysfs attribute. In a
 * sense that reading from this file does alter the state of your system -- it
 * creates a new un-initialized zram device and returns back this device's
 * device_id (or an error code if it fails to create a new device).
 */
static ssize_t hot_add_show(const struct class *class,
			const struct class_attribute *attr,
			char *buf)
{
	int ret;

	mutex_lock(&zram_index_mutex);
	ret = zram_add();
	mutex_unlock(&zram_index_mutex);

	if (ret < 0)
		return ret;
	return scnprintf(buf, PAGE_SIZE, "%d\n", ret);
}
/* This attribute must be set to 0400, so CLASS_ATTR_RO() can not be used */
static struct class_attribute class_attr_hot_add =
	__ATTR(hot_add, 0400, hot_add_show, NULL);

static ssize_t hot_remove_store(const struct class *class,
			const struct class_attribute *attr,
			const char *buf,
			size_t count)
{
	struct zram *zram;
	int ret, dev_id;

	/* dev_id is gendisk->first_minor, which is `int' */
	ret = kstrtoint(buf, 10, &dev_id);
	if (ret)
		return ret;
	if (dev_id < 0)
		return -EINVAL;

	mutex_lock(&zram_index_mutex);

	zram = idr_find(&zram_index_idr, dev_id);
	if (zram) {
		ret = zram_remove(zram);
		if (!ret)
			idr_remove(&zram_index_idr, dev_id);
	} else {
		ret = -ENODEV;
	}

	mutex_unlock(&zram_index_mutex);
	return ret ? ret : count;
}
static CLASS_ATTR_WO(hot_remove);

static struct attribute *zram_control_class_attrs[] = {
	&class_attr_hot_add.attr,
	&class_attr_hot_remove.attr,
	NULL,
};
ATTRIBUTE_GROUPS(zram_control_class);

static struct class zram_control_class = {
	.name		= "zram-control",
	.class_groups	= zram_control_class_groups,
};

static int zram_remove_cb(int id, void *ptr, void *data)
{
	WARN_ON_ONCE(zram_remove(ptr));
	return 0;
}

static void destroy_devices(void)
{
	class_unregister(&zram_control_class);
	idr_for_each(&zram_index_idr, &zram_remove_cb, NULL);
	zram_debugfs_destroy();
	idr_destroy(&zram_index_idr);
	unregister_blkdev(zram_major, "zram");
	cpuhp_remove_multi_state(CPUHP_ZCOMP_PREPARE);
}

#ifdef CONFIG_ZRAM_WRITEBACK
// 紧急回写
static void __maybe_unused zram_writeback(struct zram *zram)
{
	struct device_attribute attr = {};
	char *buf_write_back = "idle";
	size_t len_write_back = strlen(buf_write_back) + 1;

	/* Simulate sysfs write to trigger writeback */
	down_read(&zram->init_lock);
	if (!init_done(zram) || !zram->backing_dev) {
		up_read(&zram->init_lock);
		pr_debug("Writeback skipped: device not initialized or no backing device\n");
		return;
	}

	mark_idle(zram, 0);
	writeback_store(disk_to_dev(zram->disk), &attr, buf_write_back, len_write_back);
	up_read(&zram->init_lock);
}

// 计算zram使用率
static unsigned long get_zram_usage(struct zram *zram)
{
	u64 pages_stored, total_pages, bd_count;

	down_read(&zram->init_lock);
	if (!init_done(zram)) {
		up_read(&zram->init_lock);
		return 0;
	}
	
	pages_stored = atomic64_read(&zram->stats.pages_stored);
	bd_count = atomic64_read(&zram->stats.bd_count);
	total_pages = zram->disksize >> PAGE_SHIFT;
	up_read(&zram->init_lock);

	if (total_pages == 0)
		return 0;

	if (pages_stored <= bd_count)
		return 0;
		
	return ((pages_stored - bd_count) * 100) / total_pages;
}

// 计算内存占用率
static int get_memory_usage(void)
{
    struct sysinfo si;
    si_meminfo(&si);

    unsigned long total = si.totalram;
    unsigned long free = si_mem_available();

    if (total == 0)
        return 0;

    return ((total - free) * 100) / total;
}

// 监控线程函数
static int monitor_func(void *data)
{
	struct zram *zram;
	int id;
	int zram_count = 0;
	int exec_count = 0;
	ktime_t cutoff_time = 0;
	unsigned long avg_zram_usage = 0;
	unsigned long total_zram_usage = 0;
	u64 combined_pressure_factor_percent;

	if (IS_ENABLED(CONFIG_ZRAM_TRACK_ENTRY_ACTIME))
		cutoff_time = ktime_sub(ktime_get_boottime(), ns_to_ktime(30 * NSEC_PER_SEC));

    while (!kthread_should_stop()) {
		int mem_usage = get_memory_usage();
		total_zram_usage = 0;
		zram_count = 0;
		
		mutex_lock(&zram_index_mutex);
		idr_for_each_entry(&zram_index_idr, zram, id) {
			unsigned long zram_usage = get_zram_usage(zram);
			total_zram_usage += zram_usage;
			zram_count++;

			down_read(&zram->init_lock);
			// 如果标记5分钟甚至是1分钟进入空闲,我们标记不到任何页面,而30秒会标记大约1/6的页面,是比较理想的状态
			if (cutoff_time != 0)
				mark_idle(zram, cutoff_time);
			up_read(&zram->init_lock);
			// 作为杀后台前的紧急回写,游戏中不应该触发,防止回写导致的卡顿
        	//if (mem_usage > MEM_THRESHOLD && check_game_pid()) {
            //	pr_info("Thresholds exceeded, triggering writeback!!!\n");
            //	zram_writeback(zram);
			//}
			//禁用,因为杀后台是多因素的,这样可能没有收益还导致大量页面被回写导致问题
		}
		mutex_unlock(&zram_index_mutex);
		
		// 计算平均压力
		if (zram_count > 0) {
			avg_zram_usage = total_zram_usage / zram_count;
		} else {
			avg_zram_usage = 0;
		}

		exec_count++;
		if (exec_count == 10) {
			combined_pressure_factor_percent = calculate_pressure_factor_log_slow_to_fast_kernel(mem_usage, avg_zram_usage, 50ULL, 200ULL);
			batch_size = div64_ul(512 * combined_pressure_factor_percent, 100ULL);
			pr_info("combined_pressure_factor_percent=%llu, batch_size=%llu\n", combined_pressure_factor_percent, batch_size);
			exec_count = 0;
		}
		
		pr_info("zram_count=%d, avg_zram_usage=%lu%%, mem_usage=%d%%\n",
			zram_count, avg_zram_usage, mem_usage);

        schedule_timeout_interruptible(CHECK_INTERVAL);
    }

    return 0;
}

static enum lru_status zram_shrink_cb(struct list_head *item, struct list_lru_one *l,
                                      spinlock_t *lock, void *arg)
{
    struct zram_table_entry *entry = container_of(item, struct zram_table_entry, lru);
    struct zram_shrink_ctx *ctx = (struct zram_shrink_ctx *)arg;
    struct zram *zram = ctx->zram;
    bool *encountered_in_swapcache = ctx->encountered_in_swapcache;
    int swapcache_count = 0;
    enum lru_status ret = LRU_REMOVED_RETRY;
    int writeback_result;
    u32 index;

	/*
	 * Second chance algorithm: if the entry has its referenced bit set, give it
	 * a second chance. Only clear the referenced bit and rotate it in the
	 * zram's LRU list.
	 */
	if (entry->referenced) {
		entry->referenced = false;
		return LRU_ROTATE;
	}

    if (entry->flags & BIT(ZRAM_LOCK)) {
        return LRU_SKIP;
    }

    /*
     * As soon as we drop the LRU lock, the entry can be freed by
     * a concurrent invalidation. This means the following:
     *
     * 1. We don't need to extract any data to the stack as in zswap
     *    because we process the entry in-place.
     *
     * 2. Usually, objects are taken off the LRU for reclaim. In
     *    this case this isn't possible, because if reclaim fails
     *    for whatever reason, we have no means of knowing if the
     *    entry is alive to put it back on the LRU.
     *
     *    So rotate it before dropping the lock. If the entry is
     *    written back or invalidated, the free path will unlink
     *    it. For failures, rotation is the right thing as well.
     *
     *    Temporary failures, where the same entry should be tried
     *    again immediately, almost never happen for this shrinker.
     *    We don't do any trylocking; -ENOMEM comes closest,
     *    but that's extremely rare and doesn't happen spuriously
     *    either. Don't bother distinguishing this case.
     */
    list_move_tail(item, &l->list);

    /*
     * It's safe to drop the lock here because we return either
     * LRU_REMOVED_RETRY, LRU_RETRY or LRU_STOP.
     */
    spin_unlock(lock);

    /* Find the index of this entry */
    index = entry - zram->table;
    
    /* Try to writeback the entry */
    writeback_result = zram_writeback_entry(zram, entry);

    if (writeback_result) {
        atomic64_inc(&zram->stats.reject_reclaim_fail);
        ret = LRU_RETRY;
    } else {
        atomic64_inc(&zram->stats.written_back_pages);
    }

	if (writeback_result == -EEXIST)
		swapcache_count++;
	
	if (swapcache_count >= 50) {
		ret = LRU_STOP;
		*encountered_in_swapcache = true;
	}

    spin_lock(lock);
    return ret;
}

static int zram_writeback_entry(struct zram *zram, struct zram_table_entry *entry)
{
    unsigned long handle;
    unsigned long flags;
    u16 size;
    struct page *page;
    unsigned long blk_idx;
    struct bio bio;
    struct bio_vec bio_vec;
    int err;
    u32 index;

    /* 
     * Check if we have a backing dev.
     * If not, we can't writeback this entry.
     */
    if (!zram->backing_dev) {
        return -ENODEV;
    }

    /* Find the index of this entry */
    index = entry - zram->table;

    zram_slot_lock(zram, index);
    
    /* Check if the slot is allocated */
    if (!zram_allocated(zram, index))
        goto next_unlock;

    /* If it's already written back, skip */
    if (zram_test_flag(zram, index, ZRAM_WB))
        goto next_unlock;

    /* If it's a same page (all bytes are the same), we don't writeback */
    if (zram_test_flag(zram, index, ZRAM_SAME))
        goto next_unlock;

    /* If it's under writeback, skip */
    if (zram_test_flag(zram, index, ZRAM_UNDER_WB))
        goto next_unlock;
        
    /* If it's incompressible, handle it */
    if (zram_test_flag(zram, index, ZRAM_INCOMPRESSIBLE))
        goto next_unlock;

    /*
     * Clearing ZRAM_UNDER_WB is duty of caller.
     * IOW, zram_free_page never clear it.
     */
    zram_set_flag(zram, index, ZRAM_UNDER_WB);
    /* Need for hugepage writeback racing */
    zram_set_flag(zram, index, ZRAM_IDLE);
    zram_slot_unlock(zram, index);
    
    handle = entry->handle;
    flags = entry->flags;
    size = (flags & (BIT(ZRAM_FLAG_SHIFT) - 1));

    /*
     * For now, we only writeback pages that are stored in the zsmalloc pool.
     */
    if (!(flags & BIT(ZRAM_HUGE)) && handle) {
        /* Allocate a page to decompress into */
        page = alloc_page(GFP_KERNEL);
        if (!page) {
            zram_slot_lock(zram, index);
            zram_clear_flag(zram, index, ZRAM_UNDER_WB);
            zram_clear_flag(zram, index, ZRAM_IDLE);
            zram_slot_unlock(zram, index);
            return -ENOMEM;
        }

        /* Read and decompress the page */
        if (zram_read_page(zram, page, index, NULL)) {
            zram_slot_lock(zram, index);
            zram_clear_flag(zram, index, ZRAM_UNDER_WB);
            zram_clear_flag(zram, index, ZRAM_IDLE);
            zram_slot_unlock(zram, index);
            __free_page(page);
            return -EIO;
        }

        /* Check writeback limits */
        spin_lock(&zram->wb_limit_lock);
        if (zram->wb_limit_enable && !zram->bd_wb_limit) {
            spin_unlock(&zram->wb_limit_lock);
            zram_slot_lock(zram, index);
            zram_clear_flag(zram, index, ZRAM_UNDER_WB);
            zram_clear_flag(zram, index, ZRAM_IDLE);
            zram_slot_unlock(zram, index);
            __free_page(page);
            return -EIO;
        }
        spin_unlock(&zram->wb_limit_lock);

        /* Allocate a block on the backing device */
        blk_idx = alloc_block_bdev(zram);
        if (!blk_idx) {
            zram_slot_lock(zram, index);
            zram_clear_flag(zram, index, ZRAM_UNDER_WB);
            zram_clear_flag(zram, index, ZRAM_IDLE);
            zram_slot_unlock(zram, index);
            __free_page(page);
            return -ENOSPC;
        }

        /* Prepare and submit the bio for writing to backing device */
        bio_init(&bio, zram->bdev, &bio_vec, 1, REQ_OP_WRITE | REQ_SYNC);
        bio.bi_iter.bi_sector = blk_idx * (PAGE_SIZE >> 9);
        __bio_add_page(&bio, page, PAGE_SIZE, 0);

        err = submit_bio_wait(&bio);
        __free_page(page);

        if (err) {
            zram_slot_lock(zram, index);
            zram_clear_flag(zram, index, ZRAM_UNDER_WB);
            zram_clear_flag(zram, index, ZRAM_IDLE);
            zram_slot_unlock(zram, index);
            free_block_bdev(zram, blk_idx);
            return -EIO;
        }

        /* Successfully written back, now free the zsmalloc entry */
        zram_slot_lock(zram, index);
        /*
         * We released zram_slot_lock so need to check if the slot was
         * changed. If there is freeing for the slot, we can catch it
         * easily by zram_allocated.
         * A subtle case is the slot is freed/reallocated/marked as
         * ZRAM_IDLE again. To close the race, idle_store doesn't
         * mark ZRAM_IDLE once it found the slot was ZRAM_UNDER_WB.
         * Thus, we could close the race by checking ZRAM_IDLE bit.
         */
        if (!zram_allocated(zram, index) ||
            !zram_test_flag(zram, index, ZRAM_IDLE)) {
            zram_clear_flag(zram, index, ZRAM_UNDER_WB);
            zram_clear_flag(zram, index, ZRAM_IDLE);
            zram_slot_unlock(zram, index);
            free_block_bdev(zram, blk_idx);
            return -EEXIST;
        }

        zs_free(zram->mem_pool, handle);
        
        /* Update entry to mark it as written back */
        zram_clear_flag(zram, index, ZRAM_UNDER_WB);
        zram_set_flag(zram, index, ZRAM_WB);
        zram_set_handle(zram, index, blk_idx);
        
        /* Update stats */
		atomic64_sub(size, &zram->stats.compr_data_size);
        atomic64_inc(&zram->stats.bd_writes);
        spin_lock(&zram->wb_limit_lock);
        if (zram->wb_limit_enable && zram->bd_wb_limit > 0)
            zram->bd_wb_limit -= 1UL << (PAGE_SHIFT - 12);
        spin_unlock(&zram->wb_limit_lock);
        /* Remove the entry from LRU list after writeback */
        zram_lru_del(zram, entry);
        zram_clear_flag(zram, index, ZRAM_IDLE);
        
        zram_slot_unlock(zram, index);
        return 0;
    } else if (flags & BIT(ZRAM_HUGE)) {
        /* Handle huge pages - currently not supported for writeback */
        zram_slot_lock(zram, index);
        zram_clear_flag(zram, index, ZRAM_UNDER_WB);
        zram_clear_flag(zram, index, ZRAM_IDLE);
        zram_slot_unlock(zram, index);
        return 0;
    } else if (flags & BIT(ZRAM_SAME)) {
        /* Same pages don't need writeback */
        zram_slot_lock(zram, index);
        zram_clear_flag(zram, index, ZRAM_UNDER_WB);
        zram_clear_flag(zram, index, ZRAM_IDLE);
        zram_slot_unlock(zram, index);
        return 0;
    }

next_unlock:
    zram_slot_unlock(zram, index);
    return -EEXIST;
}


static unsigned long zram_shrinker_count(struct shrinker *shrinker, struct shrink_control *sc)
{
	struct zram *zram = shrinker->private_data;
	unsigned long nr_stored, nr_backing, nr_freeable;

	/* Only enable shrinker when writeback is enabled */
	if (!zram->backing_dev) {
		return 0;
	}

	/*
	 * The shrinker resumes swap writeback, which will enter block
	 * and may enter fs. XXX: Harmonize with vmscan.c __GFP_FS
	 * rules (may_enter_fs()), which apply on a per-page basis.
	 */
	if (!gfp_has_io_fs(sc->gfp_mask)) {
		return 0;
	}

	nr_stored = atomic64_read(&zram->stats.pages_stored);
	nr_backing = atomic64_read(&zram->stats.compr_data_size) >> PAGE_SHIFT;

	if (!nr_stored)
		return 0;

	nr_freeable = list_lru_shrink_count(&zram->zram_list_lru, sc);
	
	if (!nr_freeable) {
		return 0;
	}

	/*
	 * Scale the number of freeable pages by the memory saving factor.
	 * This ensures that the better zram compresses memory, the fewer
	 * pages we will evict to swap (as it will otherwise incur IO for
	 * relatively small memory saving).
	 */
	return mult_frac(nr_freeable, nr_backing, nr_stored);
}

static unsigned long zram_shrinker_scan(struct shrinker *shrinker, struct shrink_control *sc)
{
	struct zram *zram = shrinker->private_data;
	unsigned long shrink_ret;
	bool encountered_in_swapcache = false;
	struct zram_shrink_ctx ctx = {
		.zram = zram,
		.encountered_in_swapcache = &encountered_in_swapcache,
	};

	sc->nr_to_scan = batch_size;

	pr_info("shrinker_scan: starting for zram device %s, nr_to_scan=%lu, gfp_mask=0x%x\n",
		 zram->disk->disk_name, sc->nr_to_scan, sc->gfp_mask);

	/* Only enable shrinker when writeback is enabled */
	if (!zram->backing_dev) {
		sc->nr_scanned = 0;
		return SHRINK_STOP;
	}

	shrink_ret = list_lru_shrink_walk(&zram->zram_list_lru, sc, zram_shrink_cb, &ctx);

	if (encountered_in_swapcache) {
		return SHRINK_STOP;
	}

	return shrink_ret ? shrink_ret : SHRINK_STOP;
}

static void zram_init_shrinker(struct zram *zram)
{
	struct shrinker *shrinker;

	shrinker = shrinker_alloc(SHRINKER_NUMA_AWARE | SHRINKER_MEMCG_AWARE, "mm-zram");
	if (!shrinker)
		return;

	shrinker->count_objects = zram_shrinker_count;
	shrinker->scan_objects = zram_shrinker_scan;
	shrinker->batch = 512;
	shrinker->seeks = DEFAULT_SEEKS;
	shrinker->private_data = zram;
	
	zram->zram_shrinker = shrinker;
	
	shrinker_register(zram->zram_shrinker);
}
#endif

static int __init zram_init(void)
{
	int ret;

	BUILD_BUG_ON(__NR_ZRAM_PAGEFLAGS > BITS_PER_LONG);

	ret = cpuhp_setup_state_multi(CPUHP_ZCOMP_PREPARE, "block/zram:prepare",
				      zcomp_cpu_up_prepare, zcomp_cpu_dead);
	if (ret < 0)
		return ret;

	ret = class_register(&zram_control_class);
	if (ret) {
		pr_err("Unable to register zram-control class\n");
		cpuhp_remove_multi_state(CPUHP_ZCOMP_PREPARE);
		return ret;
	}

	zram_debugfs_create();
	zram_major = register_blkdev(0, "zram");
	if (zram_major <= 0) {
		pr_err("Unable to get major number\n");
		class_unregister(&zram_control_class);
		cpuhp_remove_multi_state(CPUHP_ZCOMP_PREPARE);
		return -EBUSY;
	}

	while (num_devices != 0) {
		mutex_lock(&zram_index_mutex);
		ret = zram_add();
		mutex_unlock(&zram_index_mutex);
		if (ret < 0)
			goto out_error;
		num_devices--;
	}

#ifdef CONFIG_ZRAM_WRITEBACK
	monitor_thread = kthread_run(monitor_func, NULL, "zram_monitor");
#endif

#ifdef CONFIG_ZRAM_MULTI_COMP
#define ZRAM_IR_VERSION "1.2"
#define ZRAM_IR_PROGNAME "ZRAM Immediate Recompression (ZRAM-IR)"
#define ZRAM_IR_AUTHOR   "Masahito Suzuki"
	printk(KERN_INFO "%s %s by %s\n",
		ZRAM_IR_PROGNAME, ZRAM_IR_VERSION, ZRAM_IR_AUTHOR);
	zram_sysctl_table_header = register_sysctl("vm", zram_sysctl_table);
#endif //CONFIG_ZRAM_MULTI_COMP

	return 0;

out_error:
	destroy_devices();
	return ret;
}

static void __exit zram_exit(void)
{
#ifdef CONFIG_ZRAM_WRITEBACK
	if (monitor_thread) {
		kthread_stop(monitor_thread);
		monitor_thread = NULL;
	}
#endif

#ifdef CONFIG_ZRAM_MULTI_COMP
	unregister_sysctl_table(zram_sysctl_table_header);
#endif //CONFIG_ZRAM_MULTI_COMP

	destroy_devices();
}

module_init(zram_init);
module_exit(zram_exit);

module_param(num_devices, uint, 0);
MODULE_PARM_DESC(num_devices, "Number of pre-created zram devices");

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Nitin Gupta <ngupta@vflare.org>");
MODULE_AUTHOR("Coolapk@BrokeStar");
MODULE_DESCRIPTION("Compressed RAM Block Device");
