// SPDX-License-Identifier: GPL-2.0-or-later

#define KMSG_COMPONENT "zram_wb"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/freezer.h>
#include <linux/blkdev.h>
#include <linux/ktime.h>
#include <linux/slab.h>
#include <linux/sysms_finder.h>
#include <linux/wait_bit.h>

#include "zram_wb.h"

static struct task_struct *wb_thread;
static DECLARE_WAIT_QUEUE_HEAD(wb_wq);
static struct zram_wb_request_list wb_req_list;
static struct bio_set zram_wb_bs;

/* 
 * front_pad: 在 bio 结构之前预留空间存放 zram_wb_request
 * 需要对齐以保证 bio 结构的正确对齐
 */
#define ZRAM_WB_FRONT_PAD \
	roundup(sizeof(struct zram_wb_request), __alignof__(struct bio))

/*
 * 从 bio 指针获取其前面的 zram_wb_request 结构
 * bio 分配时在其前面预留了 ZRAM_WB_FRONT_PAD 字节
 */
#define bio_to_zram_wb_request(bio) \
	((struct zram_wb_request *)((char *)(bio) - ZRAM_WB_FRONT_PAD))

unsigned long alloc_block_bdev(struct zram *zram)
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

void free_block_bdev(struct zram *zram, unsigned long blk_idx)
{
	int was_set;

	was_set = test_and_clear_bit(blk_idx, zram->bitmap);
	if (WARN_ON_ONCE(!was_set))
		return;
	atomic64_dec(&zram->stats.bd_count);
}

unsigned long alloc_block_bdev_batch(struct zram *zram, int req_count, int *act_count)
{
	unsigned long blk_idx, conflict, start, end, run_len;
	int found, i;

	*act_count = 0;
	if (req_count <= 0)
		return 0;

	/* Skip bit 0 to keep zram.handle = 0 invalid. */
	blk_idx = 1;
	while (blk_idx < zram->nr_pages) {
		blk_idx = find_next_zero_bit(zram->bitmap, zram->nr_pages, blk_idx);
		if (blk_idx >= zram->nr_pages)
			break;

		start = blk_idx;
		end = find_next_bit(zram->bitmap, zram->nr_pages, start);
		run_len = end - start;
		if (run_len < min_t(unsigned long, req_count, 2)) {
			if (end >= zram->nr_pages)
				break;
			blk_idx = end + 1;
			continue;
		}
		found = min_t(unsigned long, run_len, req_count);
		if (!found)
			break;

		for (i = 0; i < found; i++) {
			if (test_and_set_bit(start + i, zram->bitmap))
				goto retry;
		}

		atomic64_add(found, &zram->stats.bd_count);
		*act_count = found;
		return start;

retry:
		/*
		 * Roll back on conflict and continue after the conflicting
		 * block.  The returned range must be truly contiguous because
		 * callers address it as base + i.
		 */
		conflict = start + i;
		while (i-- > 0) {
			test_and_clear_bit(start + i, zram->bitmap);
			atomic64_dec(&zram->stats.bd_count);
		}
		blk_idx = conflict + 1;
	}

	return 0;
}

void free_block_bdev_range(struct zram *zram, unsigned long blk_idx, int count)
{
	int i;

	for (i = 0; i < count; i++) {
		int was_set = test_and_clear_bit(blk_idx + i, zram->bitmap);
		if (WARN_ON_ONCE(!was_set))
			continue;
		atomic64_dec(&zram->stats.bd_count);
	}
}

static void wb_read_endio(struct bio *bio)
{
	struct completion *done = bio->bi_private;

	if (bio->bi_status)
		pr_err("I/O error in wb_read_endio: %d\n", bio->bi_status);

	complete(done);
}

int zram_read_wb_pages_sync(struct zram *zram, struct page **pages,
			    unsigned long start_entry, unsigned int nr_pages)
{
	struct bio *bio;
	struct completion done;
	int i;
	int ret = 0;

	for (i = 0; i < nr_pages; i++) {
		init_completion(&done);

		bio = bio_alloc(zram->bdev, 1, REQ_OP_READ, GFP_NOIO);
		if (!bio) {
			ret = -ENOMEM;
			break;
		}

		bio->bi_iter.bi_sector = (start_entry + i) * (PAGE_SIZE >> 9);
		bio->bi_end_io = wb_read_endio;
		bio->bi_private = &done;

		if (bio_add_page(bio, pages[i], PAGE_SIZE, 0) != PAGE_SIZE) {
			bio_put(bio);
			ret = -EIO;
			break;
		}

		submit_bio(bio);
		wait_for_completion(&done);

		if (bio->bi_status) {
			bio_put(bio);
			ret = -EIO;
			break;
		}

		bio_put(bio);
	}

	return ret;
}

