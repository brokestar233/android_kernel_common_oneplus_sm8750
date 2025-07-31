/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2022. All rights reserved.
 * Description: Support delta compression for ZRAM
 */

#include <linux/atomic.h>
#include <linux/types.h>
#include <linux/lz4kd.h>
#include <linux/zsmalloc.h>
#include <linux/highmem.h>
#include <linux/idr.h>
#include <trace/events/block.h> // Should be under CONFIG_ZSDDC_DEBUG
#include "zram_drv.h"
#include "zcomp_ext.h"
#include "sddc.h"
#include "sddc_utils.h"

#ifdef CONFIG_HP_CORE
#include "hp/hyperhold.h"
#endif

static inline unsigned long zram_get_handle(struct zram *zram, u32 index)
{
	return zram->table[index].handle;
}

static inline size_t zram_get_obj_size(struct zram *zram, u32 index)
{
	return zram->table[index].flags & (BIT(ZRAM_FLAG_SHIFT) - 1);
}

static inline bool zram_test_flag(struct zram *zram, u32 index,
			enum zram_pageflags flag)
{
	return zram->table[index].flags & BIT(flag);
}

static inline void zram_set_flag(struct zram *zram, u32 index,
			enum zram_pageflags flag)
{
	zram->table[index].flags |= BIT(flag);
}

static inline void zram_clear_flag(struct zram *zram, u32 index,
			enum zram_pageflags flag)
{
	zram->table[index].flags &= ~BIT(flag);
}

static inline void zram_slot_lock(struct zram *zram, u32 index)
{
	bit_spin_lock(ZRAM_LOCK, &zram->table[index].flags);
}

static inline void zram_slot_unlock(struct zram *zram, u32 index)
{
	bit_spin_unlock(ZRAM_LOCK, &zram->table[index].flags);
}

static void zram_set_obj_size(struct zram *zram,
					u32 index, size_t size)
{
	unsigned long flags = zram->table[index].flags >> ZRAM_FLAG_SHIFT;

	zram->table[index].flags = (flags << ZRAM_FLAG_SHIFT) | size;
}

enum {
	//
	// page consists of slices, slice consists of samples
	// Each sample of page data can be used to calculate hash.
	// 1st sample in slice is used to calculate hash to store slice info in s4k_state
	//
	// So when hash collision happens for samples of different pages we
	// compare these samples byte-by-byte to find out if pages are
	// similar or are full duplicates.
	// By similar pages we mean that they have at least one sample in
	// common even if at different offsets.
	// To use LZ-class(+entropy) algorithm for delta-compression of similar pages
	// minimum sample size may be 3 bytes
	// With 3 bytes sample one does not need to calculate hashes at all.
	// Instead, 3 bytes cast to int32_t, can be used as hash.
	// Reasonable size of sample - to have proper delta compression gains - may
	// be bigger, say, power of 2 for faster hash calculation.
	ENCODED_BYTES_MIN = 8,
	ENCODED_GAIN_BYTES_MIN = 1 << 3, // 6 to spare at least 64B
};

static struct zram *zram_driver = NULL;

inline static bool zram_object_is_raw(struct zram *zram, uint32_t page_id)
{
	return PAGE_SIZE == zram_get_obj_size(zram, page_id);
}

inline static bool zram_object_is_unique(struct zram *zram, uint32_t page_id)
{
	return zram_test_flag(zram, page_id, ZRAM_SDDC_UNIQUE);
}

inline static bool zram_object_is_duplicate(struct zram *zram, uint32_t page_id)
{
	return zram_test_flag(zram, page_id, ZRAM_SDDC_DUPLICATE);
}

inline static bool zram_object_is_proxy(struct zram *zram, uint32_t page_id)
{
	return (page_id >= zram->page_proxy_id_min);
}

