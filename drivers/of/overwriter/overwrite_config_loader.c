#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/of.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/moduleparam.h>
#include <linux/ctype.h>
#include <linux/get_prop_from_cmdline.h>

#include "../of_private.h"

#include "overwrite_configs.h"

#define PATCH_TAG "overwrite_configs"


/* Parse a space-separated list of numbers (hex or decimal) into a binary array, each number converted to 4 bytes */
static int parse_numbers(const char *value_str, u8 **out_buf, size_t *out_len)
{
    char *dup, *p, *token_start;
    u8 *buf;
    size_t count = 0, i = 0;
    unsigned long val; // 使用 unsigned long 来存储更大的值
    char *endptr;
    bool in_token = false;

    /* Duplicate value string for parsing */
    dup = kstrdup(value_str, GFP_ATOMIC);
    if (!dup)
        return -ENOMEM;

    /* First pass: count number of tokens */
    p = dup;
    while (*p) {
        if (*p != ' ' && *p != '\t' && *p != '\n') {
            if (!in_token) {
                count++;
                in_token = true;
            }
        } else {
            in_token = false;
        }
        p++;
    }

    if (count == 0) {
        kfree(dup);
        return -EINVAL;
    }

    pr_info("parse_numbers: found %zu tokens in '%s'\n", count, value_str);

    /* Allocate buffer for binary values, each number takes 4 bytes */
    buf = kmalloc(count * 4, GFP_ATOMIC);
    if (!buf) {
        kfree(dup);
        return -ENOMEM;
    }

    /* Second pass: parse each token */
    p = dup;
    token_start = NULL;
    in_token = false;
    
    while (*p && i < count) {
        if (*p != ' ' && *p != '\t' && *p != '\n') {
            if (!in_token) {
                token_start = p;
                in_token = true;
            }
        } else {
            if (in_token) {
                /* End of token, null-terminate and parse */
                *p = '\0';
                
                pr_info("parse_numbers: parsing token '%s'\n", token_start);
                
                if (strncmp(token_start, "0x", 2) == 0 || strncmp(token_start, "0X", 2) == 0) {
                    val = simple_strtoul(token_start, &endptr, 16);
                } else {
                    val = simple_strtoul(token_start, &endptr, 10);
                }
                
                // 修改最大值检查，允许32位值
                if (endptr == token_start || *endptr != '\0' || val > 0xFFFFFFFFUL) {
                    pr_err("parse_numbers: invalid number '%s' (must be <= 0xFFFFFFFF)\n", token_start);
                    kfree(buf);
                    kfree(dup);
                    return -EINVAL;
                }
                
                // 将值转换为4字节大端格式
                buf[i * 4 + 0] = (u8)((val >> 24) & 0xFF); // Highest byte
                buf[i * 4 + 1] = (u8)((val >> 16) & 0xFF); // High byte
                buf[i * 4 + 2] = (u8)((val >> 8) & 0xFF);  // Low byte
                buf[i * 4 + 3] = (u8)(val & 0xFF);         // Lowest byte
                
                pr_info("parse_numbers: parsed value[%zu] = 0x%08lx -> [0x%02x, 0x%02x, 0x%02x, 0x%02x]\n", 
                        i, val, buf[i * 4 + 0], buf[i * 4 + 1], buf[i * 4 + 2], buf[i * 4 + 3]);
                i++;
                in_token = false;
            }
        }
        p++;
    }
    
    /* Handle last token if string doesn't end with whitespace */
    if (in_token && token_start && i < count) {
        pr_info("parse_numbers: parsing final token '%s'\n", token_start);
        
        if (strncmp(token_start, "0x", 2) == 0 || strncmp(token_start, "0X", 2) == 0) {
            val = simple_strtoul(token_start, &endptr, 16);
        } else {
            val = simple_strtoul(token_start, &endptr, 10);
        }
        
        // 修改最大值检查，允许32位值
        if (endptr == token_start || *endptr != '\0' || val > 0xFFFFFFFFUL) {
            pr_err("parse_numbers: invalid number '%s' (must be <= 0xFFFFFFFF)\n", token_start);
            kfree(buf);
            kfree(dup);
            return -EINVAL;
        }
        
        // 将值转换为4字节大端格式
        buf[i * 4 + 0] = (u8)((val >> 24) & 0xFF); // Highest byte
        buf[i * 4 + 1] = (u8)((val >> 16) & 0xFF); // High byte
        buf[i * 4 + 2] = (u8)((val >> 8) & 0xFF);  // Low byte
        buf[i * 4 + 3] = (u8)(val & 0xFF);         // Lowest byte
        
        pr_info("parse_numbers: parsed final value[%zu] = 0x%08lx -> [0x%02x, 0x%02x, 0x%02x, 0x%02x]\n", 
                i, val, buf[i * 4 + 0], buf[i * 4 + 1], buf[i * 4 + 2], buf[i * 4 + 3]);
        i++;
    }

    *out_buf = buf;
    *out_len = i * 4;  /* Total length is number of values times 4 bytes */
    kfree(dup);
    
    pr_info("parse_numbers: successfully parsed %zu values (%zu bytes)\n", i, *out_len);
    return 0;
}

