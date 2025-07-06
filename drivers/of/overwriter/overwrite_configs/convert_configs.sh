#!/bin/sh
srctree=$(pwd)

> $srctree/drivers/of/overwriter/overwrite_configs.c

cat << EOF > $srctree/drivers/of/overwriter/overwrite_configs.c
#include "overwrite_configs.h"
#include <linux/stddef.h>

const char *overwrite_config_lines[] = {
EOF

line_count=0
for file in $srctree/../drivers/of/overwriter/overwrite_configs/*.conf; do
    while IFS= read -r line; do
        if [ -n "$line" ]; then
            echo "    \"$line\"," >> $srctree/drivers/of/overwriter/overwrite_configs.c
            ((line_count++))
        fi
    done < "$file"
done

cat << EOF >> $srctree/drivers/of/overwriter/overwrite_configs.c
    NULL
};

const int overwrite_config_line_count = $line_count;
EOF