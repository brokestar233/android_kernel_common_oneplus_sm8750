obj-y += overwrite_configs.o overwrite_config_loader.o

extra-y += overwrite_configs.c

quiet_cmd_gencfg = GENCFG $@
      cmd_gencfg = $(srctree)/drivers/of/overwriter/overwrite_configs/convert_configs.sh

PHONY += FORCE
FORCE:

$(obj)/overwrite_configs.c: FORCE
	$(Q)mkdir -p $(dir $@)
	$(call if_changed,gencfg)