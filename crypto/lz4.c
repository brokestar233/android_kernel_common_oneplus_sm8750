// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cryptographic API.
 *
 * Copyright (c) 2013 Chanho Min <chanho.min@lge.com>
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/crypto.h>
#include <linux/vmalloc.h>
#include <linux/lz4.h>
#include <crypto/internal/scompress.h>
#include "../lib/lz4/dict.h"

struct lz4_ctx {
	void *lz4_comp_mem;
	void *comp_stream;  /* LZ4_stream_t for dict compression */
	void *decomp_stream; /* LZ4_streamDecode_t for dict decompression */
};

static bool lz4_use_dict = true;
module_param(lz4_use_dict, bool, 0644);
MODULE_PARM_DESC(lz4_use_dict, "Enable LZ4 dictionary compression (default: true)");

static void *__lz4_alloc_ctx(struct lz4_ctx *ctx)
{
	ctx->lz4_comp_mem = vmalloc(LZ4_MEM_COMPRESS);
	if (!ctx->lz4_comp_mem) {
		kfree(ctx);
		return ERR_PTR(-ENOMEM);
	}

	ctx->comp_stream = kzalloc(LZ4_STREAM_MINSIZE, GFP_KERNEL);
	if (!ctx->comp_stream) {
		vfree(ctx->lz4_comp_mem);
		kfree(ctx);
		return ERR_PTR(-ENOMEM);
	}

	ctx->decomp_stream = kzalloc(sizeof(LZ4_streamDecode_t), GFP_KERNEL);
	if (!ctx->decomp_stream) {
		kfree(ctx->comp_stream);
		vfree(ctx->lz4_comp_mem);
		kfree(ctx);
		return ERR_PTR(-ENOMEM);
	}

	return ctx;
}

static void *lz4_alloc_ctx(struct crypto_scomp *tfm)
{
	struct lz4_ctx *ctx;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	ctx = __lz4_alloc_ctx(ctx);
	if (IS_ERR(ctx))
		return ERR_PTR(-ENOMEM);

	return ctx;
}

static int lz4_init(struct crypto_tfm *tfm)
{
	struct lz4_ctx *ctx = crypto_tfm_ctx(tfm);

	ctx = __lz4_alloc_ctx(ctx);
	if (IS_ERR(ctx))
		return -ENOMEM;

	return 0;
}

static void __lz4_free_ctx(void *ctx)
{
	struct lz4_ctx *zctx = ctx;

	kfree(zctx->decomp_stream);
	kfree(zctx->comp_stream);
	vfree(zctx->lz4_comp_mem);
}

static void lz4_free_ctx(struct crypto_scomp *tfm, void *ctx)
{
	struct lz4_ctx *zctx = ctx;

	if (!zctx)
		return;

	__lz4_free_ctx(zctx);
	kfree(zctx);
}

static void lz4_exit(struct crypto_tfm *tfm)
{
	struct lz4_ctx *ctx = crypto_tfm_ctx(tfm);

	__lz4_free_ctx(ctx);
}

static int __lz4_compress_crypto(const u8 *src, unsigned int slen,
				 u8 *dst, unsigned int *dlen, void *ctx)
{
	struct lz4_ctx *zctx = ctx;
	void *workspace = zctx ? zctx->lz4_comp_mem : NULL;
	int out_len;

	if (!workspace)
		return -ENOMEM;

	/* If dictionary is enabled and available, use it; otherwise, fallback to default */
	if (lz4_use_dict && lz4_dict_len > 0) {
		LZ4_stream_t *stream = zctx->comp_stream;
		if (!stream)
			return -ENOMEM;

		/* Reset stream for each call to avoid state carry-over */
		LZ4_resetStream(stream);

		out_len = LZ4_loadDict(stream, lz4_dict, lz4_dict_len);
		if (out_len != (int)lz4_dict_len)
			return -EINVAL;

		out_len = LZ4_compress_fast_continue(stream, src, dst, slen, *dlen, 1);
	} else {
		/* Fallback to no-dictionary compression, always use pre-allocated workspace */
		out_len = LZ4_compress_default(src, dst, slen, *dlen, workspace);
	}

	if (!out_len)
		return -EINVAL;

	*dlen = out_len;
	return 0;
}

