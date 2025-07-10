#!/bin/sh
srctree=$(pwd)

# 创建目标文件并写入头部
> "$srctree/drivers/of/overwriter/overwrite_configs.c"

cat << EOF > "$srctree/drivers/of/overwriter/overwrite_configs.c"
#include "overwrite_configs.h"
#include <linux/stddef.h>

const char *overwrite_config_lines[] = {
EOF

line_count=0

# 处理 common 文件夹中的配置文件
common_dir="$srctree/../drivers/of/overwriter/overwrite_configs/common"
if [ -d "$common_dir" ]; then
    for file in "$common_dir"/*.conf; do
        if [ -f "$file" ]; then
            while IFS= read -r line; do
                if [ -n "$line" ]; then
                    echo "    \"common:$line\"," >> "$srctree/drivers/of/overwriter/overwrite_configs.c"
                    line_count=$((line_count + 1))
                fi
            done < "$file"
        fi
    done
fi

# 处理数字命名的机型代号文件夹
config_base_dir="$srctree/../drivers/of/overwriter/overwrite_configs"
for model_dir in "$config_base_dir"/[0-9]*; do
    if [ -d "$model_dir" ]; then
        # 获取文件夹名称作为前缀
        model_prefix=$(basename "$model_dir")
        for file in "$model_dir"/*.conf; do
            if [ -f "$file" ]; then
                while IFS= read -r line; do
                    if [ -n "$line" ]; then
                        echo "    \"$model_prefix:$line\"," >> "$srctree/drivers/of/overwriter/overwrite_configs.c"
                        line_count=$((line_count + 1))
                    fi
                done < "$file"
            fi
        done
    fi
done

# 写入尾部
cat << EOF >> "$srctree/drivers/of/overwriter/overwrite_configs.c"
    NULL
};

const int overwrite_config_line_count = $line_count;
EOF