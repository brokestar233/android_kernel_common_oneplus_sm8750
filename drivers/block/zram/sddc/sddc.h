/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2022. All rights reserved.
 * Description: Delta compression for ZRAM
 */

#ifndef _SDDC_H_
#define _SDDC_H_

#include <linux/types.h>
#include "zram_drv.h"
#include "sddc_utils.h"

/* This file provides sddc API for kernel/ZRAM side */


/*
 *   sddc_ctxt is opaque type, used in the sddc API.  Value
 *   of this type keeps all state variables and metadata on kernel side needed
 *   for the sddc API, e.g. storage (ZRAM) implementation for encoded/compressed
 *   page data, etc.
 */
typedef struct sddc_state sddc_state;
struct sddc_ctxt {
	sddc_state *state;
};

extern struct zcomp_ext *sddc_create(const char *comp, struct zram *zram, bool use_hw_crc32);
extern  void sddc_destroy(void *zcomp_ext);

extern void *sddc_get_encoded(const struct zcomp_ext_strm *zstrm);
extern encode_status_t sddc_get_encode_status(const struct zcomp_ext_strm *zstrm);
extern unsigned int sddc_get_proxy_id(const struct zcomp_ext_strm *zstrm);
extern bool sddc_is_under_wb(uint32_t index);
extern bool sddc_size_check(size_t size);

extern int sddc_encode(struct zcomp_ext_strm *zstrm, struct page *page,
		uint32_t page_id, unsigned int *comp_len,
		bool support_sddc);

extern void sddc_try_encode_delta(struct zcomp_ext_strm *zstrm, const void *in,
		uint64_t in_max, uint32_t page_id, unsigned int *comp_len);

extern int sddc_decode(struct zcomp_ext_strm *zstrm, struct page *page,
		uint32_t page_id, void *in);

#endif /* _SDDC_H_ */
