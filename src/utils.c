// SPDX-License-Identifier: GPL-2.0

/*
 * okrapm utility functions
 */

#include <linux/kernel.h>
#include <linux/string.h>

/*
 * okrapm_strip_newline - remove trailing newline from user input
 * @s:   input string (modified in place)
 * @len: length of string
 *
 * Returns the new length after stripping.
 */
size_t okrapm_strip_newline(char *s, size_t len)
{
	while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
		s[--len] = '\0';
	return len;
}