static inline void ref_counter_inc(struct zram *zram, uint32_t page_id)
{
	if (page_id >= zram->page_proxy_id_min)
		atomic_inc(&zram_driver->ref_counter[page_id % zram->page_proxy_id_min].ref_count);
}

static inline void ref_counter_dec(struct zram *zram, uint32_t page_id)
{
	if (page_id >= zram->page_proxy_id_min)
		atomic_dec(&zram_driver->ref_counter[page_id % zram->page_proxy_id_min].ref_count);
}

static uint32_t zram_dereferenced_page(struct zram *zram, uint32_t page_id)
{
	return zram_object_is_duplicate(zram, page_id) ?
		       zram->table[page_id].dup_page_id :
		       page_id;
}

#define INITIAL_REF_COUNT 2
static void zram_dup_on_write(struct zram *zram, uint32_t page_id,
		uint32_t proxy_id)
{
	uint32_t ref_counter_index = proxy_id % zram->page_proxy_id_min;

	zram_slot_lock(zram_driver, proxy_id);
	/* Copy flags, handle, size to the proxy entry */
	zram->table[proxy_id].handle = zram->table[page_id].handle;
	/* page_id is locked */
	zram->table[proxy_id].flags = zram->table[page_id].flags;
	atomic_set(&zram->ref_counter[ref_counter_index].ref_count, INITIAL_REF_COUNT);
	zram_slot_unlock(zram_driver, proxy_id);
#ifdef CONFIG_HP_CORE
	hyperhold_untrack(zram, page_id);
#endif
	// Set flags
	zram_clear_flag(zram, page_id, ZRAM_HUGE);
	zram_clear_flag(zram, page_id, ZRAM_SDDC_UNIQUE);
	zram_clear_flag(zram, page_id, ZRAM_SDDC_DELTA);
	zram_set_flag(zram, page_id, ZRAM_SDDC_DUPLICATE);
	// Duplicate entry to proxy entry
	zram->table[page_id].dup_page_id = proxy_id;
	zram_set_obj_size(zram, page_id, 0);
	atomic64_inc(&zram->stats.num_duplicate);
}

inline static void zs_unmap_object_cond(struct zs_pool *pool, unsigned long handle, bool cond)
{
	if (cond)
		zs_unmap_object(pool, handle);
}

static int decode(struct zcomp_ext_strm *zstrm, sddcutils_state *self,
		struct page *page, uint32_t page_id, void *in);

static int decode_delta(struct zcomp_ext_strm *zstrm, sddcutils_state *self,
		struct page *page, unsigned long handle, unsigned int size,
		void *in)
{
	void *src = 0;
	void *dst = 0;
	uint32_t ref_page_id = 0;
	unsigned long ref_page_handle = 0;
	unsigned int ref_page_size = 0;
	unsigned int decoded_max = 0;
	void *ref_page = NULL;
	void *src_data = NULL;
	int ret = 0;

	src = in ? in : zs_map_object(zram_driver->mem_pool, handle, ZS_MM_RO);
	ref_page_id = *(uint32_t *)src;
	zs_unmap_object_cond(zram_driver->mem_pool, handle, !in);

	zram_slot_lock(zram_driver, ref_page_id);

	ref_page_size = zram_get_obj_size(zram_driver, ref_page_id);
	ref_page_handle = zram_get_handle(zram_driver, ref_page_id);
	ref_page = zs_map_object(zram_driver->mem_pool, ref_page_handle, ZS_MM_RO);
	memcpy((uint8_t *)self->decoded, ref_page, ref_page_size);
	zs_unmap_object(zram_driver->mem_pool, ref_page_handle);

	zram_slot_unlock(zram_driver, ref_page_id);

	src = in ? in : zs_map_object(zram_driver->mem_pool, handle, ZS_MM_RO);
	src_data = src + sizeof(ref_page_id);

	ret = zcomp_ext_decompress_delta(
		zstrm, src_data,
		size - sizeof(ref_page_id), self->decoded,
		(uint8_t *)self->decoded + ref_page_size,
		&decoded_max);

	zs_unmap_object_cond(zram_driver->mem_pool, handle, !in);

	if (ret == 0) {
		dst = kmap_atomic(page);
		if (PAGE_SIZE == decoded_max) {
			memcpy(dst, (uint8_t *)self->decoded + ref_page_size, PAGE_SIZE);
		} else if (decoded_max > 0 && decoded_max < PAGE_SIZE) {
			ret = zcomp_ext_decompress(
				zstrm,
				(uint8_t *)self->decoded + ref_page_size,
				decoded_max,
				dst);
		} else {
			pr_err("decode_delta() failed, page = %p, ref_page_id = %u\n",
				page, ref_page_id);
			BUG_ON(1);
		}
		kunmap_atomic(dst);
	}

	return ret;
}

