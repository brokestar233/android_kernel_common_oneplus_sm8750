#!/bin/bash

# === 路径配置 ===
out_file="$1"
srctree="$2"

overlay_dir=$(echo "$srctree" | sed 's|//|/|g')

if [[ "$overlay_dir" == ..* ]]; then
    overlay_dir="$overlay_dir"
else
    overlay_dir=$(echo "$overlay_dir" | sed 's|\(.*\)\1|\1|')
fi

overlay_dir=$(echo "$overlay_dir" | sed 's|/firmware$||; s|$|/firmware|')

> "$out_file"

# === 写入文件头 ===
cat <<'EOF' > "$out_file"
#include "overlay_files.h"
#include <linux/stddef.h>
#include <linux/zstd.h>

EOF

# === 关联数组存储信息 ===
declare -A name_map        # struct_name -> array_name
declare -A count_map       # struct_name -> element_count
declare -A orig_size_map   # struct_name -> original_size

# === 临时文件 ===
tmp_xxd="/tmp/overlay_xxd_$$.c"
tmp_comp="/tmp/overlay_comp_$$.bin"

# === 处理所有 .bin 文件 ===
shopt -s nullglob
bin_files=("$overlay_dir"/*.bin)
file_idx=0

for bin in "${bin_files[@]}"; do
    [ -f "$bin" ] || continue

    base=$(basename "$bin")
    
    # [修改点1] struct_name: 保留完整文件名 (例如: regdb.bin)
    # 也就是内核请求时最后的那一部分，配合 C 代码中的 basename 匹配逻辑
    struct_name="$base"

    # [修改点2] array_name: 变量名需要符合C语言规范，去掉点号和后缀
    # regdb.bin -> regdb -> regdb_data
    safe_name="${base%.bin}" 
    array_name="${safe_name//[^a-zA-Z0-9]/_}_data"  

    # 先获取原始大小
    orig_size=$(stat -c%s "$bin")
    
    # 使用 zstd 进行最大压缩
    if ! /usr/bin/zstd -22 -f "$bin" -o "$tmp_comp"; then
        echo "zstd compression failed: $bin" >&2
        continue
    fi

    # 生成字节数组
    if ! /usr/bin/xxd -i "$tmp_comp" > "$tmp_xxd.raw" 2>/dev/null; then
        echo "xxd failed: $bin" >&2
        continue
    fi

    len_line=$(grep -E 'unsigned int[[:space:]]+.*_len[[:space:]]*=' "$tmp_xxd.raw")
    count=$(echo "$len_line" | awk -F'=' '{print $2}' | awk -F';' '{print $1}' | tr -d ' ')

    if [ -z "$count" ]; then
        echo "Failed to extract _len from xxd output: $bin" >&2
        rm -f "$tmp_xxd.raw"
        continue
    fi

    # 替换：
    # 1. 替换数组定义行，使用清洗过的 array_name
    # 2. 转换 0x... 为 0x...U
    # 3. 删除 _len 定义行
    sed -E \
        -e "s/unsigned char[[:space:]]+([^[]+)[[:space:]]*\[([^]]*)\]/const unsigned char ${array_name}[\2]/" \
        -e 's/0x([0-9a-fA-F]{2})/0x\1U/g' \
        -e 's/-/_/g' \
        -e '/unsigned int[[:space:]]+.*_len[[:space:]]*=/d' \
        "$tmp_xxd.raw" > "$tmp_xxd" || { echo "sed failed: $bin" >&2; rm -f "$tmp_xxd.raw"; continue; }

    rm -f "$tmp_xxd.raw" "$tmp_comp"

    ((file_idx++))

    # [修改点3] 使用 struct_name (文件名) 作为 key
    name_map["$struct_name"]="$array_name"
    count_map["$struct_name"]=$count
    orig_size_map["$struct_name"]=$orig_size

    # 追加数组定义到输出文件
    cat "$tmp_xxd" >> "$out_file"
    echo >> "$out_file"
done

# === 生成 overlay_file_list 数组 ===
cat <<EOF >> "$out_file"
// 所有 overlay 模块的描述表
const struct overlay_file firmware_file_list[] = {
EOF

if (( file_idx == 0 )); then
    echo "No .bin files found in $overlay_dir"
else
    # [修改点4] 遍历生成的 struct_name
    for name in "${!name_map[@]}"; do
        array_name="${name_map[$name]}"
        count="${count_map[$name]}"
        orig_size="${orig_size_map[$name]}"
        # 输出时 .name 字段将包含 ".bin" 后缀，例如 "regdb.bin"
        printf '    { .name = "%s", .data = %s, .len = %d, .orig_size = %d },\n' \
               "$name" "$array_name" "$count" "$orig_size" >> "$out_file"
    done
fi

cat <<EOF >> "$out_file"
};

const int firmware_file_list_count = $file_idx;

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
