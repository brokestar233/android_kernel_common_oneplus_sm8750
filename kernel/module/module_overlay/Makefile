obj-y += overlay_files.o module_intercept.o
extra-y += overlay_files.c

quiet_cmd_genoverlay = GEN     $@
      cmd_genoverlay = $(srctree)/kernel/module/module_overlay/convert_overlay.sh $@

PHONY += FORCE
FORCE:

$(obj)/overlay_files.c: FORCE
	$(Q)mkdir -p $(dir $@)
	$(call if_changed,genoverlay)
