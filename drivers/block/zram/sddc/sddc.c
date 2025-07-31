/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2022. All rights reserved.
 * Description: Delta compression for ZRAM
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <trace/events/block.h> // Should be under CONFIG_ZSDDC_DEBUG
#include "lz4kd_private.h"
#include "sddc.h"

enum {
	ZRAM_PAGES_LOG2 = 35 - PAGE_SHIFT, // 40 to have 1TB for this PoC
	ZRAM_PAGES_MAX = 1 << ZRAM_PAGES_LOG2
};

enum {
	SAMPLE_BYTES_LOG2   = 4,
	SAMPLE_BYTES_MAX    = 1 << SAMPLE_BYTES_LOG2,
	/*
	  Size of sample. 16 bytes result in best CR due to similarity detection
          for known datasets.  For other datasets sample size may be tuned to
          different value.
        */

	IN_SAMPLES_LOG2     = PAGE_SHIFT - SAMPLE_BYTES_LOG2,
	IN_SAMPLES_MAX      = 1 << IN_SAMPLES_LOG2,
	/*
	  Max number of samples in input/ingested page.  This number may be different
	  from number of samples to be used to update hash table with info about page.
	  The reason is that hash table has limited size and using less samples to
	  update it would allow to have more pages represented in hash table.
	  The max number depends on page size and sample size.
	  So, the input page may be sampled with higher frequency to possibly find
	  more candidate pages for similarity comparison.
	*/

	/* samples to put in HT */
	HT_SAMPLES_LOG2     = 32 - ZRAM_PAGES_LOG2,
	HT_SAMPLES_MASK = (1U << HT_SAMPLES_LOG2) - 1U,
	HT_SAMPLE_STEP_LOG2 = PAGE_SHIFT - HT_SAMPLES_LOG2,
	HT_SAMPLE_STEP_MASK = (1U << HT_SAMPLE_STEP_LOG2) - 1U,
	/*
	  Min number of bytes in encoded page to store information about the page in
	  hash table.
	  The algorithm uses simpe heuristic to select what pages to store in hash table:
	  information about pages with small encoded size is not stored in hash table.
	  The reason for this heuristic is that presence of bigger pages in hash table
	  possibly allows to find bigger duplicates and therefore to spare more space.
	  Tunable parameter, changing its value may result in slightly better CR for
	  selected dataset. See similarity_bytes_min constant below.
	*/
	SEMI_PATTERN_BYTES_MAX = 32,
	/*
	  Min number of consecutive bytes with the same values in two pages to consider
	  pages similar, and therefore to use delta compression for input page.
	*/

	SIMILARITY_BYTES_MAX = PAGE_SIZE / 8,
	/*
	  Max number of consecutive bytes found equal for input page and one of stored
	  pages, to stop search for similar pages when using hash table.
	  With known datasets this parameter results in CR quite close to maximum CR that
	  can be achieved if search for similar pages is done for all pages found in hash
	  table.
	  Bigger value results in better CR, at the cost of encoding speed.
	*/

	HT_SLOT_TO_KEEP_REF_COUNT_MIN  = 2,
	XXX                 = 0
};

static uint32_t similarity_bytes_min = SAMPLE_BYTES_MAX * 1;
static uint32_t head_sample_offset = 1 << HT_SAMPLE_STEP_LOG2;
static uint32_t tail_sample_offset = 2 << HT_SAMPLE_STEP_LOG2;

enum {
	/*
	  Max number of page samples to calculate hashes and to update hash table with
	  page info.
	  The practical number of samples to update hash table may be quite small, e.g.
	  1 - 2 samples, to have reasonable diversity of of pages represented in hash
	  table.
	  However, if the hash table is big enough this number of samples may also be
	  quite large.
	  The max number depends on number of pages in ZRAM (or in any storage).
	  The reason for this dependency is that sample index is stored together with
	  page index in 32bit number.
	  Using 32bit numbers to store information in page allows to have bigger hash
	  table for given memory budget, as compared to using 64bit numbers.
	*/
	HT_BUCKETS_LOG2     = 16, // +1: no sample in bucket_cell
	HT_BUCKETS_MAX      = 1ULL << HT_BUCKETS_LOG2,
	BUCKET_CELLS_LOG2   = 3,
	BUCKET_CELLS_MAX    = 1ULL << BUCKET_CELLS_LOG2,
	HT_PAGE_BYTES_MIN   = 256,
};