static int decode(struct zcomp_ext_strm *zstrm, sddcutils_state *self,
		struct page *page, uint32_t page_id, void *in)
{
	uint32_t proxy_page_id = zram_dereferenced_page(zram_driver, page_id);
	unsigned long handle = 0;
	unsigned int size;
	void *src = 0;
	void *dst = 0;
	int ret = 0;

	if (!in)
#ifdef CONFIG_ZRAM_DEDUP
		handle = zram_get_direct_handle(zram_driver, proxy_page_id);
#else
		handle = zram_get_handle(zram_driver, proxy_page_id);
#endif
	if ((!handle && !in) || zram_test_flag(zram_driver, proxy_page_id, ZRAM_SAME))
		return -1;

	size = zram_get_obj_size(zram_driver, proxy_page_id);

	if (zram_object_is_raw(zram_driver, proxy_page_id)) { // Raw data
		src = in ? in : zs_map_object(zram_driver->mem_pool, handle, ZS_MM_RO);
		dst = kmap_atomic(page);
		memcpy(dst, src, PAGE_SIZE);
		kunmap_atomic(dst);
		zs_unmap_object_cond(zram_driver->mem_pool, handle, !in);
	} else if (zram_object_is_unique(zram_driver, proxy_page_id)) { // UNIQUE
		src = in ? in : zs_map_object(zram_driver->mem_pool, handle, ZS_MM_RO);
		dst = kmap_atomic(page);
		ret = zcomp_ext_decompress(zstrm, src, size, dst);
		kunmap_atomic(dst);
		zs_unmap_object_cond(zram_driver->mem_pool, handle, !in);
	} else { // DELTA
		ret = decode_delta(
			zstrm, self, page, handle, size, in);
	}

	return ret;
}

// SDDC utils APIs
bool sddcutils_dup_on_write(sddcutils_state *self, uint32_t page_id)
{
	int32_t proxy_id;
	if (zram_object_is_proxy(zram_driver, page_id)) {
		self->proxy_page_id = page_id;
		ref_counter_inc(zram_driver, self->proxy_page_id);
	} else if (zram_object_is_duplicate(zram_driver, page_id)) {
		self->proxy_page_id = zram_driver->table[page_id].dup_page_id;
		ref_counter_inc(zram_driver, self->proxy_page_id);
	} else { /* else delta, encoded or raw */
		proxy_id = zram_alloc_proxy(zram_driver);
		self->proxy_page_id = proxy_id > 0 ? proxy_id : 0;
		if (self->proxy_page_id)
			zram_dup_on_write(zram_driver, page_id, self->proxy_page_id);
	}

	if (self->proxy_page_id)
		self->encode_status = ENCODE_STATUS_DUPLICATE;

	return !!self->proxy_page_id;
}

uint32_t sddcutils_page_size(uint32_t page_id)
{
	page_id = zram_dereferenced_page(zram_driver, page_id);
	return zram_get_obj_size(zram_driver, page_id);
}

uint32_t sddcutils_obj_page_size(uint32_t page_id)
{
	return zram_get_obj_size(zram_driver, page_id);
}

