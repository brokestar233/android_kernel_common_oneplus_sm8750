ccflags-y += -O3 \
    -DLZ4_FREESTANDING=1 \
    -DLZ4_FAST_DEC_LOOP=1

obj-y += lz4.o lz4hc.o dict.o

obj-$(CONFIG_ARM64) += $(addprefix lz4armv8/, lz4accel.o lz4armv8.o)

extra-y += dict.c

quiet_cmd_gencfg = GENCFG $@
      cmd_gencfg = xxd -i $(srctree)/lib/lz4/lz4.dict | sed 's/___lib_lz4_lz4_dict/lz4_dict/g' > $(srctree)/out/lib/lz4/dict.c

PHONY += FORCE
FORCE:

$(obj)/dict.c: FORCE
	$(Q)mkdir -p $(dir $@)
	$(call if_changed,gencfg)