enum {
	MATCH_PAGE_IDS_LOG2 = 2,
	MATCH_PAGE_IDS_MAX  = 1 << MATCH_PAGE_IDS_LOG2,
};

typedef struct in_sample {
	uint32_t hash;
	uint32_t offset; /* 0 for whole, 1 for start, 2 for end */
} in_sample;

typedef struct bucket_cell {
	uint32_t slot;
} bucket_cell;

typedef struct ext_cell {
	bucket_cell *cell;
	uint32_t old_page_id;
	uint32_t old_offset;
} ext_cell;

enum {
	HT_CELLS_MAX = 1ULL << (HT_BUCKETS_LOG2 + BUCKET_CELLS_LOG2)
};

typedef struct sddc_shared {
	bucket_cell ht[HT_BUCKETS_MAX][BUCKET_CELLS_MAX];
} sddc_shared;

static sddc_shared *shared = NULL;
static sddc_shared *shared2 = NULL;

typedef struct match_state {
	bucket_cell *cell;
	uint32_t old_page_id;
	uint32_t bytes_max;
	uint32_t ref_count;
} match_state;

struct sddc_state {
	sddcutils_state *ustate;
	in_sample in_samples[IN_SAMPLES_MAX];
	uint64_t in_samples_now;
	match_state match;
	bool similarity_check;
};

asmlinkage u32 __crc32c_le(u32 crc, unsigned char const *p, size_t len);
static bool use_hw_crc32_sddc;

static DEFINE_SPINLOCK(shared_lock);

static void lock_shared(void)
{
	spin_lock(&shared_lock);
}

static void unlock_shared(void)
{
	spin_unlock(&shared_lock);
}

static uint32_t hw_accl_crc(const uint8_t *in, size_t in_max, uint32_t seed)
{
	return __crc32c_le(seed, in, in_max);
}

static uint32_t murmur3_32_even(const uint8_t *in, size_t in_max, uint32_t seed)
{
	enum { STEP_LOG = 2 };
	uint32_t h = seed;
	size_t in_x4_max = in_max >> STEP_LOG;
	if (in_x4_max) {
		enum { STEP = 1 << (STEP_LOG - 2) }; /* 2 for uint32 */
		const uint32_t *in_x4 = (const uint32_t*)in;
		const uint32_t *in_x4_end = in_x4 + in_x4_max;
		do {
			uint32_t k = *in_x4;
			k *= 0xcc9e2d51;
			k = (k << 15) | (k >> 17);
			k *= 0x1b873593;
			h ^= k;
			h = (h << 13) | (h >> 19);
			h = (h * 5) + 0xe6546b64;
		} while ((in_x4 += STEP) < in_x4_end);
	}
	/* hashing the odd bytes in tail makes CR a bit worse */
	h ^= in_max;
	h ^= h >> 16;
	h *= 0x85ebca6b;
	h ^= h >> 13;
	h *= 0xc2b2ae35;
	h ^= h >> 16;
	return h;
}

static uint32_t hash_for_encoded(
	const uint8_t *encoded,
	uint64_t encoded_max)
{
	uint32_t h;
	if (use_hw_crc32_sddc)
		h = hw_accl_crc(encoded, encoded_max, ~(uint32_t)0);
	else
		h = murmur3_32_even(encoded, encoded_max, 506832829U);
	return h & mask(HT_BUCKETS_LOG2);
}

static uint32_t hash_for_sample(const uint8_t *sample_at)
{
	uint32_t hash;
	if (use_hw_crc32_sddc)
		hash = hw_accl_crc(sample_at, SAMPLE_BYTES_MAX, ~(uint32_t)0);
	else
		hash = murmur3_32_even(sample_at, SAMPLE_BYTES_MAX, 506832829U);
	return hash & mask(HT_BUCKETS_LOG2);
}

