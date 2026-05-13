# SPDX-License-Identifier: GPL-2.0-only
zram-y	:=	zcomp.o zram_drv.o zram_memcg.o

obj-$(CONFIG_ZRAM)	+=	zram.o

zram-$(CONFIG_ZRAM_WRITEBACK)		+= zram_wb.o
