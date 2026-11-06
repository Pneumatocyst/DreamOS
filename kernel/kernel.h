#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>

#define DREAM_VERSION "0.1"

struct regs;

/* lib/string.c */
void  *memset(void *d, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);
char  *strrchr(const char *s, int c);
size_t strlcpy(char *d, const char *s, size_t n);

/* lib/printf.c — %s %c %d %u %x %p, with l, width, 0 and - flags */
int  vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int  snprintf(char *buf, size_t n, const char *fmt, ...);
void kprintf(const char *fmt, ...);   /* "stdout": screen + serial, or wherever the shell redirects it */
void kwrite(const char *s, size_t n); /* same, unformatted */
void kerrorf(const char *fmt, ...);   /* "stderr": always the screen, even inside a pipe */
extern unsigned kerror_count;         /* bumped by kerrorf; the shell uses it for && */
void kout_redirect(void (*hook)(const char *s, size_t n));   /* NULL = back to screen */

/* kernel log, read back with dmesg */
void klog(const char *msg);
const char *klog_buf(size_t *len);

/* Colour escapes for kprintf: ESC + palette slot (see drivers/theme.h) */
#define C_FG     "\0330"
#define C_DIM    "\0331"
#define C_ACCENT "\0332"
#define C_OK     "\0333"
#define C_WARN   "\0334"
#define C_ERR    "\0335"
#define BLOCK    "\002"               /* a solid colour cell, for swatches */

/* main.c */
uint64_t mem_usable_bytes(void);
void     mem_print_map(void);

/* panic.c */
__attribute__((noreturn)) void panic(const char *msg);
__attribute__((noreturn)) void panic_exception(struct regs *r);

/* shell/shell.c */
__attribute__((noreturn)) void shell_run(void);
