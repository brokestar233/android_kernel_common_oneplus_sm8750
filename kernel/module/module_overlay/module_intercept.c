// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/zstd.h>
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
    void *decompressed_data = NULL;
    size_t decompressed_size;
    zstd_dctx *dctx = NULL;
    void *workspace = NULL;
    size_t workspace_size;

    ov = find_overlay(name);
    if (!ov)
        return false;

    /* 释放用户态传来的数据 */
    vfree(info->hdr);
    info->hdr = NULL;

    /* 初始化zstd解压缩上下文 */
    workspace_size = zstd_dctx_workspace_bound();
    workspace = vzalloc(workspace_size);
    if (!workspace) {
        pr_err("module_overlay: Failed to allocate workspace for %s\n", name);
        return false;
    }

    dctx = zstd_init_dctx(workspace, workspace_size);
    if (!dctx) {
        pr_err("module_overlay: Failed to initialize dctx for %s\n", name);
        vfree(workspace);
        return false;
    }

    /* 分配解压后缓冲区 */
    decompressed_data = vmalloc(ov->orig_size);
    if (!decompressed_data) {
        pr_err("module_overlay: vmalloc failed for decompressed data of %s\n", name);
        vfree(workspace);
        return false;
    }

    /* 解压缩数据 */
    decompressed_size = zstd_decompress_dctx(dctx, decompressed_data, ov->orig_size, ov->data, ov->len);
    if (zstd_is_error(decompressed_size)) {
        pr_err("module_overlay: zstd decompress failed for %s: %zu\n", name, decompressed_size);
        vfree(decompressed_data);
        vfree(workspace);
        return false;
    }

    vfree(workspace);

    /* 设置解压后的数据 */
    info->hdr = decompressed_data;
    info->len = decompressed_size;

    pr_info("module_overlay: %s replaced with embedded version (%zu -> %zu bytes)\n",
            name, ov->len, decompressed_size);

    return true;
}
