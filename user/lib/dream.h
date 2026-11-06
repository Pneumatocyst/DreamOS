#pragma once
/* dream's tiny C library for user programs. Everything here ends up asking the
   kernel for help through a system call: `int 0x80` with a number in rax. */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>
#include <dream/abi.h>     /* system call numbers, keys, the window system's structures */

/* system calls */
__attribute__((noreturn)) void exit(int code);
long write(int fd, const void *buf, size_t len);
long read(int fd, void *buf, size_t len);       /* fd 0: keys, or piped input (0 = end) */
void sleep_ms(unsigned long ms);
int  getpid(void);
unsigned long uptime_ms(void);
long readfile(const char *path, void *buf, size_t len);
long writefile(const char *path, const void *buf, size_t len);   /* creates or replaces it */
long listdir(const char *path, char *buf, size_t len);   /* "name\n" per entry, folders end in / */
long fileinfo(const char *path);
int  stat(const char *path, struct file_stat *st);       /* 0, or an E_* error */
int  mkdir(const char *path);
int  create(const char *path);                           /* an empty file */
int  remove(const char *path);                           /* folders: with everything in them */
int  rename(const char *path, const char *new_name);     /* stays in the same folder */
const char *fs_error(int err);                           /* "no such file" etc. */
void sysinfo(struct sysinfo *si);
int  tasks(struct task_entry *out, int max);
int  kill(int task_id);                                  /* 0, -1 no such task, -2 kernel task */
bool config_get(const char *key, char out[64]);          /* settings in /etc/dream.conf */
void config_set(const char *key, const char *value);
int  theme_get(int index, struct theme_info *out);       /* -1 = current; returns how many themes */
int  theme_set(const char *name);
int  wallpaper_set(const char *name);
int  wallpaper_name(int index, char out[16]);            /* returns how many wallpapers */
bool passwd_isset(void);
void passwd_set(const char *pw);                         /* "" removes it */
int  spawn(const char *program, const char *arg);        /* start a program in the background */
int  terminal_run(const char *command);                  /* type it into the terminal; -1 if busy */
long clip_get(char *buf, size_t len);                    /* the clipboard's text: bytes copied */
void clip_set(const char *text, size_t len);                         /* size, FILE_IS_DIR, or -1 if missing */

/* the rest is plain C */
size_t strlen(const char *s);
int  strcmp(const char *a, const char *b);
int  strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
size_t strlcpy(char *d, const char *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int  memcmp(const void *a, const void *b, size_t n);
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
int  atoi(const char *s);
void puts(const char *s);                       /* no newline added */
void putchar(char c);
int  printf(const char *fmt, ...);
int  snprintf(char *buf, size_t n, const char *fmt, ...);
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
