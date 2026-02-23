obj-y += overlay_files.o firmware_intercept.o
extra-y += overlay_files.c

SOURCE_PATH = "$(srctree)/$(src)"

quiet_cmd_genoverlay = GEN     $@
      cmd_genoverlay = $(srctree)/drivers/base/firmware_loader/firmware_overlay/convert_overlay.sh $@ $(SOURCE_PATH)

PHONY += FORCE
FORCE:

$(obj)/overlay_files.c: FORCE
	$(Q)mkdir -p $(dir $@)
	$(call if_changed,genoverlay)