uint32_t __sddcutils_page_ref_count(uint32_t page_id)
{
	if (page_id < zram_driver->page_proxy_id_min)
		return 0;

	page_id %= zram_driver->page_proxy_id_min;
	return atomic_read(&zram_driver->ref_counter[page_id].ref_count);
}

uint32_t sddcutils_page_ref_count(uint32_t page_id)
{
	uint32_t ref_counter_index = zram_dereferenced_page(zram_driver, page_id);

	return __sddcutils_page_ref_count(ref_counter_index);
}

const uint8_t* sddcutils_map_encoded(uint32_t page_id)
{
	uint32_t proxy_page_id = zram_dereferenced_page(zram_driver, page_id);
	unsigned long handle;
	uint8_t *ret = NULL;

	if (zram_test_flag(zram_driver, proxy_page_id, ZRAM_SAME) ||
	    zram_test_flag(zram_driver, proxy_page_id, ZRAM_SDDC_DELTA))
		goto out;

#ifdef CONFIG_ZRAM_DEDUP
	handle = zram_get_direct_handle(zram_driver, proxy_page_id);
#else
	handle = zram_get_handle(zram_driver, proxy_page_id);
#endif
	if (handle)
		ret = zs_map_object(zram_driver->mem_pool, handle, ZS_MM_RO);
out:
	return ret;
}

void sddcutils_unmap_encoded(uint32_t page_id)
{
	uint32_t proxy_page_id = zram_dereferenced_page(zram_driver, page_id);
	unsigned long handle;

	if (zram_test_flag(zram_driver, proxy_page_id, ZRAM_SAME) ||
	    zram_test_flag(zram_driver, proxy_page_id, ZRAM_SDDC_DELTA))
		return;

#ifdef CONFIG_ZRAM_DEDUP
	handle = zram_get_direct_handle(zram_driver, proxy_page_id);
#else
	handle = zram_get_handle(zram_driver, proxy_page_id);
#endif
	if (handle)
		zs_unmap_object(zram_driver->mem_pool, handle);
}

int sddcutils_encode(struct zcomp_ext_strm *zstrm, sddcutils_state *self,
		     struct page *page, unsigned int *comp_len)
{
	void *src;
	int ret;

	src = kmap_atomic(page);
	ret = zcomp_ext_compress(zstrm, src, self->encoded, comp_len);

	if (unlikely(!comp_len || *comp_len >= get_huge_class_size())) {
		*comp_len = PAGE_SIZE;
		memcpy(self->encoded, src, PAGE_SIZE);
	}
	kunmap_atomic(src);

	self->encode_status = ENCODE_STATUS_ENCODED;

	return ret;
}

