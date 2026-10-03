
#include <linux/kernel.h>
#include <linux/string.h>

size_t okrapm_strip_newline(char *s, size_t len)
{
	while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
		s[--len] = '\0';
	return len;
}
