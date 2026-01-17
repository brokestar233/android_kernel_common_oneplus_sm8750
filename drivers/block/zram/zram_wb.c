// SPDX-License-Identifier: GPL-2.0-or-later

#define KMSG_COMPONENT "zram_wb"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/freezer.h>
#include <linux/blkdev.h>

#include "zram_wb.h"

static struct task_struct *wb_thread;
static DECLARE_WAIT_QUEUE_HEAD(wb_wq);
static struct zram_wb_request_list wb_req_list;
static struct bio_set zram_wb_bs;

/* 
 * front_pad: 在 bio 结构之前预留空间存放 zram_wb_batch_request
 * 这个结构现在比较大 (包含数组)，必须确保 bio 对齐
 */
#define ZRAM_WB_FRONT_PAD \
	roundup(sizeof(struct zram_wb_batch_request), __alignof__(struct bio))

/*
 * 从 bio 指针获取其前面的 zram_wb_batch_request 结构
 */
#define bio_to_wb_batch(bio) \
	((struct zram_wb_batch_request *)((char *)(bio) - ZRAM_WB_FRONT_PAD))

/* 
 * 内部辅助函数：尝试分配指定长度的连续区间
 * 返回值：成功返回起始索引，失败返回 0
 */
static unsigned long alloc_block_bdev_range(struct zram *zram, int count)
{
	unsigned long blk_idx = 1; /* skip 0 bit */
	unsigned long i;
	/*
	 * 自动对齐：尝试让起始索引按 count 对齐 (前提 count 是 2 的幂)
     * 这样可以显著提高底层块设备的合并效率
     */
    unsigned long align_mask = (unsigned long)count - 1;

retry:
	/* 
	 * 1. 查找：寻找连续 count 个 0 位 
	 */
	blk_idx = bitmap_find_next_zero_area(zram->bitmap, zram->nr_pages, 
					     blk_idx, count, align_mask);
	
	if (blk_idx >= zram->nr_pages)
		return 0;

	/* 
	 * 2. 尝试锁定：原子地设置每一位
	 * 这是一个乐观锁策略。如果中途失败，必须回滚。
	 */
	for (i = 0; i < count; i++) {
		if (test_and_set_bit(blk_idx + i, zram->bitmap)) {
			/* 
			 * 发生竞争：有人在我们之前抢占了 blk_idx + i。
			 * 回滚：清除之前已经由本线程设置的位 [0 ... i-1]
			 */
			while (i > 0) {
				i--;
				clear_bit(blk_idx + i, zram->bitmap);
			}
			
			/* 从冲突位置的下一位重新开始搜索 */
			blk_idx++; 
			goto retry;
		}
	}

	/* 成功分配 */
	atomic64_add(count, &zram->stats.bd_count);
	return blk_idx;
}

/*
 * 实现 TODO 1.2: 分配降级策略
 * req_count: 请求分配的块数 (例如 64)
 * act_count: 输出参数，实际分配的块数
 */
unsigned long alloc_block_bdev_batch(struct zram *zram, int req_count, int *act_count)
{
    static const int FALLBACK_SIZES[] = {64, 32, 16, 8, 4, 2, 1};
    int i;

    for (i = 0; i < ARRAY_SIZE(FALLBACK_SIZES); i++) {
        int try_count = FALLBACK_SIZES[i];
        if (try_count > req_count) continue;   // 不超过请求量

        unsigned long blk_idx = alloc_block_bdev_range(zram, try_count);
        if (blk_idx) {
            if (act_count) *act_count = try_count;
            return blk_idx;
        }
    }

    if (act_count) *act_count = 0;
    return 0;
}

/* 保持原有单块分配函数的兼容性 */
unsigned long alloc_block_bdev(struct zram *zram)
{
	int count = 0;
	return alloc_block_bdev_batch(zram, 1, &count);
}

/* 新增：批量释放辅助函数 */
void free_block_bdev_range(struct zram *zram, unsigned long blk_idx, int count)
{
	int i;
	/* 
	 * 注意：这里假设调用者保证了范围的合法性
	 * 逐个清除位
	 */
	for (i = 0; i < count; i++) {
		if (!test_and_clear_bit(blk_idx + i, zram->bitmap))
			WARN_ON_ONCE(1); /* 释放了未分配的块 */
	}
	atomic64_sub(count, &zram->stats.bd_count);
}

/* 保持原有单块释放函数的兼容性 */
void free_block_bdev(struct zram *zram, unsigned long blk_idx)
{
	free_block_bdev_range(zram, blk_idx, 1);
}


/*
 * 处理完成的 BIO 批次
 * 这是一个核心函数，负责批量释放资源
 */