static void wb_write_endio(struct bio *bio)
{
	struct completion *done = bio->bi_private;

	complete(done);
}

int zram_wb_write_pages_sync(struct zram *zram, struct page **pages,
			     unsigned long start_blk, unsigned int nr_pages)
{
	struct bio *bio;
	struct completion done;
	int i;
	int ret = 0;

	for (i = 0; i < nr_pages; i++) {
		init_completion(&done);

		bio = bio_alloc(zram->bdev, 1, REQ_OP_WRITE, GFP_NOIO);
		if (!bio) {
			ret = -ENOMEM;
			break;
		}

		bio->bi_iter.bi_sector = (start_blk + i) * (PAGE_SIZE >> 9);
		bio->bi_end_io = wb_write_endio;
		bio->bi_private = &done;

		if (bio_add_page(bio, pages[i], PAGE_SIZE, 0) != PAGE_SIZE) {
			bio_put(bio);
			ret = -EIO;
			break;
		}

		submit_bio(bio);
		wait_for_completion(&done);

		if (bio->bi_status) {
			bio_put(bio);
			ret = -EIO;
			break;
		}

		bio_put(bio);
	}

	return ret;
}

static void complete_wb_request(struct zram_wb_request *req)
{
	struct zram *zram = req->zram;
	struct zram_pp_slot *pps = req->pps;
	struct zram_pp_ctl *ctl = req->ppctl;
	unsigned long index = pps->index;
	unsigned long blk_idx = req->blk_idx;
	struct bio *bio = req->bio;

	if (bio->bi_status)
		goto out_err;

	atomic64_inc(&zram->stats.bd_writes);
	zram_slot_lock(zram, index);

	/*
	 * We release slot lock during writeback so slot can change
	 * under us: slot_free() or slot_free() and reallocation
	 * (zram_write_page()). In both cases slot loses
	 * ZRAM_PP_SLOT flag. No concurrent post-processing can set
	 * ZRAM_PP_SLOT on such slots until current post-processing
	 * finishes.
	 */
	if (!zram_test_flag(zram, index, ZRAM_PP_SLOT)) {
		zram_slot_unlock(zram, index);
		goto out_err;
	}

	zram_free_page(zram, index);
	zram_set_flag(zram, index, ZRAM_WB);
	zram_set_handle(zram, index, blk_idx);
	atomic64_inc(&zram->stats.pages_stored);
	spin_lock(&zram->wb_limit_lock);
	if (zram->wb_limit_enable && zram->bd_wb_limit > 0)
		zram->bd_wb_limit -=  1UL << (PAGE_SHIFT - 12);
	spin_unlock(&zram->wb_limit_lock);
	zram_slot_unlock(zram, index);

	/*
	 * zram_free_page 已经在内部清除了 ZRAM_PP_SLOT (zram_drv.c:1708),
	 * 所以这里只需要 kfree pps,不需要调用 free_pp_slot 再次清标志。
	 */
	kfree(pps);
	free_wb_request(req);
	if (atomic_dec_and_test(&ctl->num_pp_slots))
		complete(&ctl->all_done);
	return;

out_err:
	atomic64_inc(&zram->stats.failed_writes);
	free_block_bdev(zram, blk_idx);
	free_pp_slot(zram, pps);
	free_wb_request(req);
	if (atomic_dec_and_test(&ctl->num_pp_slots))
		complete(&ctl->all_done);
}

static void enqueue_wb_request(struct zram_wb_request_list *req_list,
			       struct zram_wb_request *req)
{
	/*
	 * The enqueue path comes from softirq context:
	 * blk_done_softirq -> bio_endio -> zram_writeback_end_io
	 * Use spin_lock_bh for locking.
	 */
	spin_lock_bh(&req_list->lock);
	list_add_tail(&req->node, &req_list->head);
	req_list->count++;
	spin_unlock_bh(&req_list->lock);
}

static struct zram_wb_request *dequeue_wb_request(
	struct zram_wb_request_list *req_list)
{
	struct zram_wb_request *req = NULL;

	spin_lock_bh(&req_list->lock);
	if (!list_empty(&req_list->head)) {
		req = list_first_entry(&req_list->head,
				       struct zram_wb_request,
				       node);
		list_del(&req->node);
		req_list->count--;
	}
	spin_unlock_bh(&req_list->lock);