/* Check if a character is a hex digit */
static bool is_hex_digit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* Check if a character is a decimal digit */
static bool is_decimal_digit(char c)
{
    return (c >= '0' && c <= '9');
}

/* Check if a string contains only numeric values (hex or decimal) */
static bool is_numeric_value(const char *value)
{
    const char *p = value;
    bool has_non_space = false;
    
    /* Skip leading whitespace */
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n'))
        p++;
    
    /* Check each token */
    while (*p) {
        /* Skip whitespace between tokens */
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n'))
            p++;
        
        if (!*p)
            break;
        
        has_non_space = true;
        
        /* Check if token looks like a number */
        if (strncmp(p, "0x", 2) == 0 || strncmp(p, "0X", 2) == 0) {
            /* Hex number */
            p += 2;
            if (!*p || (!is_hex_digit(*p)))
                return false;
            while (*p && is_hex_digit(*p))
                p++;
        } else if (is_decimal_digit(*p)) {
            /* Decimal number */
            while (*p && is_decimal_digit(*p))
                p++;
        } else {
            /* Not a number */
            return false;
        }
        
        /* Should be end of string or whitespace */
        if (*p && *p != ' ' && *p != '\t' && *p != '\n')
            return false;
    }
    
    return has_non_space;
}

/* Remove a device tree node by unlinking it from parent's child list */
static int __init remove_dt_node(const char *path)
{
    struct device_node *np, *parent, *child, *prev_child;
    unsigned long flags;
    int ret = 0;

    pr_info("%s: Removing node: '%s'\n", PATCH_TAG, path);

    /* Find the device tree node */
    np = of_find_node_by_path(path);
    if (!np) {
        pr_err("%s: DT node not found: '%s'\n", PATCH_TAG, path);
        return -ENODEV;
    }

    /* Get parent node */
    parent = of_get_parent(np);
    if (!parent) {
        pr_err("%s: Cannot remove root node: '%s'\n", PATCH_TAG, path);
        of_node_put(np);
        return -EINVAL;
    }

    /* Find and unlink the node from parent's child list */
    raw_spin_lock_irqsave(&devtree_lock, flags);
    
    child = parent->child;
    prev_child = NULL;
    
    while (child) {
        if (child == np) {
            /* Found the node to remove */
            if (prev_child) {
                prev_child->sibling = child->sibling;
            } else {
                parent->child = child->sibling;
            }
            
            /* Clear the node's parent and sibling pointers */
            child->parent = NULL;
            child->sibling = NULL;
            
            pr_info("%s: Successfully removed node: '%s'\n", PATCH_TAG, path);
            break;
        }
        prev_child = child;
        child = child->sibling;
    }
    
    raw_spin_unlock_irqrestore(&devtree_lock, flags);
    
    of_node_put(parent);
    of_node_put(np);
    return ret;
}

/* Remove a device tree property by unlinking it from property list */
static int __init remove_dt_property(const char *path, const char *prop_name)
{
    struct device_node *np;
    struct property *prop, *prev_prop;
    unsigned long flags;
    int ret = 0;

    pr_info("%s: Removing property '%s' from node: '%s'\n", PATCH_TAG, prop_name, path);

    /* Find the device tree node */
    np = of_find_node_by_path(path);
    if (!np) {
        pr_err("%s: DT node not found: '%s'\n", PATCH_TAG, path);
        return -ENODEV;
    }

    /* Find and unlink the property from the node's property list */
    raw_spin_lock_irqsave(&devtree_lock, flags);
    
    prop = np->properties;
    prev_prop = NULL;
    
    while (prop) {
        if (strcmp(prop->name, prop_name) == 0) {
            /* Found the property to remove */
            if (prev_prop) {
                prev_prop->next = prop->next;
            } else {
                np->properties = prop->next;
            }
            
            /* Clear the property's next pointer */
            prop->next = NULL;
            
            pr_info("%s: Successfully removed property '%s' from node '%s'\n", PATCH_TAG, prop_name, path);
            ret = 0;
            break;
        }
        prev_prop = prop;
        prop = prop->next;
    }
    
    if (!prop) {
        pr_err("%s: Property '%s' not found in node '%s'\n", PATCH_TAG, prop_name, path);
        ret = -EINVAL;
    }
    
    raw_spin_unlock_irqrestore(&devtree_lock, flags);
    
    of_node_put(np);
    return ret;
}

