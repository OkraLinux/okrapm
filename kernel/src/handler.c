
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/string.h>

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
