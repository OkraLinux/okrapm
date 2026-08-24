#ifndef _OKRAPM_H_
#define _OKRAPM_H_

#include <linux/kernel.h>
#include <linux/types.h>

/* Stub API – real implementation would wrap okrapmlib */
void okrapm_install(const char *pkg);
void okrapm_remove(const char *pkg);
ssize_t okrapm_list(char *buf, size_t size);

#endif /* _OKRAPM_H_ */