/* Create a new device tree node */
static int __init create_dt_node(const char *path)
{
    struct device_node *np, *parent;
    char *node_name, *parent_path;
    unsigned long flags;
    int ret = 0;

    pr_info("%s: Creating node: '%s'\n", PATCH_TAG, path);

    /* Find the last '/' to separate parent path and node name */
    node_name = strrchr(path, '/');
    if (!node_name || node_name == path) {
        pr_err("%s: Invalid path format for node creation: '%s'\n", PATCH_TAG, path);
        return -EINVAL;
    }

    /* Duplicate path to split */
    parent_path = kstrdup(path, GFP_ATOMIC);
    if (!parent_path) {
        pr_err("%s: Failed to allocate memory for parent path\n", PATCH_TAG);
        return -ENOMEM;
    }

    /* Split parent path and node name */
    parent_path[node_name - path] = '\0';
    node_name++;

    /* Find parent node */
    parent = of_find_node_by_path(parent_path);
    if (!parent) {
        pr_err("%s: Parent node not found: '%s'\n", PATCH_TAG, parent_path);
        kfree(parent_path);
        return -ENODEV;
    }

    /* Check if node already exists */
    np = of_find_node_by_path(path);
    if (np) {
        pr_info("%s: Node '%s' already exists\n", PATCH_TAG, path);
        of_node_put(np);
        of_node_put(parent);
        kfree(parent_path);
        return 0;
    }

    /* Create new node */
    np = kzalloc(sizeof(*np), GFP_ATOMIC);
    if (!np) {
        pr_err("%s: Failed to allocate memory for new node\n", PATCH_TAG);
        of_node_put(parent);
        kfree(parent_path);
        return -ENOMEM;
    }

    /* 初始化node的关键字段 */
    of_node_init(np);  /* 如果内核版本支持，调用此函数 */

    np->name = kstrdup(node_name, GFP_ATOMIC);
    if (!np->name) {
        pr_err("%s: Failed to allocate memory for node name\n", PATCH_TAG);
        kfree(np);
        of_node_put(parent);
        kfree(parent_path);
        return -ENOMEM;
    }

    np->full_name = kstrdup(path, GFP_ATOMIC);
    if (!np->full_name) {
        pr_err("%s: Failed to allocate memory for full node name\n", PATCH_TAG);
        kfree(np->name);
        kfree(np);
        of_node_put(parent);
        kfree(parent_path);
        return -ENOMEM;
    }

    /* Link node to parent */
    raw_spin_lock_irqsave(&devtree_lock, flags);
    np->parent = of_node_get(parent);  /* 增加parent的引用计数 */
    np->sibling = parent->child;
    parent->child = np;
    raw_spin_unlock_irqrestore(&devtree_lock, flags);

    pr_info("%s: Successfully created node: '%s'\n", PATCH_TAG, path);

    of_node_put(parent);
    kfree(parent_path);
    return ret;
}

/*
 * 新增函数: parse_hex_bytes
 * 解析形如 "[00 11 22 FF]" 的十六进制字节字符串，将其转换为二进制字节数组。
 */
static int parse_hex_bytes(const char *value_str, u8 **out_buf, size_t *out_len)
{
    char *dup, *p, *token_start;
    u8 *buf;
    size_t count = 0, i = 0;
    unsigned long val;
    char *endptr;

    // 确保字符串以 '[' 开头，以 ']' 结尾，并且至少有 "[]"
    if (!value_str || strlen(value_str) < 2 || value_str[0] != '[' || value_str[strlen(value_str) - 1] != ']') {
        pr_err("parse_hex_bytes: Invalid format, must be like '[00 FF 12]'\n");
        return -EINVAL;
    }

    // 复制内部内容，跳过 '[' 和 ']'
    dup = kstrdup(value_str + 1, GFP_ATOMIC);
    if (!dup)
        return -ENOMEM;
    dup[strlen(dup) - 1] = '\0'; // 移除末尾的 ']'

    // 第一遍：计算字节数量
    p = dup;
    while (*p) {
        if (is_hex_digit(*p)) {
            // 找到一个十六进制字符，假设这是一个字节的开始
            count++;
            // 跳过当前字节的两个十六进制字符和可能的空格
            p++; 
            if (is_hex_digit(*p)) p++;
            while (*p && (*p == ' ' || *p == '\t' || *p == '\n')) p++;
        } else if (*p == ' ' || *p == '\t' || *p == '\n') {
            p++;
        } else {
            pr_err("parse_hex_bytes: Invalid character '%c' in hex string '%s'\n", *p, value_str);
            kfree(dup);
            return -EINVAL;
        }
    }

    if (count == 0) {
        pr_info("parse_hex_bytes: No hex bytes found in '%s'\n", value_str);
        *out_buf = NULL;
        *out_len = 0;
        kfree(dup);
        return 0;
    }

    pr_info("parse_hex_bytes: found %zu bytes in '%s'\n", count, value_str);

    buf = kmalloc(count, GFP_ATOMIC);
    if (!buf) {
        kfree(dup);
        return -ENOMEM;
    }

    // 第二遍：解析每个字节
    p = dup;
    i = 0;
    while (*p && i < count) {
        // 跳过空格
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n')) p++;

        if (!*p) break; // 已经处理完所有内容

        token_start = p;
        // 确保有两个十六进制字符
        if (!is_hex_digit(token_start[0]) || !is_hex_digit(token_start[1])) {
            pr_err("parse_hex_bytes: Invalid hex byte format '%s' in '%s'\n", token_start, value_str);
            kfree(buf);
            kfree(dup);
            return -EINVAL;
        }
        p += 2; // 跳过两个十六进制字符

        char byte_str[3];
        strncpy(byte_str, token_start, 2);
        byte_str[2] = '\0';

        val = simple_strtoul(byte_str, &endptr, 16);
        if (endptr == byte_str || *endptr != '\0') {
            pr_err("parse_hex_bytes: invalid hex byte '%s'\n", byte_str);
            kfree(buf);
            kfree(dup);
            return -EINVAL;
        }
        
        buf[i++] = (u8)val;
    }

    *out_buf = buf;
    *out_len = i;
    kfree(dup);

    pr_info("parse_hex_bytes: successfully parsed %zu bytes\n", i);
    return 0;
}