static uint64_t slot_page_id(uint64_t bs)
{
	return bs & mask64(ZRAM_PAGES_LOG2);
}

static uint32_t slot_sample_offset(uint64_t bs)
{
	return ((bs >> ZRAM_PAGES_LOG2) & HT_SAMPLES_MASK)
		<< HT_SAMPLE_STEP_LOG2;
}

/*
   Bucket slot is constructed of:
   1. page_id
   2. Sample offset for HT
*/
static uint64_t slot_of(uint64_t page_id, uint64_t sample_offset)
{
	return ((sample_offset >> HT_SAMPLE_STEP_LOG2) << ZRAM_PAGES_LOG2) |
	       page_id;
}

static bool is_slot_filled(uint64_t bs)
{
	return bs != 0;
}

static bool is_1st_slot_better_than_2nd(sddc_state *const self,
					uint64_t bs0, uint64_t bs1,
					uint64_t ref_count_min)
{
	uint32_t ref_count0, ref_count1, page_size0, page_size1;
	const uint64_t page_id0 = slot_page_id(bs0);
	const uint64_t page_id1 = slot_page_id(bs1);

	if (page_id0 == page_id1)
		return false;

	ref_count0 = __sddcutils_page_ref_count(page_id0);
	ref_count1 = __sddcutils_page_ref_count(page_id1);
	if (ref_count0 < ref_count1)
		return false;

	if (ref_count0 > ref_count1)
		return true;

	page_size0 = sddcutils_obj_page_size(page_id0);
	page_size1 = sddcutils_obj_page_size(page_id1);
	/* heuristics: results in better CR */
	return page_size0 > page_size1 || ref_count0 > ref_count_min ||
		page_id0 == self->match.old_page_id; /* keep old_page for proxy_id */
}

static void store_in_samples_in_ht(
	sddc_state *const self,
	const uint64_t page_id)
{
	uint64_t ref_count_min = HT_SLOT_TO_KEEP_REF_COUNT_MIN;
	const in_sample *is = self->in_samples;
	bucket_cell *cell = shared->ht[is->hash];
	bucket_cell *const bucket_end = cell + BUCKET_CELLS_MAX;
	bucket_cell *cell_to_replace = cell;

	/* iterate all slots in bucket to find bucket to replace */
	do {
		if (!is_slot_filled(cell->slot)) {
			goto FILL_BUCKET_SLOT;
		}
		if (is_1st_slot_better_than_2nd(self, cell_to_replace->slot,
						cell->slot, ref_count_min))
			cell_to_replace = cell;
	} while (++cell < bucket_end);

	cell = cell_to_replace;
FILL_BUCKET_SLOT:
	cell->slot = slot_of(page_id, is->offset);
}

static void store_in_samples_in_ht2(
	sddc_state *const self,
	const uint64_t page_id)
{
	uint64_t ref_count_min = HT_SLOT_TO_KEEP_REF_COUNT_MIN;
	const in_sample *is = self->in_samples + 1;
	const in_sample *const is_end = is + 2;

	for (; is < is_end; ++is) {
		bucket_cell *cell = shared2->ht[is->hash];
		bucket_cell *const bucket_end = cell + BUCKET_CELLS_MAX;
		bucket_cell *cell_to_replace = cell;

		/* iterate all slots in bucket to find bucket to replace */
		do {
			if (!is_slot_filled(cell->slot)) {
				goto FILL_BUCKET_SLOT;
			}
			if (is_1st_slot_better_than_2nd(self, cell_to_replace->slot,
							cell->slot, ref_count_min))
				cell_to_replace = cell;
		} while (++cell < bucket_end);

		cell = cell_to_replace;
FILL_BUCKET_SLOT:
		cell->slot = slot_of(page_id, is->offset);
	}
}

enum { E_MAX = 1 << 4 }; /* must be <= SEMI_PATTERN_BYTES_MAX */

