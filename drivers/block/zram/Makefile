# SPDX-License-Identifier: GPL-2.0-only
ccflags-y += -O3
ifdef CONFIG_CRYPTO_DELTA
zram-y	:=	sddc/zcomp_ext.o zram_drv.o
else
zram-y	:=	zcomp.o zram_drv.o
endif

obj-$(CONFIG_ZRAM)	+=	zram.o

zram-$(CONFIG_OPLUS_FEATURE_ZRAM_OPT)	+=	zram_opt.o
obj-$(CONFIG_CRYPTO_DELTA) += sddc/