	return req;
}

static void destroy_wb_request_list(struct zram_wb_request_list *req_list)
{
	struct zram_wb_request *req;

	while (!list_empty(&req_list->head)) {
		req = dequeue_wb_request(req_list);
		free_block_bdev(req->zram, req->blk_idx);
		free_pp_slot(req->zram, req->pps);
		free_wb_request(req);
	}
}

static bool wb_ready_to_run(void)
{
	int count;

	spin_lock_bh(&wb_req_list.lock);
	count = wb_req_list.count;
	spin_unlock_bh(&wb_req_list.lock);

	return count > 0;
}

static int wb_thread_func(void *data)
{
	set_freezable();

	while (!kthread_should_stop()) {
		wait_event_freezable(wb_wq, wb_ready_to_run());

		while (1) {
			struct zram_wb_request *req;

			req = dequeue_wb_request(&wb_req_list);
			if (!req)
				break;
			complete_wb_request(req);
		}
	}
	return 0;
}

static void zram_writeback_end_io(struct bio *bio)
{
	struct zram_wb_request *req = bio_to_zram_wb_request(bio);

	enqueue_wb_request(&wb_req_list, req);
	wake_up(&wb_wq);
}

struct zram_wb_request *alloc_wb_request(struct zram *zram,
					 struct zram_pp_slot *pps,
					 struct zram_pp_ctl *ppctl,
					 unsigned long blk_idx)
{
	struct zram_wb_request *req;
	struct page *page;
	struct bio *bio;
	int err = 0;

	page = alloc_page(GFP_NOIO | __GFP_NOWARN);
	if (!page)
		return ERR_PTR(-ENOMEM);

	/*
	 * 使用 bioset 分配 bio,front_pad 空间会被自动分配在 bio 之前
	 * 需要 BIOSET_NEED_BVECS 标志,因为我们手动添加 page
	 */
	bio = bio_alloc_bioset(zram->bdev, 1, REQ_OP_WRITE, GFP_NOIO,
			       &zram_wb_bs);
	if (!bio) {
		err = -ENOMEM;
		goto out_free_page;
	}

	/* 通过宏访问 bio 前面的 zram_wb_request 结构 */
	req = bio_to_zram_wb_request(bio);
	req->zram = zram;
	req->pps = pps;
	req->ppctl = ppctl;
	req->blk_idx = blk_idx;
	req->bio = bio;

	bio->bi_iter.bi_sector = blk_idx * (PAGE_SIZE >> 9);
	__bio_add_page(bio, page, PAGE_SIZE, 0);
	bio->bi_end_io = zram_writeback_end_io;
	return req;

out_free_page:
	__free_page(page);
	return ERR_PTR(err);
}

void free_wb_request(struct zram_wb_request *req)
{
	struct bio *bio = req->bio;
	struct page *page = bio_first_page_all(bio);

	if (page)
		__free_page(page);
	/*
	 * bio_put 会自动释放 bio 以及其 front_pad 空间
	 * 不需要单独释放 req
	 */
	bio_put(bio);
}

/* ===== GC Implementation ===== */

static enum zram_gc_mode zram_gc_mode(struct zram *zram)
{
	if (!check_charging_state())
		return ZRAM_GC_MODE_NORMAL;

	if (!check_game_pid())
		return ZRAM_GC_MODE_NORMAL;

	return ZRAM_GC_MODE_AGGRESSIVE;
}

static unsigned int zram_gc_max_pages(struct zram *zram)
{
	if (zram_gc_mode(zram) == ZRAM_GC_MODE_AGGRESSIVE)
		return ZRAM_GC_AGGRESSIVE_MAX_PAGES;

	return ZRAM_GC_NORMAL_MAX_PAGES;
}

static unsigned int zram_gc_clamp_target_pages(struct zram *zram,
		unsigned int target_pages)
{
	target_pages = min_t(unsigned int, target_pages, ZRAM_GC_MAX_BATCH);
	target_pages = min_t(unsigned int, target_pages,
			     zram_gc_max_pages(zram));

	return max_t(unsigned int, target_pages, 2);
}

static bool zram_wb_allowed(struct zram *zram)
{
	if (!zram->backing_dev)
		return false;

	return true;
}

static void zram_wb_clear_flag(struct zram *zram, u32 index,
			       enum zram_pageflags flag)
{
	zram->table[index].flags &= ~BIT(flag);
}