static bool bytes_match0(
	const uint8_t *const a,
	const uint8_t *const b,
	uint_fast32_t ab_max)
{
	return  memcmp(a, b, E_MAX) == 0 &&
		memcmp(a + ab_max - E_MAX, b + ab_max - E_MAX, E_MAX) == 0 &&
		memcmp(a + E_MAX, b + E_MAX, ab_max - 2 * E_MAX) == 0;
}

static bool bytes_match(
	const uint8_t *const a,
	const uint8_t *const b,
	uint_fast32_t bytes_max)
{
	return memcmp(a, b, bytes_max) == 0;
}

enum { MATCH_MIN = 1 << 3 }; /* faster, a bit lower CR */

static void check_match_bytes(
	sddc_state *self,
	uint64_t match_bytes_max,
	ext_cell *xc)
{
	match_state *const match = &self->match;
	uint32_t ref_count = sddcutils_page_ref_count(xc->old_page_id);

	if (match->bytes_max < match_bytes_max ||
	    (match->bytes_max == match_bytes_max &&
	     match->ref_count < ref_count)) {
		match->bytes_max = match_bytes_max;
		match->old_page_id = xc->old_page_id;
		match->cell = xc->cell;
		match->ref_count = ref_count;
	}
}

/* xc->old_page_id should be locked */
static void handle_dup_matched(
	sddc_state *self,
	ext_cell *xc)
{
	match_state *const match = &self->match;
	/* If dup_on_write fails - proxy_table is full, try other page */
	if (sddcutils_dup_on_write(self->ustate, xc->old_page_id)) {
		match->bytes_max = PAGE_SIZE;
		match->old_page_id = xc->old_page_id;
		match->cell = xc->cell;
	}
}

static void match_pages_partially(
	sddc_state *self,
	const uint8_t *in_page,
	uint64_t encoded_max,
	ext_cell *xc,
	const uint8_t *old_page,
	uint64_t old_page_bytes_max)
{
	/* caller guarantees match_bytes_max < PAGE_SIZE) */
	const uint8_t *in_page_end = in_page + encoded_max;
	uint64_t match_bytes_max = 0; /* same head or tail */
	const uint8_t *old_page_end = old_page + old_page_bytes_max;
	if (head_sample_offset == xc->old_offset) { /* head samples are the same */
		match_bytes_max = SAMPLE_BYTES_MAX;
		in_page += SAMPLE_BYTES_MAX;
		old_page += SAMPLE_BYTES_MAX;

		while (in_page + MATCH_MIN <= in_page_end &&
			old_page + MATCH_MIN <= old_page_end) {
				if (bytes_match(old_page, in_page, MATCH_MIN))
					match_bytes_max += MATCH_MIN;
			old_page += MATCH_MIN;
			in_page += MATCH_MIN;
		}
		check_match_bytes(self, match_bytes_max, xc);
	}
	if (tail_sample_offset == xc->old_offset) { /* tail samples the same */
		match_bytes_max = SAMPLE_BYTES_MAX;
		in_page_end -= SAMPLE_BYTES_MAX;
		old_page_end -= SAMPLE_BYTES_MAX;

		while (in_page_end - MATCH_MIN >= in_page &&
			old_page_end - MATCH_MIN >= old_page) {
			if (bytes_match(old_page_end - MATCH_MIN,
				in_page_end - MATCH_MIN, MATCH_MIN))
				match_bytes_max += MATCH_MIN;

			old_page_end -= MATCH_MIN;
			in_page_end -= MATCH_MIN;
		}
		check_match_bytes(self, match_bytes_max, xc);
	}
}