static void complete_wb_batch(struct zram_wb_batch_request *req)
{
	struct zram *zram = req->zram;
	struct zram_pp_ctl *ctl = req->ppctl;
	struct bio *bio = req->bio;
	bool io_error = bio->bi_status != BLK_STS_OK;
	int i;

	/* 遍历批次中的每一个子请求 */
	for (i = 0; i < req->count; i++) {
		struct zram_wb_sub_req *sub = &req->sub_reqs[i];
		unsigned long index = sub->index;
		unsigned long blk_idx = sub->blk_idx;
		struct zram_pp_slot *pps = sub->pps;

		if (io_error)
			goto handle_err;

		/* 更新统计 */
		atomic64_inc(&zram->stats.bd_writes);

		/* 锁定槽位进行状态变更 */
		zram_slot_lock(zram, index);

		/* 
		 * 极少数情况：在写回期间槽位被重新分配或释放了
		 * 我们必须检查 ZRAM_PP_SLOT 标志
		 */
		if (!zram_test_flag(zram, index, ZRAM_PP_SLOT)) {
			zram_slot_unlock(zram, index);
			goto handle_err;
		}

		/* 成功路径：释放内存页，设置写回标志 */
		zram_free_page(zram, index);
		zram_set_flag(zram, index, ZRAM_WB);
		zram_set_handle(zram, index, blk_idx);
		atomic64_inc(&zram->stats.pages_stored);

		/* 更新写回限制配额 */
		spin_lock(&zram->wb_limit_lock);
		if (zram->wb_limit_enable && zram->bd_wb_limit > 0)
			zram->bd_wb_limit -=  1UL << (PAGE_SHIFT - 12);
		spin_unlock(&zram->wb_limit_lock);

		zram_slot_unlock(zram, index);
		
		/* 释放后处理槽位包装器 */
		free_pp_slot(zram, pps);
		continue;

handle_err:
		/* 失败路径：回滚块分配，保留 ZRAM 内存页 */
		free_block_bdev(zram, blk_idx);
		free_pp_slot(zram, pps);
	}

	/* 
	 * 重要：ctl->num_pp_slots 记录了待处理的总数
	 * 此时减少当前批次处理的数量 (req->count)
	 */
	if (atomic_sub_and_test(req->count, &ctl->num_pp_slots))
		complete(&ctl->all_done);

	/* 释放 BIO 及其挂载的所有 pages */
	{
		struct bio_vec *bv;
		struct bvec_iter_all iter;
		
		bio_for_each_segment_all(bv, bio, iter) {
			__free_page(bv->bv_page);
		}
		/* bio_put 会释放 bio 内存以及 front_pad */
		bio_put(bio);
	}
}

static void enqueue_wb_request(struct zram_wb_request_list *req_list,
			       struct zram_wb_batch_request *req)
{
	spin_lock_bh(&req_list->lock);
	list_add_tail(&req->node, &req_list->head);
	req_list->count++;
	spin_unlock_bh(&req_list->lock);
}

static struct zram_wb_batch_request *dequeue_wb_request(
	struct zram_wb_request_list *req_list)
{
	struct zram_wb_batch_request *req = NULL;

	spin_lock_bh(&req_list->lock);
	if (!list_empty(&req_list->head)) {
		req = list_first_entry(&req_list->head,
				       struct zram_wb_batch_request,
				       node);
		list_del(&req->node);
		req_list->count--;
	}
	spin_unlock_bh(&req_list->lock);

	return req;
}

static void destroy_wb_request_list(struct zram_wb_request_list *req_list)
{
	struct zram_wb_batch_request *req;

	while (!list_empty(&req_list->head)) {
		req = dequeue_wb_request(req_list);
		int i;
		for(i = 0; i < req->count; i++) {
			free_block_bdev(req->zram, req->sub_reqs[i].blk_idx);
			free_pp_slot(req->zram, req->sub_reqs[i].pps);
		}
		
		/* Free pages and bio */
		struct bio_vec *bv;
		struct bvec_iter_all iter;
		bio_for_each_segment_all(bv, req->bio, iter) {
			__free_page(bv->bv_page);
		}
		bio_put(req->bio);
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
			struct zram_wb_batch_request *req;
			req = dequeue_wb_request(&wb_req_list);
			if (!req)
				break;
			complete_wb_batch(req);
		}
	}
	return 0;
}

static void zram_writeback_end_io(struct bio *bio)
{
	struct zram_wb_batch_request *req = bio_to_wb_batch(bio);
	enqueue_wb_request(&wb_req_list, req);
	wake_up(&wb_wq);
}

/* 
 * 外部接口：分配一个新的批次请求 
 */
struct zram_wb_batch_request *alloc_wb_batch_request(struct zram *zram,
						     struct zram_pp_ctl *ctl,
						     unsigned long start_blk_idx)
{
	struct bio *bio;
	struct zram_wb_batch_request *req;

	/*
	 * 使用 bioset 分配 bio。
	 * ZRAM_WB_MAX_BATCH_SIZE 定义了 bio_vec 的最大数量。
	 * front_pad 会自动被 bio_alloc 分配在 bio 之前。
	 */
	bio = bio_alloc_bioset(zram->bdev, ZRAM_WB_MAX_BATCH_SIZE, 
			       REQ_OP_WRITE, GFP_NOIO,
			       &zram_wb_bs);
	if (!bio)
		return NULL;

	/* 获取前面预留的结构体 */
	req = bio_to_wb_batch(bio);
	req->zram = zram;
	req->ppctl = ctl;
	req->bio = bio;
	req->count = 0; /* 初始计数为 0 */

	/* 设置 bio 的起始扇区和回调 */
	bio->bi_iter.bi_sector = start_blk_idx * (PAGE_SIZE >> 9);
	bio->bi_end_io = zram_writeback_end_io;
	
	return req;
}

int setup_zram_writeback(void)
{
	/*
	 * 初始化 bioset:
	 * pool_size: 64 (缓冲池大小)
	 * front_pad: ZRAM_WB_FRONT_PAD (包含我们的 zram_wb_batch_request)
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
	if (wb_thread)
		kthread_stop(wb_thread);
	destroy_wb_request_list(&wb_req_list);
	bioset_exit(&zram_wb_bs);
}