/* Create a new device tree property with a value */
static int __init create_dt_property(const char *path, const char *prop_name, const char *value)
{
    struct device_node *np;
    struct property *prop;
    unsigned long flags;
    u8 *bin_value = NULL;
    size_t bin_len;
    int ret = 0;
    bool is_string_value;
    bool is_hex_byte_value = false;

    pr_info("%s: Creating property '%s' in node '%s' with value '%s'\n", PATCH_TAG, prop_name, path, value ? value : "(null)");

    /* Find the device tree node */
    np = of_find_node_by_path(path);
    if (!np) {
        pr_err("%s: DT node not found: '%s'\n", PATCH_TAG, path);
        return -ENODEV;
    }

    /* Check if property already exists */
    prop = of_find_property(np, prop_name, NULL);
    if (prop) {
        pr_info("%s: Property '%s' already exists in node '%s'\n", PATCH_TAG, prop_name, path);
        of_node_put(np);
        return -EEXIST;
    }

    /* Allocate new property */
    prop = kzalloc(sizeof(*prop), GFP_ATOMIC);
    if (!prop) {
        pr_err("%s: Failed to allocate memory for new property\n", PATCH_TAG);
        of_node_put(np);
        return -ENOMEM;
    }

    /* Set property name */
    prop->name = kstrdup(prop_name, GFP_ATOMIC);
    if (!prop->name) {
        pr_err("%s: Failed to allocate memory for property name\n", PATCH_TAG);
        kfree(prop);
        of_node_put(np);
        return -ENOMEM;
    }

    /* Check if value is NULL or empty string */
    if (!value || value[0] == '\0') {
        // Modified: When value is NULL or empty, write [00]
        bin_value = kzalloc(1, GFP_ATOMIC); // Allocate 1 byte for 0x00
        if (!bin_value) {
            pr_err("%s: Failed to allocate memory for [00] value\n", PATCH_TAG);
            kfree(prop->name);
            kfree(prop);
            of_node_put(np);
            return -ENOMEM;
        }
        bin_value[0] = 0x00; // Set the byte to 0x00
        prop->length = 1;
        prop->value = bin_value;
        pr_info("%s: Created property '%s' with default [00] value due to empty input\n", PATCH_TAG, prop_name);
    } else {
        // Check if it's a hex byte array format
        if (value[0] == '[' && value[strlen(value) - 1] == ']') {
            is_hex_byte_value = true;
            ret = parse_hex_bytes(value, &bin_value, &bin_len);
            if (ret != 0) {
                pr_err("%s: Failed to parse hex byte value '%s': %d\n", PATCH_TAG, value, ret);
                kfree(prop->name);
                kfree(prop);
                of_node_put(np);
                return ret;
            }
            prop->length = bin_len;
            prop->value = bin_value;
            pr_info("%s: Created hex byte property '%s' with length %zu\n", PATCH_TAG, prop_name, bin_len);
        } else {
            /* Determine if this is a string or numeric value */
            is_string_value = !is_numeric_value(value);

            if (is_string_value) {
                /* Handle as string value */
                size_t str_len = strlen(value);
                prop->length = str_len + 1;  /* Include null terminator */
                prop->value = kzalloc(prop->length, GFP_ATOMIC);
                if (!prop->value) {
                    pr_err("%s: Failed to allocate memory for string value\n", PATCH_TAG);
                    kfree(prop->name);
                    kfree(prop);
                    of_node_put(np);
                    return -ENOMEM;
                }
                memcpy(prop->value, value, str_len);
                ((char *)prop->value)[str_len] = '\0';  /* Ensure null termination */
                pr_info("%s: Created string property '%s' with value '%s' (len=%d)\n",
                        PATCH_TAG, prop_name, value, prop->length);
            } else {
                /* Handle as numeric value */
                ret = parse_numbers(value, &bin_value, &bin_len);
                if (ret != 0) {
                    pr_err("%s: Failed to parse numeric value '%s': %d\n", PATCH_TAG, value, ret);
                    kfree(prop->name);
                    kfree(prop);
                    of_node_put(np);
                    return ret;
                }

                prop->length = bin_len;
                prop->value = bin_value;
                pr_info("%s: Created numeric property '%s' with length %zu\n", PATCH_TAG, prop_name, bin_len);
            }
        }
    }

    /* Link property to node */
    raw_spin_lock_irqsave(&devtree_lock, flags);
    prop->next = np->properties;
    np->properties = prop;
    raw_spin_unlock_irqrestore(&devtree_lock, flags);

    of_node_put(np);
    return ret;
}

