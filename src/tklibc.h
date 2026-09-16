/* tklibc.h — the handful of pure memory/string primitives used instead of the
 * C runtime's, so the binary imports no CRT module at all. See tklibc.c.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TIMEKEEPER_TKLIBC_H
#define TIMEKEEPER_TKLIBC_H

#include <stddef.h>

void       *tk_memmove(void *dst, const void *src, size_t n);
void       *tk_memcpy(void *dst, const void *src, size_t n);
void       *tk_memset(void *dst, int c, size_t n);
int         tk_memcmp(const void *a, const void *b, size_t n);
size_t      tk_strlen(const char *s);
int         tk_strcmp(const char *a, const char *b);
int         tk_strncmp(const char *a, const char *b, size_t n);
const char *tk_strpbrk(const char *s, const char *set);
const char *tk_strstr(const char *haystack, const char *needle);

#endif /* TIMEKEEPER_TKLIBC_H */
