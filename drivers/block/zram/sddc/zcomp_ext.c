/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2022. All rights reserved.
 * Description: Support delta compression for ZRAM
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/cpu.h>
#include <linux/crypto.h>

#include "zcomp_ext.h"

static const char * const backends[] = {
#if IS_ENABLED(CONFIG_CRYPTO_LZO)
	"lzo",
	"lzo-rle",
#endif
#if IS_ENABLED(CONFIG_CRYPTO_LZ4)
	"lz4",
#endif
#if IS_ENABLED(CONFIG_CRYPTO_LZ4HC)
	"lz4hc",
#endif
#if IS_ENABLED(CONFIG_CRYPTO_LZ4K)
	"lz4k",
#endif
 #if IS_ENABLED(CONFIG_CRYPTO_LZ4K_OPLUS)
	"lz4k_oplus",
 #endif
#if IS_ENABLED(CONFIG_CRYPTO_LZ4KD)
	"lz4kd",
#endif
#if IS_ENABLED(CONFIG_CRYPTO_DEFLATE)
	"deflate",
#endif
#if IS_ENABLED(CONFIG_CRYPTO_842)
	"842",
#endif
#if IS_ENABLED(CONFIG_CRYPTO_ZSTD)
	"zstd",
#endif
	NULL
};

static void zcomp_ext_strm_free(struct zcomp_ext *comp,
				struct zcomp_ext_strm *zstrm)
{
	if (!IS_ERR_OR_NULL(zstrm->tfm))
		crypto_free_comp(zstrm->tfm);
	comp->ctxt_free(zstrm->ctxt);
	kfree(zstrm);
}

/*
 * allocate new zcomp_ext_strm structure with ->tfm initialized by
 * backend, return NULL on error
 */
static struct zcomp_ext_strm *zcomp_ext_strm_alloc(struct zcomp_ext *comp)
{
	struct zcomp_ext_strm *zstrm = kmalloc(sizeof(*zstrm), GFP_KERNEL);
	if (!zstrm)
		return NULL;

	zstrm->tfm = crypto_alloc_comp(comp->name, 0, 0);

	zstrm->ctxt = comp->ctxt_alloc();
	if (IS_ERR_OR_NULL(zstrm->tfm) || !zstrm->ctxt) {
		zcomp_ext_strm_free(comp, zstrm);
		zstrm = NULL;
	}
	return zstrm;
}

bool zcomp_ext_available_algorithm(const char *comp)
{
	int i;

	i = __sysfs_match_string(backends, -1, comp);
	if (i >= 0)
		return true;

	/*
	 * Crypto does not ignore a trailing new line symbol,
	 * so make sure you don't supply a string containing
	 * one.
	 * This also means that we permit zcomp initialisation
	 * with any compressing algorithm known to crypto api.
	 */
	return crypto_has_comp(comp, 0, 0) == 1;
}

/* show available compressors */
ssize_t zcomp_ext_available_show(const char *comp, char *buf)
{
	bool known_algorithm = false;
	ssize_t sz = 0;
	int i = 0;

	for (; backends[i]; i++) {
		if (!strcmp(comp, backends[i])) {
			known_algorithm = true;
			sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2, "[%s] ",
					backends[i]);
		} else {
			sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2, "%s ",
					backends[i]);
		}
	}

	/*
	 * Out-of-tree module known to crypto api or a missing
	 * entry in `backends'.
	 */
	if (!known_algorithm && crypto_has_comp(comp, 0, 0) == 1)
		sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2, "[%s] ", comp);

	sz += scnprintf(buf + sz, PAGE_SIZE - sz, "\n");
	return sz;
}

struct zcomp_ext_strm *zcomp_ext_stream_get(struct zcomp_ext *comp)
{
	return *get_cpu_ptr(comp->stream);
}

void zcomp_ext_stream_put(struct zcomp_ext *comp)
{
	put_cpu_ptr(comp->stream);
}

int zcomp_ext_compress(struct zcomp_ext_strm *zstrm, const void *src, void *dst,
		       unsigned int *dst_len)
{
	/*
	 * Our dst memory (zstrm->buffer) is always `2 * PAGE_SIZE' sized
	 * because sometimes we can endup having a bigger compressed data
	 * due to various reasons: for example compression algorithms tend
	 * to add some padding to the compressed buffer. Speaking of padding,
	 * comp algorithm `842' pads the compressed length to multiple of 8
	 * and returns -ENOSP when the dst memory is not big enough, which
	 * is not something that ZRAM wants to see. We can handle the
	 * `compressed_size > PAGE_SIZE' case easily in ZRAM, but when we
	 * receive -ERRNO from the compressing backend we can't help it
	 * anymore. To make `842' happy we need to tell the exact size of
	 * the dst buffer, zram_drv will take care of the fact that
	 * compressed buffer is too big.
	 */
	*dst_len = PAGE_SIZE * 2;

	return crypto_comp_compress(zstrm->tfm, src, PAGE_SIZE, dst, dst_len);
}

