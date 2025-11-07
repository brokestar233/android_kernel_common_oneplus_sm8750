#!/bin/bash

# === 路径配置 ===
out_file="$1"
srctree=$(pwd)
overlay_dir="$srctree/../kernel/module/module_overlay/modules"

> "$out_file"

# === 写入文件头 ===
cat <<'EOF' > "$out_file"
#include "overlay_files.h"
#include <linux/stddef.h>

EOF

# === 关联数组存储信息 ===
declare -A name_map    # name -> array_name
declare -A count_map   # name -> element_count

# === 临时文件 ===
tmp_xxd="/tmp/overlay_xxd_$$.c"

# === 处理所有 .ko 文件 ===
shopt -s nullglob
ko_files=("$overlay_dir"/*.ko)
file_idx=0

for ko in "${ko_files[@]}"; do
    [ -f "$ko" ] || continue

    base=$(basename "$ko")
    name="${base%.ko}"                    # 去掉 .ko
    array_name="${name//[^a-zA-Z0-9]/_}_data"  # 合法 C 标识符

    # 生成字节数组
    if ! xxd -i "$ko" > "$tmp_xxd.raw" 2>/dev/null; then
        echo "xxd failed: $ko" >&2
        continue
    fi

    ### CHANGED START ###
    # 修复：从 xxd 输出中提取 _len 变量的值作为 count
    # xxd 的输出包含 "unsigned int ..._len = XXX;"
    len_line=$(grep -E 'unsigned int[[:space:]]+.*_len[[:space:]]*=' "$tmp_xxd.raw")
    count=$(echo "$len_line" | awk -F'=' '{print $2}' | awk -F';' '{print $1}' | tr -d ' ')

    if [ -z "$count" ]; then
        echo "Failed to extract _len from xxd output: $ko" >&2
        rm -f "$tmp_xxd.raw"
        continue
    fi

    # 安全替换：
    # 1. 替换数组定义行
    # 2. 转换 0x... 为 0x...U
    # 3. (修复) 删除 _len 定义行
    sed -E \
        -e "s/unsigned char[[:space:]]+([^[]+)[[:space:]]*\[([^]]*)\]/const unsigned char ${array_name}[\2]/" \
        -e 's/0x([0-9a-fA-F]{2})/0x\1U/g' \
        -e 's/-/_/g' \
        -e '/unsigned int[[:space:]]+.*_len[[:space:]]*=/d' \
        "$tmp_xxd.raw" > "$tmp_xxd" || { echo "sed failed: $ko" >&2; rm -f "$tmp_xxd.raw"; continue; }
    ### CHANGED END ###

    rm -f "$tmp_xxd.raw"

    ### CHANGED START ###
    # 修复：删除旧的、不正确的 count 计算方式
    # count=$(grep -E '^[[:space:]]+0x[0-9a-fA-F]' "$tmp_xxd" | wc -l)
    ((file_idx++))
    ### CHANGED END ###

    # 记录
    name_map["$name"]="$array_name"
    count_map["$name"]=$count

    # 追加数组定义到输出文件
    cat "$tmp_xxd" >> "$out_file"
    echo >> "$out_file"
done

# === 生成 overlay_file_list 数组 ===
cat <<EOF >> "$out_file"
// 所有 overlay 模块的描述表
const struct overlay_file overlay_file_list[] = {
EOF

if (( file_idx == 0 )); then
    echo "No .ko files found in $overlay_dir"
else
    for name in "${!name_map[@]}"; do
        array_name="${name_map[$name]}"
        count="${count_map[$name]}"
        printf '    { .name = "%s", .data = %s, .len = %d },\n' \
               "$name" "$array_name" "$count" >> "$out_file"
    done
fi

cat <<EOF >> "$out_file"
};

const int overlay_file_list_count = $file_idx;

EOF

# === 清理临时文件 ===
rm -f "$tmp_xxd"

# === 统一换行符为 LF ===
if command -v dos2unix >/dev/null 2>&1; then
    dos2unix "$out_file" 2>/dev/null || true
else
    tr -d '\r' < "$out_file" > "${out_file}.tmp" 2>/dev/null && \
        mv "${out_file}.tmp" "$out_file"
fi

echo "Generated $out_file: $file_idx overlay file(s) processed."
