/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2022. All rights reserved.
 * Description: Support delta compression for ZRAM
 */

#ifndef _ZCOMP_EXT_H_
#define _ZCOMP_EXT_H_

/*
 * The list of properties are supported by compressor choosen.
 * Currently we support only delta compression and deduplication.
 * It can be extended to support other properties (for example,
 * dictionary)
 */
enum zcomp_ext_props {
	ZCOMP_EXT_DELTA_COMP,

	__NR_ZCOMP_EXT_PROPS,
};

struct zcomp_ext_strm {
	void *ctxt;
	struct crypto_comp *tfm;
};

typedef void *(*CtxtAlloc)(void);
typedef void (*CtxtFree)(void *);
/* dynamic per-device compression frontend */
struct zcomp_ext {
	struct zcomp_ext_strm *__percpu *stream;
	const char *name;
	CtxtAlloc ctxt_alloc;
	CtxtFree ctxt_free;
	unsigned props;
	struct hlist_node node;
};

int zcomp_ext_cpu_up_prepare(unsigned int cpu, struct hlist_node *node);
int zcomp_ext_cpu_dead(unsigned int cpu, struct hlist_node *node);
ssize_t zcomp_ext_available_show(const char *comp, char *buf);
bool zcomp_ext_available_algorithm(const char *comp);

struct zcomp_ext *zcomp_ext_create(const char *comp, CtxtAlloc ctxt_alloc,
				   CtxtFree ctxt_free);

void zcomp_ext_destroy(struct zcomp_ext *comp);

struct zcomp_ext_strm *zcomp_ext_stream_get(struct zcomp_ext *comp);
void zcomp_ext_stream_put(struct zcomp_ext *comp);

int zcomp_ext_compress(struct zcomp_ext_strm *zstrm, const void *src, void *dst,
		       unsigned int *dst_len);

int zcomp_ext_decompress(struct zcomp_ext_strm *zstrm, const void *src,
			 unsigned int src_len, void *dst);

int zcomp_ext_compress_delta(struct zcomp_ext_strm *zstrm, const void *src0,
			     const void *src, unsigned int src_len, void *dst,
			     unsigned int *dst_len, unsigned int out_max);

int zcomp_ext_decompress_delta(struct zcomp_ext_strm *zstrm, const void *src,
			       unsigned int src_len, const void *dst0,
			       void *dst, unsigned int *dst_len);

bool zcomp_ext_set_max_streams(struct zcomp_ext *comp, int num_strm);

static inline void zcomp_ext_set_prop(struct zcomp_ext *comp,
				      enum zcomp_ext_props prop)
{
	comp->props |= 1U << prop;
}

static inline void zcomp_ext_clear_prop(struct zcomp_ext *comp,
					enum zcomp_ext_props prop)
{
	comp->props &= ~(1U << prop);
}

static inline bool zcomp_ext_check_prop(struct zcomp_ext *comp,
					enum zcomp_ext_props prop)
{
	return comp->props & (1U << prop);
}

#endif /* _ZCOMP_EXT_H_ */