static void match_page(
	sddc_state *self,
	const uint8_t *in_page,
	uint64_t encoded_max,
	ext_cell *xc)
{
	const uint8_t *in_page_end = in_page + encoded_max;
	const uint32_t old_page_id = xc->old_page_id;
	uint64_t old_page_bytes_max = 0;
	const uint8_t *old_page_end = 0;
	const uint8_t *old_page = 0;

	sddcutils_slot_lock(old_page_id);
	if (sddcutils_under_wb(old_page_id))
		goto unlock_page;
	old_page_bytes_max =
		sddcutils_page_size(old_page_id);
	if (xc->old_offset == head_sample_offset &&
		old_page_bytes_max != encoded_max &&
		(old_page_bytes_max == PAGE_SIZE ||
		encoded_max == PAGE_SIZE))
		goto unlock_page; /* do not compare encoded and non-encoded */
	old_page = sddcutils_map_encoded(old_page_id);
	if (!old_page)
		goto unlock_page;
	if (head_sample_offset == xc->old_offset &&
		!bytes_match(in_page, old_page, SAMPLE_BYTES_MAX))
		goto release_page;
	old_page_end  = old_page + old_page_bytes_max;
	if (tail_sample_offset == xc->old_offset &&
		!bytes_match(in_page_end - SAMPLE_BYTES_MAX,
		old_page_end - SAMPLE_BYTES_MAX,
		SAMPLE_BYTES_MAX))
		goto release_page;
	/* here we have head or tail samples match */
	if (old_page_bytes_max == encoded_max &&
		bytes_match0(in_page, old_page, encoded_max)) {
		handle_dup_matched(self, xc);
		goto release_page;
	}
	match_pages_partially(
		self, in_page, encoded_max, xc, old_page, old_page_bytes_max);
release_page:
	sddcutils_unmap_encoded(old_page_id);
unlock_page:
	sddcutils_slot_unlock(old_page_id);
}

static void match_page0(
	sddc_state *self,
	const uint8_t *in_page,
	uint64_t encoded_max,
	ext_cell *xc)
{
	const uint32_t old_page_id = xc->old_page_id;
	const uint8_t *old_page = 0;

	sddcutils_slot_lock(old_page_id);
	if (sddcutils_under_wb(old_page_id))
		goto unlock_page;
	if (encoded_max != sddcutils_page_size(old_page_id))
		goto unlock_page;
	old_page = sddcutils_map_encoded(old_page_id);
	if (!old_page)
		goto unlock_page;
	if (bytes_match0(in_page, old_page, encoded_max))
		handle_dup_matched(self, xc);
	sddcutils_unmap_encoded(old_page_id);
unlock_page:
	sddcutils_slot_unlock(old_page_id);
}

static void fill_ext_cell(
	ext_cell *const xc,
	bucket_cell *const cell,
	uint32_t old_offset)
{
	xc->cell = cell;
	xc->old_offset = old_offset;
}

static void add_in_samples(
	sddc_state *const self,
	const uint8_t *encoded,
	uint64_t encoded_max)
{
	in_sample *is = self->in_samples;
	is->hash = hash_for_encoded(encoded, encoded_max);
	is->offset = 0;
	self->in_samples_now = 1;
}

static void add_in_samples2(
	sddc_state *const self,
	const uint8_t *encoded,
	uint64_t encoded_max)
{
	in_sample *is = self->in_samples;
	is[1].hash = hash_for_sample(encoded);
	is[1].offset = head_sample_offset;
	is[2].hash = hash_for_sample(encoded + encoded_max -
		SAMPLE_BYTES_MAX);
	is[2].offset = tail_sample_offset;
	self->in_samples_now = 3;
}

static void check_page_duplicate(
	sddc_state *const self,
	uint32_t page_id,
	const uint8_t *encoded,
	uint64_t encoded_max)
{
	in_sample *is = self->in_samples;
	bucket_cell *cell = shared->ht[is->hash];
	bucket_cell *const bucket_end = cell + BUCKET_CELLS_MAX;
	uint64_t old_page_id = 0;
	ext_cell xc[1];

	memset(xc, 0, sizeof(struct ext_cell));

	do {
		if (!is_slot_filled(cell->slot))
			continue;

		old_page_id = slot_page_id(cell->slot);
		if (old_page_id == page_id)
			continue;

		xc->old_page_id = old_page_id;
		fill_ext_cell(xc, cell, 0);
		match_page0(self, encoded, encoded_max, xc);
		if (self->match.bytes_max == PAGE_SIZE)
			return;
	} while (++cell < bucket_end);
}

