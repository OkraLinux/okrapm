// SPDX-License-Identifier: GPL-2.0

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/string.h>

/* Stub handler implementations for okrapm kernel module */

/* In a full implementation these would wrap the user-space okrapmlib APIs */

void okrapm_install(const char *pkg)
{
    pr_info("okrapm: (stub) install called for %s\n", pkg);
}

void okrapm_remove(const char *pkg)
{
    pr_info("okrapm: (stub) remove called for %s\n", pkg);
}

ssize_t okrapm_list(char *buf, size_t size)
{
    return scnprintf(buf, size, "[stub] package list empty\n");
}