static int lz4_scompress(struct crypto_scomp *tfm, const u8 *src,
			 unsigned int slen, u8 *dst, unsigned int *dlen,
			 void *ctx)
{
	return __lz4_compress_crypto(src, slen, dst, dlen, ctx);
}

static int lz4_compress_crypto(struct crypto_tfm *tfm, const u8 *src,
			       unsigned int slen, u8 *dst, unsigned int *dlen)
{
	struct lz4_ctx *ctx = crypto_tfm_ctx(tfm);

	return __lz4_compress_crypto(src, slen, dst, dlen, ctx);
}

static int __lz4_decompress_crypto(const u8 *src, unsigned int slen,
				   u8 *dst, unsigned int *dlen, void *ctx)
{
	int out_len;

	if (!ctx)
		return -ENOMEM;

	/* If dictionary is enabled and available, use it; otherwise, fallback to default */
	if (lz4_use_dict && lz4_dict_len > 0) {
		LZ4_streamDecode_t *stream = ((struct lz4_ctx *)ctx)->decomp_stream;
		if (!stream)
			return -ENOMEM;

		out_len = LZ4_setStreamDecode(stream, lz4_dict, lz4_dict_len);
		if (out_len != 1)  /* LZ4_setStreamDecode returns 1 on success */
			return -EINVAL;

		out_len = LZ4_decompress_safe_continue(stream, src, dst, slen, *dlen);
	} else {
#if defined(CONFIG_ARM64) && defined(CONFIG_KERNEL_MODE_NEON)
		out_len = LZ4_arm64_decompress_safe(src, dst, slen, *dlen, false);
#else
		out_len = LZ4_decompress_safe(src, dst, slen, *dlen);
#endif
	}

	if (out_len < 0)
		return -EINVAL;

	*dlen = out_len;
	return 0;
}

static int lz4_sdecompress(struct crypto_scomp *tfm, const u8 *src,
			   unsigned int slen, u8 *dst, unsigned int *dlen,
			   void *ctx)
{
	return __lz4_decompress_crypto(src, slen, dst, dlen, ctx);
}

static int lz4_decompress_crypto(struct crypto_tfm *tfm, const u8 *src,
				 unsigned int slen, u8 *dst,
				 unsigned int *dlen)
{
	struct lz4_ctx *ctx = crypto_tfm_ctx(tfm);

	return __lz4_decompress_crypto(src, slen, dst, dlen, ctx);
}

static struct crypto_alg alg_lz4 = {
	.cra_name		= "lz4",
	.cra_driver_name	= "lz4-generic",
	.cra_flags		= CRYPTO_ALG_TYPE_COMPRESS,
	.cra_ctxsize		= sizeof(struct lz4_ctx),
	.cra_module		= THIS_MODULE,
	.cra_init		= lz4_init,
	.cra_exit		= lz4_exit,
	.cra_u			= { .compress = {
	.coa_compress		= lz4_compress_crypto,
	.coa_decompress		= lz4_decompress_crypto } }
};

static struct scomp_alg scomp = {
	.alloc_ctx		= lz4_alloc_ctx,
	.free_ctx		= lz4_free_ctx,
	.compress		= lz4_scompress,
	.decompress		= lz4_sdecompress,
	.base			= {
		.cra_name	= "lz4",
		.cra_driver_name = "lz4-scomp",
		.cra_module	 = THIS_MODULE,
	}
};

static int __init lz4_mod_init(void)
{
	int ret;

	ret = crypto_register_alg(&alg_lz4);
	if (ret)
		return ret;

	ret = crypto_register_scomp(&scomp);
	if (ret) {
		crypto_unregister_alg(&alg_lz4);
		return ret;
	}

	return ret;
}

static void __exit lz4_mod_fini(void)
{
	crypto_unregister_alg(&alg_lz4);
	crypto_unregister_scomp(&scomp);
}

subsys_initcall(lz4_mod_init);
module_exit(lz4_mod_fini);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("LZ4 Compression Algorithm");
MODULE_ALIAS_CRYPTO("lz4");