static int __init patch_device_tree(const char *input)
{
    struct device_node *np;
    struct property *prop;
    char *dup, *path, *prop_name, *value, *space_pos;
    u8 *bin_value = NULL;
    void *old_value = NULL;
    size_t bin_len;
    int ret;
    bool is_string_value = false;
    bool is_hex_byte_value = false;
    char operation;
    char *op_input;
    size_t final_len;

    if (!input || strlen(input) == 0) {
        pr_err("%s: Invalid input\n", PATCH_TAG);
        return -EINVAL;
    }

    pr_info("%s: Input string: '%s'\n", PATCH_TAG, input);

    dup = kstrdup(input, GFP_ATOMIC);
    if (!dup) {
        pr_err("%s: Failed to duplicate input string\n", PATCH_TAG);
        return -ENOMEM;
    }

    /* Check operation type */
    operation = dup[0];
    if (operation == 'r' || operation == 'd' || operation == 'c' || operation == 'a') {
        /* Skip operation character and following space */
        op_input = dup + 1;
        while (*op_input && (*op_input == ' ' || *op_input == '\t'))
            op_input++;
        
        if (operation == 'r') {
            /* Remove node: "r /path/to/node" */
            ret = remove_dt_node(op_input);
            kfree(dup);
            return ret;
        } else if (operation == 'd') {
            /* Remove property: "d /path/to/node/property" */
            char *last_slash = strrchr(op_input, '/');
            if (!last_slash || last_slash == op_input) {
                pr_err("%s: Invalid format for remove property operation: no valid path\n", PATCH_TAG);
                kfree(dup);
                return -EINVAL;
            }
            
            *last_slash = '\0';
            path = op_input;
            prop_name = last_slash + 1;
            
            while (*prop_name && (*prop_name == ' ' || *prop_name == '\t'))
                prop_name++;
            
            pr_info("%s: Parsed path: '%s'\n", PATCH_TAG, path);
            pr_info("%s: Property name: '%s'\n", PATCH_TAG, prop_name);
            
            ret = remove_dt_property(path, prop_name);
            kfree(dup);
            return ret;
        } else if (operation == 'c') {
            /* Create node: "c /path/to/node" */
            ret = create_dt_node(op_input);
            kfree(dup);
            return ret;
        } else if (operation == 'a') {
            /* Create property: "a /path/to/node/property value" */
            space_pos = strchr(op_input, ' ');
            if (!space_pos) {
                pr_err("%s: Invalid format for create property operation: no value specified\n", PATCH_TAG);
                kfree(dup);
                return -EINVAL;
            }
            
            *space_pos = '\0';
            path = op_input;
            value = space_pos + 1;
            
            while (*value && (*value == ' ' || *value == '\t' || *value == '\n'))
                value++;
            
            if (*value == '\0') {
                pr_err("%s: Empty value for create property operation\n", PATCH_TAG);
                kfree(dup);
                return -EINVAL;
            }
            
            prop_name = strrchr(path, '/');
            if (!prop_name || prop_name == path) {
                pr_err("%s: Invalid path format for create property: '%s'\n", PATCH_TAG, path);
                kfree(dup);
                return -EINVAL;
            }
            
            *prop_name = '\0';
            prop_name++;
            
            pr_info("%s: Parsed path: '%s'\n", PATCH_TAG, path);
            pr_info("%s: Property name: '%s'\n", PATCH_TAG, prop_name);
            pr_info("%s: Property value: '%s'\n", PATCH_TAG, value);
            
            ret = create_dt_property(path, prop_name, value);
            kfree(dup);
            return ret;
        }
    }
    
    /* Existing modify property operation */
    space_pos = strchr(dup, ' ');
    if (!space_pos) {
        pr_err("%s: Invalid input format, no space found\n", PATCH_TAG);
        kfree(dup);
        return -EINVAL;
    }
    
    /* Split at the space */
    *space_pos = '\0';
    path = dup;
    value = space_pos + 1;
    
    /* Skip leading whitespace in value */
    while (*value && (*value == ' ' || *value == '\t' || *value == '\n'))
        value++;
    
    pr_info("%s: Parsed path: '%s'\n", PATCH_TAG, path);
    pr_info("%s: Parsed value: '%s'\n", PATCH_TAG, value);

    if (*value == '\0') {
        pr_err("%s: Empty value after path\n", PATCH_TAG);
        kfree(dup);
        return -EINVAL;
    }

    /* Extract property name from path */
    prop_name = strrchr(path, '/');
    if (!prop_name || prop_name == path) {
        pr_err("%s: Invalid path format: '%s'\n", PATCH_TAG, path);
        kfree(dup);
        return -EINVAL;
    }
    
    /* Split path and property name */
    *prop_name = '\0';
    prop_name++;
    
    pr_info("%s: Node path: '%s'\n", PATCH_TAG, path);
    pr_info("%s: Property name: '%s'\n", PATCH_TAG, prop_name);

    /* Find the device tree node */
    np = of_find_node_by_path(path);
    if (!np) {
        pr_err("%s: DT node not found: '%s'\n", PATCH_TAG, path);
        kfree(dup);
        return -ENODEV;
    }

    /* Find the property */
    prop = of_find_property(np, prop_name, NULL);
    if (!prop) {
        pr_err("%s: Property '%s' not found in node '%s'\n", PATCH_TAG, prop_name, path);
        of_node_put(np);
        kfree(dup);
        return -EINVAL;
    }
    
    pr_info("%s: Found property '%s', current length: %d\n", PATCH_TAG, prop_name, prop->length);

    // 检查是否为十六进制字节数组格式
    if (value[0] == '[' && value[strlen(value) - 1] == ']') {
        is_hex_byte_value = true;
        ret = parse_hex_bytes(value, &bin_value, &bin_len);
        if (ret != 0) {
            pr_err("%s: Failed to parse hex byte value '%s': %d\n", PATCH_TAG, value, ret);
            of_node_put(np);
            kfree(dup);
            return ret;
        }

        pr_info("%s: Parsed %zu bytes from hex byte string\n", PATCH_TAG, bin_len);

        old_value = prop->value;
        final_len = bin_len; // 对于直接写入字节，长度就是解析出来的字节长度

        prop->value = kzalloc(final_len, GFP_ATOMIC);
        if (!prop->value) {
            pr_err("%s: Failed to allocate memory for hex byte value\n", PATCH_TAG);
            prop->value = old_value;
            kfree(bin_value);
            of_node_put(np);
            kfree(dup);
            return -ENOMEM;
        }
        memcpy(prop->value, bin_value, bin_len);
        prop->length = final_len;

        pr_info("%s: Patched %s/%s to hex bytes (len=%zu)\n", PATCH_TAG, path, prop_name, final_len);
        kfree(bin_value);

    } else {
        /* Determine if this is a string value or numeric value */
        is_string_value = !is_numeric_value(value);
        
        if (is_string_value) {
            /* Handle as string value */
            size_t str_len = strlen(value);
            size_t final_len = str_len + 1;  /* Include null terminator */
            
            pr_info("%s: Treating value as string (len=%zu)\n", PATCH_TAG, str_len);
            
            /* Save old value and update property */
            old_value = prop->value;
            
            prop->value = kzalloc(final_len, GFP_ATOMIC);
            if (!prop->value) {
                pr_err("%s: Failed to allocate memory for string value\n", PATCH_TAG);
                prop->value = old_value;  /* Restore old value */
                of_node_put(np);
                kfree(dup);
                return -ENOMEM;
            }
            
            /* Copy the string value */
            memcpy(prop->value, value, str_len);
            prop->length = final_len;
            
            pr_info("%s: Patched %s/%s to string '%s' (len=%zu)\n", PATCH_TAG, path, prop_name, value, final_len);
        } else {
            /* Handle as numeric value - parse the numbers */
            ret = parse_numbers(value, &bin_value, &bin_len);
            if (ret != 0) {
                pr_err("%s: Failed to parse numeric value '%s': %d\n", PATCH_TAG, value, ret);
                of_node_put(np);
                kfree(dup);
                return ret;
            }

            pr_info("%s: Parsed %zu bytes from numeric value string\n", PATCH_TAG, bin_len);

            /* Save old value and update property */
            old_value = prop->value;
            
            /* If the parsed length is smaller than original, pad with zeros */
            final_len = max(bin_len, (size_t)prop->length);
            
            prop->value = kzalloc(final_len, GFP_ATOMIC);  /* kzalloc zeros the memory */
            if (!prop->value) {
                pr_err("%s: Failed to allocate memory for numeric value\n", PATCH_TAG);
                prop->value = old_value;  /* Restore old value */
                kfree(bin_value);
                of_node_put(np);
                kfree(dup);
                return -ENOMEM;
            }
            
            /* Copy the parsed data */
            memcpy(prop->value, bin_value, bin_len);
            prop->length = final_len;

            pr_info("%s: Updated property length from %d to %zu bytes\n", PATCH_TAG, 
                     prop->length, final_len);

            /* Create hex string for logging */
            if (bin_len > 0) {
                size_t hex_str_size = bin_len * 5 + 1;  /* "0xNN " per byte + null */
                char *hex_str = kmalloc(hex_str_size, GFP_ATOMIC);
                if (hex_str) {
                    size_t j, pos = 0;
                    hex_str[0] = '\0';  /* Initialize string */
                    for (j = 0; j < bin_len; j++) {
                        int written = snprintf(hex_str + pos, hex_str_size - pos, "0x%02x", bin_value[j]);
                        if (written > 0 && pos + written < hex_str_size) {
                            pos += written;
                        }
                        if (j < bin_len - 1 && pos < hex_str_size - 1) {
                            hex_str[pos++] = ' ';
                            hex_str[pos] = '\0';
                        }
                    }
                    pr_info("%s: Patched %s/%s to %s (len=%zu)\n", PATCH_TAG, path, prop_name, hex_str, bin_len);
                    kfree(hex_str);
                } else {
                    pr_info("%s: Patched %s/%s to binary data (%zu bytes)\n", PATCH_TAG, path, prop_name, bin_len);
                }
            }
            
            /* Clean up numeric value buffer */
            kfree(bin_value);
        }
    }


    /* Verify the updated value */
    if (prop->length > 0) {
        if (is_string_value) {
            pr_info("%s: Verification - string value: '%s', length: %d\n", PATCH_TAG, (char *)prop->value, prop->length);
        } else if (is_hex_byte_value) {
             // 打印十六进制字节验证
            if (prop->value && prop->length > 0) {
                size_t hex_str_size = prop->length * 3 + 1; // "NN " per byte + null
                char *hex_str = kmalloc(hex_str_size, GFP_ATOMIC);
                if (hex_str) {
                    size_t j, pos = 0;
                    hex_str[0] = '[';
                    pos = 1;
                    for (j = 0; j < prop->length; j++) {
                        int written = snprintf(hex_str + pos, hex_str_size - pos, "%02x ", ((u8 *)prop->value)[j]);
                        if (written > 0 && pos + written < hex_str_size) {
                            pos += written;
                        }
                    }
                    if (pos > 1) hex_str[pos-1] = ']'; // 替换最后一个空格为 ']'
                    else hex_str[pos++] = ']'; // 如果没有内容，直接加 ']'
                    hex_str[pos] = '\0';
                    pr_info("%s: Verification - hex byte value: '%s', length: %d\n", PATCH_TAG, hex_str, prop->length);
                    kfree(hex_str);
                } else {
                     pr_info("%s: Verification - hex byte value (binary), length: %d\n", PATCH_TAG, prop->length);
                }
            }
        }
        else { // numeric value
            u8 *val = (u8 *)prop->value;
            pr_info("%s: Verification - first byte: 0x%02x, length: %d\n", PATCH_TAG, val[0], prop->length);
        }
    }

    /* Clean up */
    of_node_put(np);
    kfree(dup);
    return 0;
}

