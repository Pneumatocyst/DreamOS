#pragma once
/* dream's tiny C library for user programs. Everything here ends up asking the
   kernel for help through a system call: `int 0x80` with a number in rax. */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>

/* system calls */
__attribute__((noreturn)) void exit(int code);
long write(int fd, const void *buf, size_t len);
long read(int fd, void *buf, size_t len);       /* fd 0: keys, or piped input (0 = end) */
void sleep_ms(unsigned long ms);
int  getpid(void);
unsigned long uptime_ms(void);
long readfile(const char *path, void *buf, size_t len);

/* the rest is plain C */
size_t strlen(const char *s);
int  strcmp(const char *a, const char *b);
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
int  atoi(const char *s);
void puts(const char *s);                       /* no newline added */
void putchar(char c);
int  printf(const char *fmt, ...);
int  getchar(void);                             /* -1 at end of input */
int  readline(char *buf, int size);             /* with echo and backspace */
unsigned rand(void);

/* colours, same escapes as the kernel's */
#define FG     "\0330"
#define DIM    "\0331"
#define ACCENT "\0332"
#define OK     "\0333"
#define WARN   "\0334"
#define ERR    "\0335"
