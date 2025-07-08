/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright (C) 2022, Stephan Mueller <smueller@chronox.de>
 */

#ifndef _LRNG_INTERFACE_RANDOM_H
#define _LRNG_INTERFACE_RANDOM_H

#ifdef CONFIG_LRNG_RANDOM_IF
void invalidate_batched_entropy(void);
void lrng_kick_random_ready(void);
#else /* CONFIG_LRNG_RANDOM_IF */
static inline void invalidate_batched_entropy(void) { }
static inline void lrng_kick_random_ready(void) { }
#endif /* CONFIG_LRNG_RANDOM_IF */

#endif /* _LRNG_INTERFACE_RANDOM_H */
