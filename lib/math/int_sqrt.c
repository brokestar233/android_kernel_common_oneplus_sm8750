// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2013 Davidlohr Bueso <davidlohr.bueso@hp.com>
 *
 * Oryon-optimized hybrid integer square root.
 *
 * This implementation draws on ideas from firelzrd/isqrt:
 * https://github.com/firelzrd/isqrt
 *
 * Thanks to Masahito Suzuki for publishing the original CLZ/LUT/Newton-based
 * integer square root implementation that inspired this Oryon-specific kernel
 * adaptation.
 *
 * Oryon-specific adaptation and Linux kernel integration by:
 * brokestar233 <3765589194@qq.com>
 *
 * Small inputs are routed to table-driven helpers, while larger inputs use
 * a CLZ/LUT-seeded Newton-Raphson path that takes advantage of Oryon's fast
 * integer divide pipeline.
 */

#include <linux/export.h>
#include <linux/bitops.h>
#include <linux/limits.h>
#include <linux/math.h>

static const s8 int_sqrt16_delta_lut[256] = {
	  0,  0,  0, -1,  0, -1, -1, -1,  0, -1, -1, -1, -1, -1, -1, -1,
	  0, -1, -1, -1, -1, -1, -1, -2, -2, -1, -1, -1, -1, -1, -1, -1,
	  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -2, -2, -2, -2, -3, -3,
	 -3, -3, -2, -2, -2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -2, -2, -2,
	 -2, -2, -2, -3, -3, -3, -3, -4, -4, -4, -4, -5, -5, -5, -5, -6,
	 -6, -5, -5, -5, -4, -4, -4, -3, -3, -3, -3, -2, -2, -2, -2, -2,
	 -2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	 -1, -1, -2, -2, -2, -2, -2, -2, -2, -3, -3, -3, -3, -3, -3, -4,
	 -4, -4, -4, -4, -4, -5, -5, -5, -5, -6, -6, -6, -6, -6, -7, -7,
	 -7, -7, -8, -8, -8, -8, -9, -9, -9, -9,-10,-10,-10,-11,-11,-11,
	-11,-11,-10,-10,-10, -9, -9, -9, -8, -8, -8, -7, -7, -7, -6, -6,
	 -6, -6, -5, -5, -5, -5, -4, -4, -4, -4, -4, -3, -3, -3, -3, -3,
	 -3, -3, -2, -2, -2, -2, -2, -2, -2, -2, -1, -1, -1, -1, -1, -1,
	 -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

static const u16 int_sqrt_est_lut[128] = {
	256,257,259,261,263,265,267,269,271,273,275,277,278,280,282,284,
	286,288,289,291,293,295,296,298,300,301,303,305,306,308,310,311,
	313,315,316,318,320,321,323,324,326,327,329,331,332,334,335,337,
	338,340,341,343,344,346,347,349,350,352,353,354,356,357,359,360,
	362,364,367,370,373,375,378,381,384,386,389,391,394,397,399,402,
	404,407,409,412,414,417,419,422,424,426,429,431,434,436,438,441,
	443,445,448,450,452,454,457,459,461,463,465,468,470,472,474,476,
	478,481,483,485,487,489,491,493,495,497,499,501,503,505,507,509,
};

static __always_inline u32 int_sqrt_oryon16(u16 v)
{
	u32 e, zone, msb, sub_bits, sub_m, h, y;

	if (v <= 1)
		return v;

	e = fls(v) - 1;
	zone = e >> 1;
	if (!zone)
		return 1;

	msb = e & 1;
	sub_bits = zone - 1;
	sub_m = (v >> (e - sub_bits)) & ((1U << sub_bits) - 1);
	h = (1U << zone) | (msb << sub_bits) | sub_m;
	y = h + (s32)int_sqrt16_delta_lut[h];

	if ((y + 1) * (y + 1) <= v)
		y++;
	if (y * y > v)
		y--;

	return y;
}

static __always_inline u32 int_sqrt_oryon32(u32 v)
{
	u32 e, half_e, frac6, y;

	if (v <= 1)
		return v;

	e = fls(v) - 1;
	half_e = e >> 1;
	frac6 = e >= 6 ? (v >> (e - 6)) & 0x3f : (v << (6 - e)) & 0x3f;
	y = ((u32)int_sqrt_est_lut[((e & 1) << 6) | frac6] << half_e) >> 8;
	y |= !y;

	y = (y + v / y) >> 1;

	while ((u64)y * y > v)
		y--;
	while ((u64)(y + 1) * (y + 1) <= v)
		y++;

	return y;
}

static __always_inline u32 int_sqrt_oryon64(u64 v)
{
	u32 e, half_e, frac6;
	u64 y, yy;

	if (v <= 1)
		return v;

	e = fls64(v) - 1;
	half_e = e >> 1;
	frac6 = e >= 6 ? (u32)((v >> (e - 6)) & 0x3f) : (u32)((v << (6 - e)) & 0x3f);
	y = ((u64)int_sqrt_est_lut[((e & 1) << 6) | frac6] << half_e) >> 8;
	y |= !y;

	y = (y + v / y) >> 1;
	y = (y + v / y) >> 1;
	y = (y + v / y) >> 1;

	if (y > U32_MAX)
		y = U32_MAX;

	yy = y * y;
	if (yy > v) {
		y--;
	} else if (y < U32_MAX) {
		u64 yp1 = y + 1;

		if (yp1 * yp1 <= v)
			y = yp1;
	}

	return y;
}

/**
 * int_sqrt - computes the integer square root
 * @x: integer of which to calculate the sqrt
 *
 * Computes: floor(sqrt(x))
 */
inline unsigned long int_sqrt(unsigned long x)
{
	if (x <= U16_MAX)
		return int_sqrt_oryon16(x);

	if (x <= U32_MAX)
		return int_sqrt_oryon32(x);

	return int_sqrt_oryon64(x);
}
EXPORT_SYMBOL(int_sqrt);

#if BITS_PER_LONG < 64
/**
 * int_sqrt64 - strongly typed int_sqrt function when minimum 64 bit input
 * is expected.
 * @x: 64bit integer of which to calculate the sqrt
 */
u32 int_sqrt64(u64 x)
{
	if (x <= U16_MAX)
		return int_sqrt_oryon16(x);

	if (x <= U32_MAX)
		return int_sqrt_oryon32(x);

	return int_sqrt_oryon64(x);
}
EXPORT_SYMBOL(int_sqrt64);
#endif
