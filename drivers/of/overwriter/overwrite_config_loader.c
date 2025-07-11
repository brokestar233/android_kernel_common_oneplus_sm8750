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

    pr_info("%s: Creating property '%s' in node '%s' with value '%s'\n", PATCH_TAG, prop_name, path, value);

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

    /* 检查value是否为空 */
    if (!value) {
        /* 创建空属性 */
        prop->length = 0;
        prop->value = NULL;
        pr_info("%s: Created empty property '%s'\n", PATCH_TAG, prop_name);
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
            ((char *)prop->value)[str_len] = '\0';  /* 确保null终止 */
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

static char *get_property_from_cmdline(const char *input)
{
    char *cmdline = saved_command_line;
    char *prop_buf = NULL;
    char *prop_start, *prop_end;
    int len;

    pr_info("Kernel cmdline: %s\n", cmdline);

    /* Construct the property prefix (e.g., "input=") */
    char *prop_prefix = kmalloc(strlen(input) + 2, GFP_ATOMIC);
    if (!prop_prefix) {
        pr_err("Failed to allocate memory for property prefix\n");
        return NULL;
    }
    snprintf(prop_prefix, strlen(input) + 2, "%s=", input);

    /* Find the property in the command line */
    prop_start = strstr(cmdline, prop_prefix);
    if (prop_start) {
        prop_start += strlen(prop_prefix);
        prop_end = strchr(prop_start, ' ');
        len = prop_end ? (prop_end - prop_start) : strlen(prop_start);

        /* Allocate memory for the property value */
        prop_buf = kmalloc(len + 1, GFP_ATOMIC);
        if (prop_buf) {
            strncpy(prop_buf, prop_start, len);
            prop_buf[len] = '\0';
            pr_info("Found property %s: %s\n", input, prop_buf);
        } else {
            pr_err("Failed to allocate memory for property value\n");
        }
    } else {
        pr_err("Property %s not found in cmdline\n", input);
    }

    kfree(prop_prefix);
    return prop_buf ? prop_buf : NULL;
}

static int __init overwrite_config_init(void)
{
    char *device_name = get_property_from_cmdline("oplusboot.prjname");
    
    for (int i = 0; i < overwrite_config_line_count; i++) {
        const char *line = overwrite_config_lines[i];
        char *line_copy = kstrdup(line, GFP_ATOMIC);
        if (!line_copy) {
            pr_err("Failed to allocate memory for line copy\n");
            continue;
        }

        /* Split prefix and value */
        char *prefix = line_copy;
        char *value = strchr(line_copy, ':');
        if (value) {
            *value = '\0'; /* Null-terminate prefix */
            value++;       /* Move to the value part */
            
            /* Check if prefix matches device_name or "common" */
            if ((device_name && strcmp(prefix, device_name) == 0) || 
                strcmp(prefix, "common") == 0) {
                patch_device_tree(value);
                pr_info("Applied patch for prefix %s with value %s\n", prefix, value);
            } else {
                pr_info("Skipped patch for prefix %s (device: %s)\n", prefix, device_name ? device_name : "none");
            }
        } else {
            pr_err("Invalid line format: %s\n", line);
        }
        
        kfree(line_copy);
    }
    
    kfree(device_name);
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