void sddcutils_try_encode_delta(struct zcomp_ext_strm *zstrm, sddcutils_state *self,
			const void *in,	uint64_t in_max, uint32_t ref_page_id,
			unsigned int *comp_len)
{
	uint32_t proxy_page_id = 0;
	uint64_t ref_bytes_max = 0;
	unsigned long handle = 0;
	void *ref_page = NULL;
	unsigned int comp_len_delta = 0;
	int ret;

	zram_slot_lock(zram_driver, ref_page_id);

	if (zram_test_flag(zram_driver, ref_page_id, ZRAM_SDDC_DELTA) ||
	    zram_test_flag(zram_driver, ref_page_id, ZRAM_SAME) ||
	    sddcutils_under_wb(ref_page_id)) {
		zram_slot_unlock(zram_driver, ref_page_id);
		return;
	}

	proxy_page_id = zram_dereferenced_page(zram_driver, ref_page_id);
	// We do not get lock with proxy_page_id. Since we locked with ref_page_id it is guaranteed
	// that at least we have one referenced to the proxy_id so it cannot be deleted

#ifdef CONFIG_ZRAM_DEDUP
	handle = zram_get_direct_handle(zram_driver, proxy_page_id);
#else
	handle = zram_get_handle(zram_driver, proxy_page_id);
#endif
	if (!handle) {
		zram_slot_unlock(zram_driver, ref_page_id);
		return;
	}
	/* Copy the compressed reference page into the buffer */
	ref_bytes_max = zram_get_obj_size(zram_driver, proxy_page_id);
	ref_page = zs_map_object(zram_driver->mem_pool, handle, ZS_MM_RO);
	memcpy((uint8_t *)self->decoded, ref_page, ref_bytes_max);
	zs_unmap_object(zram_driver->mem_pool, handle);

	if (!sddcutils_dup_on_write(self, ref_page_id)) {
		zram_slot_unlock(zram_driver, ref_page_id);
		return;
	}

	self->encode_status = ENCODE_STATUS_NONE;
	proxy_page_id = self->proxy_page_id;
	zram_slot_unlock(zram_driver, ref_page_id);

	memcpy((uint8_t *)self->decoded + ref_bytes_max, in, in_max);

	ret = zcomp_ext_compress_delta(
		zstrm, self->decoded, (uint8_t *)self->decoded + ref_bytes_max, in_max,
		(uint8_t *)self->encoded + sizeof(proxy_page_id), &comp_len_delta,
		get_huge_class_size());
	if (ret || comp_len_delta < ENCODED_BYTES_MIN ||
	    comp_len_delta >= in_max - sizeof(proxy_page_id)) {
		dec_and_test_proxy(zram_driver, proxy_page_id);
		return;
	}

	memcpy(self->encoded, &proxy_page_id, sizeof(proxy_page_id));
	self->encode_status = ENCODE_STATUS_DELTA;
	*comp_len = comp_len_delta + sizeof(uint32_t);
}

int sddcutils_decode(struct zcomp_ext_strm *zstrm,
				sddcutils_state *self, struct page *page,
				uint32_t page_id, void *in)
{
	return decode(zstrm, self, page, page_id, in);
}

void sddcutils_init(struct zram *z)
{
	zram_driver = z;
}

void sddcutils_ctxt_delete(sddcutils_state *state)
{
	if (state) {
		if (state->decoded)
			free_pages((unsigned long)state->decoded, 1);
		if (state->encoded)
			free_pages((unsigned long)state->encoded, 1);

		kfree(state);
	}
}

sddcutils_state *sddcutils_ctxt_new(void)
{
	sddcutils_state *state = kzalloc(sizeof(sddcutils_state), GFP_KERNEL);

	if (!state)
		return NULL;

	state->encoded = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, 1);
	state->decoded = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, 1);

	if (!state->decoded || !state->encoded) {
		sddcutils_ctxt_delete(state);
		return NULL;
	}

	return state;
}

void *sddcutils_get_encoded(const sddcutils_state *const self)
{
	return self->encoded;
}

encode_status_t sddcutils_get_encode_status(const sddcutils_state *const self)
{
	return self->encode_status;
}

unsigned int sddcutils_get_proxy_id(const sddcutils_state *const self)
{
	return self->proxy_page_id;
}

void sddcutils_slot_lock(uint32_t index)
{
	zram_slot_lock(zram_driver, index);
}

void sddcutils_slot_unlock(uint32_t index)
{
	zram_slot_unlock(zram_driver, index);
}

bool sddcutils_under_wb(uint32_t index)
{
	index = zram_dereferenced_page(zram_driver, index);
#ifdef CONFIG_HP_CORE
	return zram_test_flag(zram_driver, index, ZRAM_UNDER_WB) ||
			zram_test_flag(zram_driver, index, ZRAM_WB) ||
			zram_test_flag(zram_driver, index, ZRAM_BATCHING_OUT);
#else
	return zram_test_flag(zram_driver, index, ZRAM_UNDER_WB) ||
			zram_test_flag(zram_driver, index, ZRAM_WB);
#endif
}