static void get_matching_ext_cells(
	sddc_state *self,
	uint32_t page_id,
	const in_sample *const is,
	const uint8_t *encoded,
	uint64_t encoded_max)
{
	bucket_cell *cell = shared2->ht[is->hash];
	bucket_cell *const bucket_end = cell + BUCKET_CELLS_MAX;
	int64_t old_page_id = 0;
	int64_t old_offset = 0;
	ext_cell xc[1];

	memset(xc, 0, sizeof(struct ext_cell));
	do {
		if (!is_slot_filled(cell->slot))
			continue;

		old_page_id = slot_page_id(cell->slot);
		if (old_page_id == page_id)
			continue;

		old_offset = slot_sample_offset(cell->slot);
		if (old_offset != is->offset)
			continue;

		xc->old_page_id = old_page_id;
		fill_ext_cell(xc, cell, old_offset);
		match_page(self, encoded, encoded_max, xc);
	} while ((++cell < bucket_end) && (self->match.bytes_max != PAGE_SIZE));
}

static void check_page_similarity(
	sddc_state *const self,
	uint32_t page_id,
	const uint8_t *encoded,
	uint64_t encoded_max)
{
	in_sample *is = self->in_samples;
	in_sample *is_end = is + self->in_samples_now;

	for (++is; is < is_end && self->match.bytes_max != PAGE_SIZE; ++is)
		get_matching_ext_cells(self, page_id, is, encoded, encoded_max);
}

static void update_ht_with_proxy_id(sddc_state *const self, uint32_t proxy_id)
{
	uint64_t sample_offset = 0;
	if (!self->match.cell || self->match.old_page_id == proxy_id)
		return;

	lock_shared();
	sample_offset = slot_sample_offset(self->match.cell->slot);

	if (slot_page_id(self->match.cell->slot) == self->match.old_page_id)
		self->match.cell->slot = slot_of(proxy_id, sample_offset);
	unlock_shared();
}

int sddc_encode(
	struct zcomp_ext_strm *zstrm,
	struct page *page,
	uint32_t page_id,
	unsigned int *comp_len,
	bool support_sddc)
{
	struct sddc_state *self = ((struct sddc_ctxt *)zstrm->ctxt)->state;
	const uint8_t *encoded = self->ustate->encoded;
	int ret = 0;

	if (!support_sddc)
		return sddcutils_encode(zstrm, self->ustate, page, comp_len);

	self->match.bytes_max = 0;
	self->match.cell = 0;
	self->match.old_page_id = 0;

	ret = sddcutils_encode(zstrm, self->ustate, page, comp_len);
	if (ret == 0 && *comp_len > SEMI_PATTERN_BYTES_MAX) {
		add_in_samples(self, encoded, *comp_len);
		check_page_duplicate(self, page_id, encoded, *comp_len);
	}

	if (self->match.bytes_max == PAGE_SIZE) {
		update_ht_with_proxy_id(self,
				self->ustate->proxy_page_id);
		return 0;
	}
	if (ret == 0 && *comp_len > HT_PAGE_BYTES_MIN)
		store_in_samples_in_ht(self, page_id);

	return ret;
}

void sddc_try_encode_delta(
	struct zcomp_ext_strm *zstrm,
	const void *in,
	uint64_t in_max,
	uint32_t page_id,
	unsigned int *comp_len)
{
	struct sddc_state *self = ((struct sddc_ctxt *)zstrm->ctxt)->state;
	uint32_t ref_page_id;

	self->match.bytes_max = 0;
	self->match.cell = 0;
	self->match.old_page_id = 0;
	self->ustate->encode_status = ENCODE_STATUS_NONE;

	add_in_samples2(self, in, in_max);
	check_page_similarity(self, page_id, in, in_max);

	if (self->match.bytes_max == PAGE_SIZE) {
		update_ht_with_proxy_id(self, self->ustate->proxy_page_id);
		return;
	}
	if (in_max > HT_PAGE_BYTES_MIN && in_max < PAGE_SIZE)
		store_in_samples_in_ht2(self, page_id);

	if (self->match.bytes_max < similarity_bytes_min)
		return;

	if (self->match.bytes_max == 0 && self->match.old_page_id == 0)
		return;

	ref_page_id = self->match.old_page_id;

	if (unlikely(page_id == ref_page_id)) {
		pr_warn("SDDC: delta encoding with same page: page id %u, ref pageid %u\n",
			page_id, ref_page_id);
		BUG_ON(1);
	}

	sddcutils_try_encode_delta(
		zstrm, self->ustate, in, in_max, ref_page_id, comp_len);

	if (self->ustate->encode_status == ENCODE_STATUS_DELTA)
		update_ht_with_proxy_id(self, self->ustate->proxy_page_id);
}

