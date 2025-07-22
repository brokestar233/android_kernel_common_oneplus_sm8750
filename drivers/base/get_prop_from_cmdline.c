#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/get_prop_from_cmdline.h>

char *get_property_from_cmdline(const char *input)
{
    char *cmdline = (char *)saved_command_line;
    char *prop_buf = NULL;
    char *prop_start, *prop_end;
    int len;

    pr_info("Kernel cmdline: %s\n", cmdline);

    char *prop_prefix = kmalloc(strlen(input) + 2, GFP_ATOMIC);
    if (!prop_prefix) {
        pr_err("Failed to allocate memory for property prefix\n");
        return NULL;
    }
    snprintf(prop_prefix, strlen(input) + 2, "%s=", input);

    prop_start = strstr(cmdline, prop_prefix);
    if (prop_start) {
        prop_start += strlen(prop_prefix);
        prop_end = strchr(prop_start, ' ');
        len = prop_end ? (prop_end - prop_start) : strlen(prop_start);

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
    return prop_buf;
}
