/* tklibc.c — the seven C-library primitives this program actually needs.
 *
 * With TK_NOCRT the only thing still dragging in an extra import module is
 * msvcrt.dll, and it is imported for exactly seven pure functions. Providing
 * them here removes that dependency entirely, so the binary's import table is
 * kernel32 + advapi32 + ws2_32 + wininet (+ user32 for one MessageBox), which
 * is what the specification asks for. tests/pe_audit.py enforces it.
 *
 * These are deliberately the boring, obviously-correct loops: no SIMD, no
 * locale, no wide-char overloading, no UB. Total cost is a few hundred bytes
 * of .text and it buys a smaller, more auditable dependency set.
 *
 * SPDX-License-Identifier: MIT
 */
#include "tklibc.h"

#include <stddef.h>
#include <stdint.h>

/* The names are ours (tk_*) on purpose: mingw-w64 declares the standard ones as
 * __declspec(dllimport), so "defining tk_memcpy" would silently keep the import and
 * the linker would still pull msvcrt.dll. Renaming makes the substitution
 * unambiguous, and the build adds -fno-tree-loop-distribute-patterns so the
 * compiler cannot synthesise new tk_memcpy/tk_memset calls behind our back. */
void *tk_memmove(void *dst, const void *src, size_t n)
{
    unsigned char       *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if (!n) return dst;
    if (d == s) return dst;
    if (d < s) {
        size_t i;
        for (i = 0; i < n; i++) d[i] = s[i];
    } else {
        size_t i;
        for (i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

void *tk_memcpy(void *dst, const void *src, size_t n)
{
    return tk_memmove(dst, src, n);
}

void *tk_memset(void *dst, int c, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    size_t i;

    for (i = 0; i < n; i++) d[i] = (unsigned char)c;
    return dst;
}

int tk_memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    size_t i;

    for (i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

size_t tk_strlen(const char *s)
{
    const char *p = s;

    while (*p) p++;
    return (size_t)(p - s);
}

int tk_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int tk_strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) { a++; b++; n--; }
    if (!n) return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

const char *tk_strpbrk(const char *s, const char *set)
{
    for (; *s; s++) {
        const char *p = set;
        for (; *p; p++) if (*p == *s) return s;
    }
    return NULL;
}

const char *tk_strstr(const char *h, const char *n)
{
    size_t nl;

    if (!n || !*n) return h;
    nl = tk_strlen(n);
    for (; *h; h++) {
        size_t i;
        if (*h != n[0]) continue;
        for (i = 1; i < nl; i++) if (h[i] != n[i]) break;
        if (i == nl) return h;
    }
    return NULL;
}