int sddc_decode(struct zcomp_ext_strm *zstrm, struct page *page,
		uint32_t page_id, void *in)
{
	struct sddc_ctxt *ctxt = zstrm->ctxt;
	return sddcutils_decode(
		zstrm, ctxt->state->ustate, page, page_id, in);
}

static void sddc_ctxt_delete(void *sddc_ctxt)
{
	struct sddc_ctxt *ctxt = sddc_ctxt;
	if (ctxt) {
		if (ctxt->state) {
			sddcutils_ctxt_delete(ctxt->state->ustate);
			kfree(ctxt->state);
		}
		kfree(ctxt);
	}
}

static void *sddc_ctxt_new(void)
{
	struct sddc_ctxt *ctxt = NULL;
	sddc_state *state = NULL;
	sddcutils_state *ustate = NULL;
	ctxt = kzalloc(sizeof(*ctxt), GFP_KERNEL);
	if (!ctxt)
		return NULL;
	if (!(ustate = sddcutils_ctxt_new())) {
		sddc_ctxt_delete(ctxt);
		return NULL;
	}
	if (!(state = kzalloc(sizeof(*state), GFP_KERNEL))) {
		sddc_ctxt_delete(ctxt);
		sddcutils_ctxt_delete(ustate);
		return NULL;
	}
	state->ustate = ustate;
	ctxt->state = state;
	return (void *)ctxt;
}

struct zcomp_ext *sddc_create(const char *comp, struct zram *zram, bool use_hw_crc32)
{
	struct zcomp_ext *ret = NULL;

	use_hw_crc32_sddc = use_hw_crc32;
	ret = zcomp_ext_create(comp, sddc_ctxt_new, sddc_ctxt_delete);
	if (IS_ERR(ret)) {
		pr_err("Can't create zcomp_ext");
		return ret;
	}

	sddcutils_init(zram);

	if (zcomp_ext_check_prop(ret, ZCOMP_EXT_DELTA_COMP)) {
		if (!shared)
			shared = vzalloc(sizeof(*shared));
		if (!shared2)
			shared2 = vzalloc(sizeof(*shared2));

		if (!shared || !shared2) {
			zcomp_ext_destroy(ret);
			if (shared) {
				vfree(shared);
				shared = NULL;
			}
			if (shared2) {
				vfree(shared2);
				shared2 = NULL;
			}
			ret = ERR_PTR(-ENOMEM);
		}
	}

	return ret;
}

void sddc_destroy(void *zcomp_ext)
{
	struct zcomp_ext *comp = zcomp_ext;
	zcomp_ext_destroy(comp);
	if (shared)
		vfree(shared);
	if (shared2)
		vfree(shared2);
	shared = NULL;
	shared2 = NULL;
}

void *sddc_get_encoded(const struct zcomp_ext_strm *zstrm)
{
	const struct sddc_ctxt *ctxt = zstrm->ctxt;
	return sddcutils_get_encoded(ctxt->state->ustate);
}

encode_status_t sddc_get_encode_status(const struct zcomp_ext_strm *zstrm)
{
	const struct sddc_ctxt *ctxt = zstrm->ctxt;
	return sddcutils_get_encode_status(ctxt->state->ustate);
}

unsigned int sddc_get_proxy_id(const struct zcomp_ext_strm *zstrm)
{
	const struct sddc_ctxt *ctxt = zstrm->ctxt;
	return sddcutils_get_proxy_id(ctxt->state->ustate);
}

bool sddc_is_under_wb(uint32_t index)
{
	return sddcutils_under_wb(index);
}

bool sddc_size_check(size_t size)
{
	return size > SIMILARITY_BYTES_MAX;
}
