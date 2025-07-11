#ifndef _OVERWRITE_CONFIGS_H
#define _OVERWRITE_CONFIGS_H

struct overwrite_config_group {
    const char *prefix;
    const char *const *values;
    int count;
};

extern const struct overwrite_config_group overwrite_config_groups[];
extern const int overwrite_config_group_count;

#endif /* _OVERWRITE_CONFIGS_H */