static void zram_wb_clear_migration(struct zram *zram, u32 index)
{
	zram_wb_clear_flag(zram, index, ZRAM_STATE_MIGRATING);
	smp_mb();
	wake_up_bit(&zram->table[index].flags, ZRAM_STATE_MIGRATING);
}

static void zram_replace_block_bdev(struct zram *zram,
		unsigned long old_blk_idx, unsigned long new_blk_idx)
{
	if (old_blk_idx == new_blk_idx)
		return;

	if (WARN_ON_ONCE(!test_bit(new_blk_idx, zram->bitmap))) {
		if (!test_and_set_bit(new_blk_idx, zram->bitmap))
			atomic64_inc(&zram->stats.bd_count);
	}
	free_block_bdev(zram, old_blk_idx);
}

static bool zram_wb_slot_can_gc(struct zram *zram, unsigned long index)
{
	if (!zram_test_flag(zram, index, ZRAM_WB))
		return false;

	if (zram_test_flag(zram, index, ZRAM_STATE_MIGRATING))
		return false;

	if (zram_test_flag(zram, index, ZRAM_PP_SLOT))
		return false;

	return true;
}

struct zram_gc_candidate {
	unsigned long index;
	unsigned long old_blk;
};

static void zram_gc_insert_candidate(struct zram_gc_candidate *candidates,
				     int *nr_candidates,
				     int max_candidates,
				     unsigned long index,
				     unsigned long old_blk)
{
	int pos;

	if (*nr_candidates == max_candidates &&
	    old_blk <= candidates[max_candidates - 1].old_blk)
		return;

	pos = min(*nr_candidates, max_candidates - 1);
	if (*nr_candidates < max_candidates)
		(*nr_candidates)++;

	while (pos > 0 && candidates[pos - 1].old_blk < old_blk) {
		candidates[pos] = candidates[pos - 1];
		pos--;
	}

	candidates[pos].index = index;
	candidates[pos].old_blk = old_blk;
}