static int __init overwrite_config_init(void)
{
    char *device_name = get_property_from_cmdline("oplusboot.prjname");
    char *enable = get_property_from_cmdline("overwrite.enable");
    
    const struct overwrite_config_group *common_group = NULL;
    const struct overwrite_config_group *device_group = NULL;

    // 第一遍遍历：找到 common 组和当前设备对应的组
    for (int i = 0; i < overwrite_config_group_count; i++) {
        const struct overwrite_config_group *group = &overwrite_config_groups[i];
        
        if (strcmp(group->prefix, "common") == 0) {
            common_group = group;
        } else if (device_name && strcmp(group->prefix, device_name) == 0) {
            device_group = group;
        }
    }

    // 应用 common 配置
    if (common_group) {
        pr_info("Applying common configs...\n");
        for (int i = 0; i < common_group->count; i++) {
            patch_device_tree(common_group->values[i]);
            pr_info("  Applied common patch: %s\n", common_group->values[i]);
        }
    } else {
        pr_info("No common configs found.\n");
    }

    if (enable && strcmp(enable, "0") == 0) {
        pr_info("Overwrite configs disabled.\n");
        goto out;
    }

    // 应用设备特有配置（如果存在）
    if (device_group) {
        pr_info("Applying device-specific configs for %s...\n", device_name);
        for (int i = 0; i < device_group->count; i++) {
            patch_device_tree(device_group->values[i]);
            pr_info("  Applied device patch: %s\n", device_group->values[i]);
        }
    } else {
        pr_info("No device-specific configs found for %s.\n", device_name ? device_name : "none");
    }

out:
    kfree(device_name); // 释放获取到的设备名内存
    return 0;
}

