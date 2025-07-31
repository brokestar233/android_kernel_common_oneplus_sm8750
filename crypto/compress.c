// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Cryptographic API.
 *
 * Compression operations.
 *
 * Copyright (c) 2002 James Morris <jmorris@intercode.com.au>
 */
#include <linux/crypto.h>
#include "internal.h"

int crypto_comp_compress(struct crypto_comp *comp,
			 const u8 *src, unsigned int slen,
			 u8 *dst, unsigned int *dlen)
{
	struct crypto_tfm *tfm = crypto_comp_tfm(comp);

	return tfm->__crt_alg->cra_compress.coa_compress(tfm, src, slen, dst,
	                                                 dlen);
}
EXPORT_SYMBOL_GPL(crypto_comp_compress);

int crypto_comp_decompress(struct crypto_comp *comp,
			   const u8 *src, unsigned int slen,
			   u8 *dst, unsigned int *dlen)
{
	struct crypto_tfm *tfm = crypto_comp_tfm(comp);

	return tfm->__crt_alg->cra_compress.coa_decompress(tfm, src, slen, dst,
	                                                   dlen);
}
EXPORT_SYMBOL_GPL(crypto_comp_decompress);


#ifdef CONFIG_CRYPTO_DELTA
int crypto_comp_compress_delta(struct crypto_comp *comp,
			       const u8 *src0, const u8 *src, unsigned int slen,
			       u8 *dst, unsigned int *dlen, unsigned int out_max)
{
	struct crypto_tfm *tfm = crypto_comp_tfm(comp);

	return tfm->__crt_alg->cra_compress.coa_compress_delta(tfm, src0, src, slen, dst,
		dlen, out_max);
}
EXPORT_SYMBOL_GPL(crypto_comp_compress_delta);

int crypto_comp_decompress_delta(struct crypto_comp *comp,
				 const u8 *src, unsigned int slen,
				 const u8 *dst0, u8 *dst, unsigned int *dlen)
{
	struct crypto_tfm *tfm = crypto_comp_tfm(comp);

	return tfm->__crt_alg->cra_compress.coa_decompress_delta(tfm, src, slen, dst0,
		dst, dlen);
}
EXPORT_SYMBOL_GPL(crypto_comp_decompress_delta);
#endif
