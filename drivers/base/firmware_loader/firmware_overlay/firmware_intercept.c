// SPDX-License-Identifier: GPL-2.0
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/zstd.h>
#include <linux/firmware.h>
#include <linux/get_prop_from_cmdline.h>
#include "../firmware.h" 
#include "overlay_files.h"

/* 辅助函数：忽略路径前缀，只获取文件名 */
static inline const char *get_fw_basename(const char *name)
{
    const char *basename = strrchr(name, '/');
    if (basename)
        return basename + 1; /* 跳过 '/' */
    return name;
}

static const struct overlay_file *find_overlay(const char *name)
{
    int i;
    const char *fw_name = get_fw_basename(name);

    for (i = 0; i < firmware_file_list_count; i++) {
        /* 比较文件名 */
        if (strcmp(firmware_file_list[i].name, fw_name) == 0)
            return &firmware_file_list[i];
    }
    return NULL;
}

static const struct confirm_item *find_confirm_item(const char *name)
{
    int i;
    const char *fw_name = get_fw_basename(name);

    for (i = 0; i < firmware_confirm_list_count; i++) {
        /* 比较文件名 */
        if (strcmp(firmware_confirm_list[i].name, fw_name) == 0)
            return &firmware_confirm_list[i];
    }
    return NULL;
}

bool should_intercept_firmware(const char *name)
{
    return find_overlay(name) != NULL;
}

enum intercept_status intercept_firmware_load(struct firmware *fw, const char *name)
{
    const struct overlay_file *ov;
    const struct confirm_item *confirm_item;
    char *cmdline_value = NULL;
    void *decompressed_data = NULL;
    size_t decompressed_size;
    zstd_dctx *dctx = NULL;
    void *workspace = NULL;
    size_t workspace_size;
    struct fw_priv *fw_priv;

    /* 获取 fw_priv 指针 */
    if (!fw || !fw->priv)
        return INTERCEPT_STATUS_ERROR;
    
    fw_priv = fw->priv;

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
                pr_info("firmware_overlay: Skipping interception of %s due to cmdline %s=0\n",
                        name, confirm_item->cmdline);
                kfree(cmdline_value);
                return INTERCEPT_STATUS_SKIP;
            }
            kfree(cmdline_value);
        }
    }

    /* 初始化zstd解压缩上下文 */
    workspace_size = zstd_dctx_workspace_bound();
    workspace = vzalloc(workspace_size);
    if (!workspace) {
        pr_err("firmware_overlay: Failed to allocate workspace for %s\n", name);
        return INTERCEPT_STATUS_ERROR;
    }

    dctx = zstd_init_dctx(workspace, workspace_size);
    if (!dctx) {
        pr_err("firmware_overlay: Failed to initialize dctx for %s\n", name);
        vfree(workspace);
        return INTERCEPT_STATUS_ERROR;
    }

    /* 分配解压后缓冲区 */
    decompressed_data = vmalloc(ov->orig_size);
    if (!decompressed_data) {
        pr_err("firmware_overlay: vmalloc failed for decompressed data of %s\n", name);
        vfree(workspace);
        return INTERCEPT_STATUS_ERROR;
    }

    /* 解压缩数据 */
    decompressed_size = zstd_decompress_dctx(dctx, decompressed_data, ov->orig_size, ov->data, ov->len);
    if (zstd_is_error(decompressed_size)) {
        pr_err("firmware_overlay: zstd decompress failed for %s: %zu\n", name, decompressed_size);
        vfree(decompressed_data);
        vfree(workspace);
        return INTERCEPT_STATUS_ERROR;
    }

    vfree(workspace);

    /* 
     * 将数据挂载到 firmware 结构体上 
     * 固件加载器会在 release_firmware 时释放 fw_priv->data
     */
    fw_priv->data = decompressed_data;
    fw_priv->size = decompressed_size;
#ifdef CONFIG_FW_LOADER_PAGED_BUF
    fw_priv->is_paged_buf = false;
#endif
    fw_priv->allocated_size = decompressed_size; 

    pr_info("firmware_overlay: %s intercepted and replaced with embedded version (%zu bytes)\n",
            name, decompressed_size);

    return INTERCEPT_STATUS_SUCCESS;
}
