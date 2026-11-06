/* No libc in a kernel — GCC still emits calls to memset/memcpy/memmove/memcmp,
   and the rest are just handy. */
#include "kernel.h"

/* These use the CPU's "repeat" string instructions: one instruction copies or
   fills a whole block, 8 bytes at a time. Screen-sized copies need the speed. */
void *memset(void *d, int c, size_t n) {
    void *p = d;
    asm volatile("rep stosb" : "+D"(p), "+c"(n) : "a"(c) : "memory");
    return d;
}

void *memcpy(void *d, const void *s, size_t n) {
    void *dp = d;
    size_t q = n / 8, r = n % 8;
    asm volatile("rep movsq" : "+D"(dp), "+S"(s), "+c"(q) :: "memory");
    asm volatile("rep movsb" : "+D"(dp), "+S"(s), "+c"(r) :: "memory");
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    if ((uintptr_t)d <= (uintptr_t)s || (uintptr_t)d >= (uintptr_t)s + n) return memcpy(d, s, n);
    /* overlapping, destination after source: copy backwards */
    uint8_t *dp = (uint8_t *)d + n - 1;
    const uint8_t *sp = (const uint8_t *)s + n - 1;
    asm volatile("std; rep movsb; cld" : "+D"(dp), "+S"(sp), "+c"(n) :: "memory");
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (; n--; x++, y++) if (*x != *y) return *x - *y;
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (; n && *a && *a == *b; n--) a++, b++;
    return n ? (uint8_t)*a - (uint8_t)*b : 0;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (!*s) return (char *)last;
    }
}

/* Copy with truncation; always NUL-terminates. Returns strlen(src). */
size_t strlcpy(char *d, const char *s, size_t n) {
    size_t len = strlen(s);
    if (n) {
        size_t c = len < n - 1 ? len : n - 1;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return len;
}
