// SPDX-License-Identifier: GPL-2.0-or-later

#define KMSG_COMPONENT "zram_wb"
#define pr_fmt(fmt) KMSG_COMPONENT ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/freezer.h>

#include "zram_wb.h"

static struct task_struct *wb_thread;
static DECLARE_WAIT_QUEUE_HEAD(wb_wq);
static struct zram_wb_request_list wb_req_list;

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
	WARN_ON_ONCE(!was_set);
	atomic64_dec(&zram->stats.bd_count);
}

static void complete_wb_request(struct zram_wb_request *req)
{
	struct zram *zram = req->zram;
	unsigned long blk_idx = req->blk_idx;

	free_block_bdev(zram, blk_idx);
	free_wb_request(req);
}

void enqueue_wb_request(struct zram_wb_request_list *req_list,
			struct zram_wb_request *req)
{
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

void free_wb_request(struct zram_wb_request *req)
{
	struct bio *bio = req->bio;
	struct page *page = bio_first_page_all(bio);

	__free_page(page);
	bio_put(bio);
	kfree(req);
}

int setup_zram_writeback(void)
{
	spin_lock_init(&wb_req_list.lock);
	INIT_LIST_HEAD(&wb_req_list.head);
	wb_req_list.count = 0;

	wb_thread = kthread_run(wb_thread_func, NULL, "zram_wb_thread");
	if (IS_ERR(wb_thread)) {
		pr_err("Unable to create zram_wb_thread\n");
		return -1;
	}
	return 0;
}

void destroy_zram_writeback(void)
{
	kthread_stop(wb_thread);
	destroy_wb_request_list(&wb_req_list);
}