int zcomp_ext_decompress(struct zcomp_ext_strm *zstrm, const void *src,
			 unsigned int src_len, void *dst)
{
	unsigned int dst_len = PAGE_SIZE;

	return crypto_comp_decompress(zstrm->tfm, src, src_len, dst, &dst_len);
}

int zcomp_ext_compress_delta(struct zcomp_ext_strm *zstrm, const void *src0,
			     const void *src, unsigned int src_len, void *dst,
			     unsigned int *dst_len, unsigned int out_max)
{
	*dst_len = PAGE_SIZE * 2 - sizeof(uint32_t);

	return crypto_comp_compress_delta(zstrm->tfm, src0, src, src_len, dst,
					  dst_len, out_max);
}

int zcomp_ext_decompress_delta(struct zcomp_ext_strm *zstrm, const void *src,
			       unsigned int src_len, const void *dst0,
			       void *dst, unsigned int *dst_len)
{
	*dst_len = PAGE_SIZE;

	return crypto_comp_decompress_delta(zstrm->tfm, src, src_len, dst0, dst,
					    dst_len);
}

int zcomp_ext_cpu_up_prepare(unsigned int cpu, struct hlist_node *node)
{
	struct zcomp_ext *comp = hlist_entry(node, struct zcomp_ext, node);
	struct zcomp_ext_strm *zstrm = NULL;

	if (WARN_ON(*per_cpu_ptr(comp->stream, cpu)))
		return 0;

	zstrm = zcomp_ext_strm_alloc(comp);
	if (IS_ERR_OR_NULL(zstrm)) {
		pr_err("Can't allocate a compression stream\n");
		return -ENOMEM;
	}
	*per_cpu_ptr(comp->stream, cpu) = zstrm;
	return 0;
}

int zcomp_ext_cpu_dead(unsigned int cpu, struct hlist_node *node)
{
	struct zcomp_ext *comp = hlist_entry(node, struct zcomp_ext, node);
	struct zcomp_ext_strm *zstrm = NULL;

	zstrm = *per_cpu_ptr(comp->stream, cpu);
	if (!IS_ERR_OR_NULL(zstrm))
		zcomp_ext_strm_free(comp, zstrm);
	*per_cpu_ptr(comp->stream, cpu) = NULL;
	return 0;
}

static int zcomp_ext_init(struct zcomp_ext *comp)
{
	int ret;

	comp->stream = alloc_percpu(struct zcomp_ext_strm *);
	if (!comp->stream)
		return -ENOMEM;

	ret = cpuhp_state_add_instance(CPUHP_ZCOMP_PREPARE, &comp->node);
	if (ret < 0)
		goto cleanup;
	return 0;

cleanup:
	free_percpu(comp->stream);
	return ret;
}

void zcomp_ext_destroy(struct zcomp_ext *comp)
{
	cpuhp_state_remove_instance(CPUHP_ZCOMP_PREPARE, &comp->node);
	free_percpu(comp->stream);
	kfree(comp);
}

/*
 * search available compressors for requested algorithm.
 * allocate new zcomp and initialize it. return compressing
 * backend pointer or ERR_PTR if things went bad. ERR_PTR(-EINVAL)
 * if requested algorithm is not supported, ERR_PTR(-ENOMEM) in
 * case of allocation error, or any other error potentially
 * returned by zcomp_ext_init().
 */
struct zcomp_ext *zcomp_ext_create(const char *compress, CtxtAlloc ctxt_alloc,
				   CtxtFree ctxt_free)
{
	struct zcomp_ext *comp = NULL;
	int error;

	if (!zcomp_ext_available_algorithm(compress))
		return ERR_PTR(-EINVAL);

	comp = kzalloc(sizeof(struct zcomp_ext), GFP_KERNEL);
	if (!comp)
		return ERR_PTR(-ENOMEM);

	comp->name = compress;
	comp->ctxt_alloc = ctxt_alloc;
	comp->ctxt_free = ctxt_free;

	if (crypto_has_delta_comp(compress, 0, 0) == 1)
		zcomp_ext_set_prop(comp, ZCOMP_EXT_DELTA_COMP);

	error = zcomp_ext_init(comp);
	if (error) {
		kfree(comp);
		return ERR_PTR(error);
	}
	return comp;
}
