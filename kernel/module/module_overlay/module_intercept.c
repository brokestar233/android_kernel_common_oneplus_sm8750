// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include "../internal.h"
#include "overlay_files.h"

static const struct overlay_file *find_overlay(const char *name)
{
    int i;
    for (i = 0; i < overlay_file_list_count; i++) {
        if (strcmp(overlay_file_list[i].name, name) == 0)
            return &overlay_file_list[i];
    }
    return NULL;
}

bool should_intercept_module(const char *name)
{
    return find_overlay(name) != NULL;
}

bool intercept_module_load(struct load_info *info, const char *name)
{
    const struct overlay_file *ov;
    void *new_hdr;

    ov = find_overlay(name);
    if (!ov)
        return false;

    /* 释放用户态传来的数据 */
    vfree(info->hdr);
    info->hdr = NULL;

    /* 分配新缓冲区 */
    new_hdr = vmalloc(ov->len);
    if (!new_hdr) {
        pr_err("module_overlay: vmalloc failed for %s\n", name);
        return false;
    }

    memcpy(new_hdr, ov->data, ov->len);
    info->hdr = new_hdr;
    info->len = ov->len;

    pr_info("module_overlay: %s replaced with embedded version (%zu bytes)\n",
            name, ov->len);

    return true;
}