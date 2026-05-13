// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cgroup.h>
#include <linux/kernel.h>
#include <linux/memcontrol.h>
#include <linux/mmzone.h>
#include <linux/signal.h>
#include <linux/swap.h>

#define BATCH_4M		(1 << 10)
#define RECLAIM_INACTIVE	0
#define RECLAIM_ALL		1

static unsigned long zram_memcg_lru_pages(struct mem_cgroup *memcg,
		int stat_item)
{
	int idx = LRU_BASE + stat_item;

	return memcg_page_state(memcg, idx);
}

static bool zram_inactive_file_is_low(struct mem_cgroup *memcg)
{
	unsigned long nr_inactive_file;

	nr_inactive_file = memcg_page_state(memcg, NR_INACTIVE_FILE);
	return nr_inactive_file < (SZ_512M + SZ_256M) / PAGE_SIZE;
}

static int zram_force_shrink_batch(struct mem_cgroup *memcg,
		unsigned long nr_need_reclaim,
		unsigned long *nr_reclaimed,
		unsigned long batch,
		unsigned int reclaim_options)
{
	int ret = 0;
	gfp_t gfp_mask = GFP_KERNEL;

	while (*nr_reclaimed < nr_need_reclaim) {
		unsigned long reclaimed;

		if (reclaim_options != MEMCG_RECLAIM_MAY_SWAP &&
		    zram_inactive_file_is_low(memcg))
			break;

		reclaimed = try_to_free_mem_cgroup_pages(memcg, batch, gfp_mask,
				reclaim_options);
		if (!reclaimed)
			break;

		*nr_reclaimed += reclaimed;

		if (unlikely(sigismember(&current->pending.signal, SIGUSR2) ||
			    sigismember(&current->signal->shared_pending.signal,
				       SIGUSR2))) {
			pr_warn("zram_memcg: abort shrink while shrinking\n");
			ret = -EINTR;
			break;
		}
	}

	pr_info("zram_memcg: memcg=%hu try reclaim=%lu reclaimed=%lu options=%u\n",
		mem_cgroup_id(memcg), nr_need_reclaim, *nr_reclaimed,
		reclaim_options);

	return ret;
}

static unsigned long zram_get_reclaim_pages(struct mem_cgroup *memcg,
		bool file, char *buf, unsigned long *batch)
{
	unsigned long nr_need_reclaim = 0;
	unsigned long reclaim_flag = 0;
	unsigned long reclaim_batch = 0;
	int ret;
	int stat_item = file ? NR_INACTIVE_FILE : NR_INACTIVE_ANON;

	buf = strstrip(buf);
	ret = sscanf(buf, "%lu %lu", &reclaim_flag, &reclaim_batch);
	if (ret != 1 && ret != 2) {
		pr_err("zram_memcg: reclaim_flag %s value is error\n", buf);
		return 0;
	}

	if (reclaim_flag == RECLAIM_INACTIVE)
		nr_need_reclaim = zram_memcg_lru_pages(memcg, stat_item);
	else if (reclaim_flag == RECLAIM_ALL)
		nr_need_reclaim = zram_memcg_lru_pages(memcg, stat_item) +
			zram_memcg_lru_pages(memcg, stat_item + LRU_ACTIVE);
	else
		nr_need_reclaim = reclaim_flag;

	if (reclaim_batch > 0 && batch)
		*batch = reclaim_batch;

	pr_info("zram_memcg: batch=%lu file=%d nr_need_reclaim=%lu memcg=%hu\n",
		batch ? *batch : 0, file, nr_need_reclaim, mem_cgroup_id(memcg));

	return nr_need_reclaim;
}

static ssize_t zram_mem_cgroup_force_shrink(struct kernfs_open_file *of,
		char *buf, size_t nbytes, bool file)
{
	struct mem_cgroup *memcg;
	unsigned long nr_need_reclaim;
	unsigned long nr_reclaimed = 0;
	unsigned long batch = BATCH_4M;
	unsigned int reclaim_options = 0;

	memcg = mem_cgroup_from_css(of_css(of));
	if (file && zram_inactive_file_is_low(memcg))
		return -EBUSY;

	nr_need_reclaim = zram_get_reclaim_pages(memcg, file, buf, &batch);
	if (!file)
		reclaim_options = MEMCG_RECLAIM_MAY_SWAP;

	zram_force_shrink_batch(memcg, nr_need_reclaim, &nr_reclaimed,
			batch, reclaim_options);

	return nbytes;
}

static ssize_t zram_mem_cgroup_force_shrink_file(struct kernfs_open_file *of,
		char *buf, size_t nbytes, loff_t off)
{
	return zram_mem_cgroup_force_shrink(of, buf, nbytes, true);
}

static struct cftype zram_memcg_legacy_files[] = {
	{
		.name = "force_shrink_file",
		.write = zram_mem_cgroup_force_shrink_file,
	},
	{ }
};

int zram_memcg_init(void)
{
	return cgroup_add_legacy_cftypes(&memory_cgrp_subsys,
			zram_memcg_legacy_files);
}

void zram_memcg_exit(void)
{
	cgroup_rm_cftypes(zram_memcg_legacy_files);
}
