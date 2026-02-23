#ifndef __OVERLAY_FILES_H__
#define __OVERLAY_FILES_H__

#include <linux/types.h>

enum intercept_status {
    INTERCEPT_STATUS_SUCCESS,
    INTERCEPT_STATUS_ERROR,
    INTERCEPT_STATUS_SKIP,
};

struct firmware;

struct overlay_file {
    const char *name;
    const unsigned char *data;
    size_t len;
    size_t orig_size;
};

struct confirm_item {
    const char *name;           /* 模块名 */
    const char *cmdline;        /* cmdline 参数名 */
};

static const struct confirm_item firmware_confirm_list[] = {
    { .name = "regdb.bin", .cmdline = "modify_wifi.enable" },
    { .name = NULL, .cmdline = NULL }  /* 结束标记 */
};

static const int firmware_confirm_list_count = sizeof(firmware_confirm_list) / sizeof(firmware_confirm_list[0]) - 1;

extern const struct overlay_file firmware_file_list[];
extern const int firmware_file_list_count;

/* 拦截接口 */
bool should_intercept_firmware(const char *name);
enum intercept_status intercept_firmware_load(struct firmware *fw, const char *name);

#endif /* __OVERLAY_FILES_H__ */
