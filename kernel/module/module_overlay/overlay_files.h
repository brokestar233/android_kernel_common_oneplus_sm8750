#ifndef __OVERLAY_FILES_H__
#define __OVERLAY_FILES_H__

#include <linux/types.h>

struct load_info;

struct overlay_file {
    const char *name;           /* 不带 .ko 的模块名 */
    const unsigned char *data;  /* 原始 .ko 字节流 */
    size_t len;                 /* 字节长度 */
};

extern const struct overlay_file overlay_file_list[];
extern const int overlay_file_list_count;

/* 拦截接口 */
bool should_intercept_module(const char *name);
bool intercept_module_load(struct load_info *info, const char *name);

#endif /* __OVERLAY_FILES_H__ */
