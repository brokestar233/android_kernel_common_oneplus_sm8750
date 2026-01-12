obj-y += overwrite_configs.o overwrite_config_loader.o

extra-y += overwrite_configs.c

SOURCE_PATH = "$(srctree)/$(src)"

quiet_cmd_gencfg = GEN $@
      cmd_gencfg = $(srctree)/drivers/of/overwriter/overwrite_configs/convert_configs.sh $@ $(SOURCE_PATH)

PHONY += FORCE
FORCE:

$(obj)/overwrite_configs.c: FORCE
	$(Q)mkdir -p $(dir $@)
	$(call if_changed,gencfg)