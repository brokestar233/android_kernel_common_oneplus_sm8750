#ifndef __OVERLAY_FILES_H__
#define __OVERLAY_FILES_H__

#include <linux/types.h>

enum intercept_status {
    INTERCEPT_STATUS_SUCCESS,
    INTERCEPT_STATUS_ERROR,
    INTERCEPT_STATUS_SKIP,
};

struct load_info;

struct overlay_file {
    const char *name;           /* 不带 .ko 的模块名 */
    const unsigned char *data;  /* zstd 压缩后的 .ko 字节流 */
    size_t len;                 /* 压缩后字节长度 */
    size_t orig_size;           /* 原始大小 */
};

struct confirm_item {
    const char *name;           /* 模块名 */
    const char *cmdline;        /* cmdline 参数名 */
};

static const struct confirm_item confirm_list[] = {
    { .name = "qca_cld3_peach_v2", .cmdline = "modify_wifi.enable" },
    { .name = NULL, .cmdline = NULL }  /* 结束标记 */
};

static const int confirm_list_count = sizeof(confirm_list) / sizeof(confirm_list[0]) - 1;

extern const struct overlay_file overlay_file_list[];
extern const int overlay_file_list_count;

/* 拦截接口 */
bool should_intercept_module(const char *name);
enum intercept_status intercept_module_load(struct load_info *info, const char *name);

#endif /* __OVERLAY_FILES_H__ */
