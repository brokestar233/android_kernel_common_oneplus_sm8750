# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_ZRAM)	+=	zram.o

zram-y	:=	zcomp.o zram_drv.o

#zram-$(CONFIG_OPLUS_FEATURE_ZRAM_OPT)	+=	zram_opt.o
zram-$(CONFIG_HYBRIDSWAP) += hybridswap/hybridmain.o
zram-$(CONFIG_HYBRIDSWAP_SWAPD) += hybridswap/hybridswapd.o
zram-$(CONFIG_HYBRIDSWAP_CORE) += hybridswap/hybridswap.o