static int zram_gc_compact(struct zram *zram, int target_pages)
{
	unsigned long index;
	unsigned long wb_indices[ZRAM_GC_MAX_BATCH];
	unsigned long old_blks[ZRAM_GC_MAX_BATCH];
	struct zram_gc_candidate candidates[ZRAM_GC_MAX_BATCH];
	struct page *pages[ZRAM_GC_MAX_BATCH] = {};
	unsigned long new_base;
	ktime_t gc_start;
	int nr_candidates = 0;
	int nr_selected = 0;
	int act_count = 0;
	int i;
	u32 nr_entries;
	unsigned long nr_total_entries;

	if (!zram_wb_allowed(zram) || target_pages < 2)
		return 0;

	if (!zram->table || !zram->disksize)
		return 0;

	nr_total_entries = zram->disksize >> PAGE_SHIFT;
	if (!nr_total_entries)
		return 0;

	gc_start = ktime_get();
	target_pages = zram_gc_clamp_target_pages(zram, target_pages);

	/*
	 * Reserve the earliest contiguous hole first, then only move WB pages
	 * from higher backing blocks into that hole.  This makes GC directional:
	 * it fills front fragmentation and frees blocks closer to the tail.
	 */
	new_base = alloc_block_bdev_batch(zram, target_pages, &act_count);
	if (!new_base || act_count < 2) {
		if (new_base)
			free_block_bdev_range(zram, new_base, act_count);
		return 0;
	}
	target_pages = act_count;

	/* Scan for high WB pages starting from cursor. */
	index = READ_ONCE(zram->gc_scan_cursor);
	for (nr_entries = 0; nr_entries < ZRAM_GC_MAX_SCAN_ENTRIES; nr_entries++) {
		unsigned long flags;
		unsigned long old_blk;

		if (index >= nr_total_entries) {
			index = 0;
			break;
		}

		if (nr_selected >= target_pages)
			break;

		/*
		 * 乐观预检：无锁读取 flags，快速跳过非 WB 页面
		 * READ_ONCE 确保编译器不优化单次读取
		 */
		flags = READ_ONCE(zram->table[index].flags);
		if (!(flags & BIT(ZRAM_WB)))
			goto next;
		if (flags & BIT(ZRAM_STATE_MIGRATING))
			goto next;
		if (flags & BIT(ZRAM_PP_SLOT))
			goto next;

		/* 候选页面：用 trylock 避免阻塞在竞争锁上 */
		if (!zram_slot_trylock(zram, index))
			goto next;

		/* 持锁后二次确认状态未变 */
		if (likely(zram_wb_slot_can_gc(zram, index))) {
			old_blk = zram_get_handle(zram, index);
			if (old_blk > new_base)
				zram_gc_insert_candidate(candidates,
							 &nr_candidates,
							 target_pages,
							 index, old_blk);
		}
		zram_slot_unlock(zram, index);
next:
		index++;
	}

	WRITE_ONCE(zram->gc_scan_cursor, index);

	/* Budget check after scan */
	if (ktime_ms_delta(ktime_get(), gc_start) >= ZRAM_GC_BUDGET_MS)
		goto free_reserved;

	for (i = 0; i < nr_candidates && nr_selected < target_pages; i++) {
		unsigned long dst_blk = new_base + nr_selected;

		if (candidates[i].old_blk <= dst_blk)
			break;

		zram_slot_lock(zram, candidates[i].index);
		if (zram_wb_slot_can_gc(zram, candidates[i].index) &&
		    zram_get_handle(zram, candidates[i].index) == candidates[i].old_blk &&
		    candidates[i].old_blk > dst_blk) {
			zram_set_flag(zram, candidates[i].index,
				      ZRAM_STATE_MIGRATING);
			wb_indices[nr_selected] = candidates[i].index;
			old_blks[nr_selected] = candidates[i].old_blk;
			nr_selected++;
		}
		zram_slot_unlock(zram, candidates[i].index);
	}

	if (nr_selected < 2)
		goto free_reserved;

	if (nr_selected < act_count) {
		free_block_bdev_range(zram, new_base + nr_selected,
				      act_count - nr_selected);
		act_count = nr_selected;
	}

	/* Allocate pages for readback */
	for (i = 0; i < nr_selected; i++) {
		pages[i] = alloc_page(GFP_NOIO | __GFP_NOWARN);
		if (!pages[i])
			goto free_pages;
	}

	/* Budget check */
	if (ktime_ms_delta(ktime_get(), gc_start) >= ZRAM_GC_BUDGET_MS)
		goto free_pages;

	/*
	 * Read each old block individually.  old_blks[] entries are not
	 * contiguous because they were allocated one at a time, so a
	 * single batched read from old_blks[0] would read wrong data.
	 */
	for (i = 0; i < nr_selected; i++) {
		if (zram_read_wb_pages_sync(zram, &pages[i], old_blks[i], 1))
			goto free_pages;
	}

	/* Budget check */
	if (ktime_ms_delta(ktime_get(), gc_start) >= ZRAM_GC_BUDGET_MS)
		goto free_pages;

	/* Write to new contiguous location */
	if (zram_wb_write_pages_sync(zram, pages, new_base, nr_selected))
		goto free_pages;

	/* Update mapping for each slot */
	for (i = 0; i < nr_selected; i++) {
		zram_slot_lock(zram, wb_indices[i]);

		if (!zram_test_flag(zram, wb_indices[i], ZRAM_STATE_MIGRATING) ||
		    !zram_test_flag(zram, wb_indices[i], ZRAM_WB) ||
		    zram_get_handle(zram, wb_indices[i]) != old_blks[i]) {
			/* Slot changed under us, free the new block. */
			free_block_bdev(zram, new_base + i);
			if (zram_test_flag(zram, wb_indices[i], ZRAM_STATE_MIGRATING))
				zram_wb_clear_migration(zram, wb_indices[i]);
			zram_slot_unlock(zram, wb_indices[i]);
			continue;
		}

		zram_set_handle(zram, wb_indices[i], new_base + i);
		zram_replace_block_bdev(zram, old_blks[i], new_base + i);
		zram_wb_clear_migration(zram, wb_indices[i]);

		zram_slot_unlock(zram, wb_indices[i]);
	}

	/* Clean up pages */
	for (i = 0; i < nr_selected; i++) {
		if (pages[i]) {
			__free_page(pages[i]);
			pages[i] = NULL;
		}
	}

	return nr_selected;

free_pages:
	free_block_bdev_range(zram, new_base, act_count);
	act_count = 0;
	for (i = 0; i < nr_selected; i++) {
		if (pages[i]) {
			__free_page(pages[i]);
			pages[i] = NULL;
		}
	}

free_reserved:
	if (act_count)
		free_block_bdev_range(zram, new_base, act_count);

	for (i = 0; i < nr_selected; i++) {
		zram_slot_lock(zram, wb_indices[i]);
		if (zram_test_flag(zram, wb_indices[i], ZRAM_STATE_MIGRATING))
			zram_wb_clear_migration(zram, wb_indices[i]);
		zram_slot_unlock(zram, wb_indices[i]);
	}

	return 0;
}

