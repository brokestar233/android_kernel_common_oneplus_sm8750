/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2022. All rights reserved.
 * Description: Support delta compression for ZRAM
 */

#ifndef _SDDC_UTILS_H_
#define _SDDC_UTILS_H_

#include <linux/types.h>
#include <linux/bitops.h>
#include "zcomp_ext.h"

typedef enum {
	ENCODE_STATUS_RAW = 1 << 0,
	ENCODE_STATUS_DUPLICATE = 1 << 1,
	ENCODE_STATUS_ENCODED = 1 << 2,
	ENCODE_STATUS_DELTA = 1 << 3,
	ENCODE_STATUS_PATTERN = 1 << 4,
	ENCODE_STATUS_PROXY = 1 << 5,
	ENCODE_STATUS_LOG2 = 6,
	ENCODE_STATUS_SHIFT = 64 - ENCODE_STATUS_LOG2,
	ENCODE_STATUS_NONE =
		1 << (ENCODE_STATUS_LOG2 + 1) /* to have any other status ==0 */
} encode_status_t;

/*
  sddcutils_state is opaque type, used in the sddcutils API.  Value
  of this type keeps all state variables and metadata on side
  needed for sddcutils_ API.
 */
typedef struct sddcutils_state {
	void *decoded; /* [PAGE_SIZE*2] */
	void *encoded; /* [PAGE_SIZE*2] */
	uint32_t proxy_page_id;
	encode_status_t encode_status;
} sddcutils_state;

extern sddcutils_state *sddcutils_ctxt_new(void);
extern void sddcutils_ctxt_delete(sddcutils_state *);
extern void sddcutils_init(struct zram *zram);

extern uint32_t __sddcutils_page_ref_count(uint32_t page_id);
extern uint32_t sddcutils_page_ref_count(uint32_t page_id);
extern uint32_t sddcutils_obj_page_size(uint32_t page_id);
extern uint32_t sddcutils_page_size(uint32_t page_id);
extern bool sddcutils_dup_on_write(sddcutils_state *self, uint32_t page_id);

extern void *sddcutils_get_encoded(const sddcutils_state *const self);
extern encode_status_t sddcutils_get_encode_status(const sddcutils_state *const self);
extern unsigned int sddcutils_get_proxy_id(const sddcutils_state *const self);

extern int sddcutils_encode(struct zcomp_ext_strm *zstrm, sddcutils_state *self,
		     struct page *page, unsigned int *comp_len);

extern void sddcutils_try_encode_delta(struct zcomp_ext_strm *zstrm, sddcutils_state *self,
			   const void *in, uint64_t in_max, uint32_t ref_page_id,
			   unsigned int *comp_len);

extern int sddcutils_decode(struct zcomp_ext_strm *zstrm, sddcutils_state *self,
		     struct page *page, uint32_t page_id, void *in);

extern void sddcutils_slot_lock(uint32_t index);
extern void sddcutils_slot_unlock(uint32_t index);

extern const uint8_t* sddcutils_map_encoded(uint32_t page_id);
extern void sddcutils_unmap_encoded(uint32_t page_id);

extern bool sddcutils_under_wb(uint32_t index);

#endif /* _SDDC_UTILS_H_ */
