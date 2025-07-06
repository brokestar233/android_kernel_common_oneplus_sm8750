#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/of.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/moduleparam.h>
#include <linux/ctype.h>
#include "../of_private.h"

#include "overwrite_configs.h"

#define PATCH_TAG "overwrite_configs"


/* Parse a space-separated list of numbers (hex or decimal) into a binary array, each number converted to 4 bytes */
static int parse_numbers(const char *value_str, u8 **out_buf, size_t *out_len)
{
    char *dup, *p, *token_start;
    u8 *buf;
    size_t count = 0, i = 0;
    unsigned long val;
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
                
                if (endptr == token_start || *endptr != '\0' || val > 0xFFFF) {
                    pr_err("parse_numbers: invalid number '%s' (must be <= 0xFFFF)\n", token_start);
                    kfree(buf);
                    kfree(dup);
                    return -EINVAL;
                }
                
                /* Convert to 4-byte big-endian (0x0000XXXX) */
                buf[i * 4 + 0] = 0x00;               /* High byte padding */
                buf[i * 4 + 1] = 0x00;               /* High byte padding */
                buf[i * 4 + 2] = (val >> 8) & 0xFF;  /* High byte of value */
                buf[i * 4 + 3] = val & 0xFF;         /* Low byte of value */
                
                pr_info("parse_numbers: parsed value[%zu] = 0x%04lx -> [0x%02x, 0x%02x, 0x%02x, 0x%02x]\n", 
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
        
        if (endptr == token_start || *endptr != '\0' || val > 0xFFFF) {
            pr_err("parse_numbers: invalid number '%s' (must be <= 0xFFFF)\n", token_start);
            kfree(buf);
            kfree(dup);
            return -EINVAL;
        }
        
        /* Convert to 4-byte big-endian (0x0000XXXX) */
        buf[i * 4 + 0] = 0x00;               /* High byte padding */
        buf[i * 4 + 1] = 0x00;               /* High byte padding */
        buf[i * 4 + 2] = (val >> 8) & 0xFF;  /* High byte of value */
        buf[i * 4 + 3] = val & 0xFF;         /* Low byte of value */
        
        pr_info("parse_numbers: parsed final value[%zu] = 0x%04lx -> [0x%02x, 0x%02x, 0x%02x, 0x%02x]\n", 
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
    unsigned long flags;  // 添加 flags 变量声明
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
    unsigned long flags;  // 添加 flags 变量声明
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
    char operation;

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

    /* Check if this is a remove operation */
    if (dup[0] == 'r' || dup[0] == 'd') {
        operation = dup[0];
        
        /* Skip operation character and following space */
        char *op_input = dup + 1;
        while (*op_input && (*op_input == ' ' || *op_input == '\t'))
            op_input++;
        
        if (operation == 'r') {
            /* Remove node: "r /path/to/node" */
            ret = remove_dt_node(op_input);
            kfree(dup);
            return ret;
        } else if (operation == 'd') {
            /* Remove property: "d /path/to/node/property" */
            /* Find the last '/' to separate path and property */
            char *last_slash = strrchr(op_input, '/');
            if (!last_slash || last_slash == op_input) {
                pr_err("%s: Invalid format for remove property operation: no valid path\n", PATCH_TAG);
                kfree(dup);
                return -EINVAL;
            }
            
            /* Split path and property name */
            *last_slash = '\0';
            path = op_input;
            prop_name = last_slash + 1;
            
            /* Remove leading/trailing whitespace from property name */
            while (*prop_name && (*prop_name == ' ' || *prop_name == '\t'))
                prop_name++;
            
            pr_info("%s: Parsed path: '%s'\n", PATCH_TAG, path);
            pr_info("%s: Property name: '%s'\n", PATCH_TAG, prop_name);
            
            ret = remove_dt_property(path, prop_name);
            kfree(dup);
            return ret;
        }
    }
    
    /* Find the first space to separate path and value */
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
        size_t final_len = max(bin_len, (size_t)prop->length);
        
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

    /* Verify the updated value */
    if (prop->length > 0) {
        if (is_string_value) {
            pr_info("%s: Verification - string value: '%s', length: %d\n", PATCH_TAG, (char *)prop->value, prop->length);
        } else {
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
    for (int i = 0; i < overwrite_config_line_count; i++) {
        const char *line = overwrite_config_lines[i];

        patch_device_tree(line);
    }
    
    return 0;
}

// 使用尽可能早的初始化为硬件设置
early_initcall(overwrite_config_init);