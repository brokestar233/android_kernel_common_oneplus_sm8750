// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/zstd.h>
#include <linux/get_prop_from_cmdline.h>
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

static const struct confirm_item *find_confirm_item(const char *name)
{
    int i;
    for (i = 0; i < confirm_list_count; i++) {
        if (strcmp(confirm_list[i].name, name) == 0)
            return &confirm_list[i];
    }
    return NULL;
}

bool should_intercept_module(const char *name)
{
    return find_overlay(name) != NULL;
}

enum intercept_status intercept_module_load(struct load_info *info, const char *name)
{
    const struct overlay_file *ov;
    const struct confirm_item *confirm_item;
    char *cmdline_value = NULL;
    void *decompressed_data = NULL;
    size_t decompressed_size;
    zstd_dctx *dctx = NULL;
    void *workspace = NULL;
    size_t workspace_size;

    ov = find_overlay(name);
    if (!ov)
        return INTERCEPT_STATUS_SKIP;

    /* 检查是否需要根据cmdline跳过拦截 */
    confirm_item = find_confirm_item(name);
    if (confirm_item && confirm_item->cmdline) {
        cmdline_value = get_property_from_cmdline(confirm_item->cmdline);
        if (cmdline_value) {
            /* 如果cmdline值为"0"，表示需要跳过拦截 */
            if (strcmp(cmdline_value, "0") == 0) {
                pr_info("module_overlay: Skipping interception of %s due to cmdline %s=0\n",
                        name, confirm_item->cmdline);
                kfree(cmdline_value);
                return INTERCEPT_STATUS_SKIP;
            }
            kfree(cmdline_value);
        }
    }

    /* 释放用户态传来的数据 */
    vfree(info->hdr);
    info->hdr = NULL;

    /* 初始化zstd解压缩上下文 */
    workspace_size = zstd_dctx_workspace_bound();
    workspace = vzalloc(workspace_size);
    if (!workspace) {
        pr_err("module_overlay: Failed to allocate workspace for %s\n", name);
        return INTERCEPT_STATUS_ERROR;
    }

    dctx = zstd_init_dctx(workspace, workspace_size);
    if (!dctx) {
        pr_err("module_overlay: Failed to initialize dctx for %s\n", name);
        vfree(workspace);
        return INTERCEPT_STATUS_ERROR;
    }

    /* 分配解压后缓冲区 */
    decompressed_data = vmalloc(ov->orig_size);
    if (!decompressed_data) {
        pr_err("module_overlay: vmalloc failed for decompressed data of %s\n", name);
        vfree(workspace);
        return INTERCEPT_STATUS_ERROR;
    }

    /* 解压缩数据 */
    decompressed_size = zstd_decompress_dctx(dctx, decompressed_data, ov->orig_size, ov->data, ov->len);
    if (zstd_is_error(decompressed_size)) {
        pr_err("module_overlay: zstd decompress failed for %s: %zu\n", name, decompressed_size);
        vfree(decompressed_data);
        vfree(workspace);
        return INTERCEPT_STATUS_ERROR;
    }

    vfree(workspace);

    /* 设置解压后的数据 */
    info->hdr = decompressed_data;
    info->len = decompressed_size;

    pr_info("module_overlay: %s replaced with embedded version (%zu -> %zu bytes)\n",
            name, ov->len, decompressed_size);

    return INTERCEPT_STATUS_SUCCESS;
}