static void zram_gc_workfn(struct work_struct *work)
{
	struct zram *zram = container_of(work, struct zram, gc_work);
	unsigned int target_pages = READ_ONCE(zram->gc_target_pages);

	if (READ_ONCE(zram->gc_stopping))
		goto out;

	target_pages = zram_gc_clamp_target_pages(zram, target_pages);

	if (down_read_trylock(&zram->init_lock)) {
		if (zram->disksize && zram->backing_dev && zram_wb_allowed(zram))
			zram_gc_compact(zram, target_pages);
		up_read(&zram->init_lock);
	}

out:
	atomic_set(&zram->gc_pending, 0);
}

static void zram_gc_periodic_workfn(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct zram *zram = container_of(dwork, struct zram, gc_periodic_work);

	if (READ_ONCE(zram->gc_stopping))
		return;

	if (down_read_trylock(&zram->init_lock)) {
		if (zram->disksize && zram->backing_dev && zram_wb_allowed(zram))
			zram_gc_compact(zram, ZRAM_GC_NORMAL_MAX_PAGES);
		up_read(&zram->init_lock);
	}

	if (READ_ONCE(zram->gc_stopping))
		return;

	queue_delayed_work(system_unbound_wq, &zram->gc_periodic_work,
			   ZRAM_GC_PERIODIC_INTERVAL);
}

void zram_schedule_gc(struct zram *zram, int target_pages)
{
	if (READ_ONCE(zram->gc_stopping))
		return;

	if (!zram->backing_dev)
		return;

	if (target_pages > ZRAM_GC_MAX_BATCH)
		target_pages = ZRAM_GC_MAX_BATCH;
	if (target_pages < 2)
		target_pages = 2;

	WRITE_ONCE(zram->gc_target_pages, target_pages);
	if (atomic_cmpxchg(&zram->gc_pending, 0, 1) == 0)
		queue_work(system_unbound_wq, &zram->gc_work);
}

void zram_cancel_gc(struct zram *zram)
{
	WRITE_ONCE(zram->gc_stopping, true);
	cancel_work_sync(&zram->gc_work);
	cancel_delayed_work_sync(&zram->gc_periodic_work);
	atomic_set(&zram->gc_pending, 0);
}

void zram_init_gc(struct zram *zram)
{
	INIT_WORK(&zram->gc_work, zram_gc_workfn);
	INIT_DELAYED_WORK(&zram->gc_periodic_work, zram_gc_periodic_workfn);
	atomic_set(&zram->gc_pending, 0);
	WRITE_ONCE(zram->gc_stopping, false);
	zram->gc_target_pages = ZRAM_GC_MAX_BATCH;
	zram->gc_scan_cursor = 0;
}

int setup_zram_writeback(void)
{
	/*
	 * 初始化 bioset:
	 * - pool_size: 64,预分配的 bio 数量,用于减少分配开销
	 * - front_pad: ZRAM_WB_FRONT_PAD,在每个 bio 之前预留空间
	 * - flags: BIOSET_NEED_BVECS,需要 bio vecs 支持
	 * 
	 * 参考 dm-table.c 的做法,使用 front_pad 可以:
	 * 1. 避免为 zram_wb_request 单独分配内存
	 * 2. 提高缓存局部性(request 和 bio 内存连续)
	 * 3. 简化内存管理(一起分配一起释放)
	 */
	if (bioset_init(&zram_wb_bs, 64, ZRAM_WB_FRONT_PAD, BIOSET_NEED_BVECS)) {
		pr_err("Unable to init zram_wb_bs\n");
		return -1;
	}

	spin_lock_init(&wb_req_list.lock);
	INIT_LIST_HEAD(&wb_req_list.head);
	wb_req_list.count = 0;

	wb_thread = kthread_run(wb_thread_func, NULL, "zram_wb_thread");
	if (IS_ERR(wb_thread)) {
		pr_err("Unable to create zram_wb_thread\n");
		bioset_exit(&zram_wb_bs); 
		return -1;
	}
	return 0;
}

void destroy_zram_writeback(void)
{
	kthread_stop(wb_thread);
	destroy_wb_request_list(&wb_req_list);
	bioset_exit(&zram_wb_bs);
}