early_initcall(overwrite_config_init);

static int __init print_info(void)
{
    pr_info("|-----------------------------------------------------------------------------|\n");
    pr_info("|                                   ^7JJ?!:                                   |\n");
    pr_info("|                                :?P#BGPG#B5!:                                |\n");
    pr_info("|                      :^^^^^^^!5##P7^   ^JG#BJ~^^^^^^^:                      |\n");
    pr_info("|                  ^?PBBBBBBBBB#BJ^         !YB#BBBBBBBBB57:                  |\n");
    pr_info("|                 !B&P7~^^^^^^^^              :^^^^^^^^~?G&G~                 |\n");
    pr_info("|                !B&Y                                    :5&B~                |\n");
    pr_info("|               ~B&Y                                      :P&G^               |\n");
    pr_info("|           ^!J5B&P:              :^!7J!                   ^G&BY?!:           |\n");
    pr_info("|         !5##G5J7:           ^!YPB#BGP?                    ^7J5G##5~         |\n");
    pr_info("|        7##J^             ^?P##GY7~:                            ^Y##7        |\n");
    pr_info("|       :P&P             ~Y##P7^                                  :P&P        |\n");
    pr_info("|       :P&5           :Y##Y^        :!?5PGGGGP5Y?!:               5&P:       |\n");
    pr_info("|       :P&5          ~G&P~       :!5B#G5J?77??Y5G##P7:            5&P:       |\n");
    pr_info("|      :J##?         !B&Y        !G&G?^           ^?P#B?:          ?##J:      |\n");
    pr_info("|     7G&P~         ^G&5        ?##J:                !G&P^          !G&G!     |\n");
    pr_info("|   :Y#B?           J&B~       !##?                   ^P&P:          :J##?    |\n");
    pr_info("|   ?&#!           :P&5        Y&G^                    !##?            J#B~   |\n");
    pr_info("|   ?&B!           ^G#J        J&B~         ^7!:       :G&Y            ?#B~   |\n");
    pr_info("|   :5&B!          :G&Y        ^G&P^       :5&G:       :G&Y           ?##J    |\n");
    pr_info("|     ?B#5^         Y&G^        ^5#BY!^::^7P&G!        !##7         ~P&B7     |\n");
    pr_info("|      ^5#B7        ^G&5          ~YG#BBBBBP?:        ^G&5:        ?##Y:      |\n");
    pr_info("|       :P&5         ~B&5:           :~~~^:          ~G&P^         5&P:       |\n");
    pr_info("|       :P&5          ^P&B7                        :J##Y:          5&P:       |\n");
    pr_info("|       :P&5            7G&G?^                   ~YB#P~            P&P:       |\n");
    pr_info("|        ?##J:           :7P##PJ!^          :^7YG#BY~            ^J##7        |\n");
    pr_info("|         !P##PY?!:         ^75GBBBPP5YY55PGB#BPJ!:         :!?YP##P!         |\n");
    pr_info("|           ^7J5B&5:            :~7?JY555YJ?!^:            :P&B5J7^           |\n");
    pr_info("|               ~B&Y                                      :5&G^               |\n");
    pr_info("|                !#&J                                    :5&B~                |\n");
    pr_info("|                 7B&P7~^^^^^^^^              :^^^^^^^^~?G&G!                 |\n");
    pr_info("|                  ^?PBBBB#BB###GJ^         ~YB#####BBBBBP?:                  |\n");
    pr_info("|                     :^^^^~^^^!5##P7:   ^?G#BY~^^^^~^^:                      |\n");
    pr_info("|                                ^?P#BGPG#B57:                                |\n");
    pr_info("|                                   ^7JJ?!:                                   |\n");
    pr_info("|-----------------------------------------------------------------------------|\n");
    pr_info("|    Link: https://github.com/project-trans and https://project-trans.org/    |\n");
    pr_info("|-----------------------------------------------------------------------------|\n");
    pr_info("|    Never surrender to anxiety and depression,                               |\n");
    pr_info("|    we will eventually have our place in this world                          |\n");
    pr_info("|    If you can remember my name, if you can all remember my name,            |\n");
    pr_info("|    maybe I or \"we\", will be able to live freely one day                     |\n");
    pr_info("|-----------------------------------------------------------------------------|\n");
    return 0;
}

late_initcall(print_info)