#include "dream.h"

/* ---- system calls ---------------------------------------------------------------- */

static long syscall3(long n, long a, long b, long c) {
    long ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
    return ret;
}

void exit(int code)                         { syscall3(0, code, 0, 0); for (;;) {} }
long write(int fd, const void *b, size_t n) { return syscall3(1, fd, (long)b, n); }
long read(int fd, void *b, size_t n)        { return syscall3(2, fd, (long)b, n); }
void sleep_ms(unsigned long ms)             { syscall3(3, ms, 0, 0); }
int  getpid(void)                           { return syscall3(4, 0, 0, 0); }
unsigned long uptime_ms(void)               { return syscall3(5, 0, 0, 0); }
long readfile(const char *p, void *b, size_t n) { return syscall3(SYS_READFILE, (long)p, (long)b, n); }
long writefile(const char *p, const void *b, size_t n) { return syscall3(SYS_WRITEFILE, (long)p, (long)b, n); }
long listdir(const char *p, char *b, size_t n) { return syscall3(SYS_LISTDIR, (long)p, (long)b, n); }
int  stat(const char *p, struct file_stat *st) { return (int)syscall3(SYS_STAT, (long)p, (long)st, 0); }
int  mkdir(const char *p)  { return (int)syscall3(SYS_FSOP, FSOP_MKDIR, (long)p, 0); }
int  create(const char *p) { return (int)syscall3(SYS_FSOP, FSOP_CREATE, (long)p, 0); }
int  remove(const char *p) { return (int)syscall3(SYS_FSOP, FSOP_REMOVE, (long)p, 0); }
int  rename(const char *p, const char *n) { return (int)syscall3(SYS_FSOP, FSOP_RENAME, (long)p, (long)n); }
const char *fs_error(int e) {
    switch (e) {
    case E_NOENT: return "no such file or folder";   case E_EXIST: return "something with that name is already there";
    case E_NOTDIR: return "not a folder";             case E_ISDIR: return "that's a folder";
    case E_NOSPC: return "out of space";              case E_NOTEMPTY: return "the folder isn't empty";
    case E_INVAL: return "that's not allowed";        case E_FBIG: return "too big";
    case E_BUSY: return "the terminal is in there";
    }
    return "something went wrong";
}
void sysinfo(struct sysinfo *si) { syscall3(SYS_SYSINFO, (long)si, 0, 0); }
int  tasks(struct task_entry *out, int max) { return (int)syscall3(SYS_TASKS, (long)out, max, 0); }
int  kill(int id) { return (int)syscall3(SYS_KILL, id, 0, 0); }
bool config_get(const char *k, char out[64]) { return syscall3(SYS_CONFIG, CONFIG_GET, (long)k, (long)out) == 0; }
void config_set(const char *k, const char *v) { syscall3(SYS_CONFIG, CONFIG_SET, (long)k, (long)v); }
int  theme_get(int i, struct theme_info *out) { return (int)syscall3(SYS_THEME, THEME_GET, i, (long)out); }
int  theme_set(const char *n) { return (int)syscall3(SYS_THEME, THEME_SET, (long)n, 0); }
int  wallpaper_set(const char *n) { return (int)syscall3(SYS_THEME, WALLPAPER_SET, (long)n, 0); }
int  wallpaper_name(int i, char out[16]) { return (int)syscall3(SYS_THEME, WALLPAPER_NAME, i, (long)out); }
bool passwd_isset(void) { return syscall3(SYS_PASSWD, PW_ISSET, 0, 0) == 1; }
void passwd_set(const char *pw) { syscall3(SYS_PASSWD, pw[0] ? PW_SET : PW_CLEAR, (long)pw, 0); }
int  spawn(const char *prog, const char *arg) { return (int)syscall3(SYS_SPAWN, (long)prog, (long)arg, 0); }
int  terminal_run(const char *cmd) { return (int)syscall3(SYS_TERMINAL, (long)cmd, 0, 0); }
long clip_get(char *b, size_t n) { return syscall3(SYS_CLIPBOARD, CLIP_GET, (long)b, n); }
void clip_set(const char *t, size_t n) { syscall3(SYS_CLIPBOARD, CLIP_SET, (long)t, n); }
long fileinfo(const char *p) { return syscall3(SYS_FILEINFO, (long)p, 0, 0); }

/* ---- strings ----------------------------------------------------------------------- */

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) { if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b; }
    return 0;
}
char *strchr(const char *s, int c) { for (; *s; s++) if (*s == (char)c) return (char *)s; return c ? NULL : (char *)s; }
char *strrchr(const char *s, int c) { const char *r = NULL; for (; *s; s++) if (*s == (char)c) r = s; return c ? (char *)r : (char *)s; }
size_t strlcpy(char *d, const char *s, size_t n) {
    size_t l = strlen(s);
    if (n) { size_t m = l < n - 1 ? l : n - 1; memcpy(d, s, m); d[m] = 0; }
    return l;
}
void *memmove(void *d, const void *s, size_t n) {
    unsigned char *p = d; const unsigned char *q = s;
    if (p < q) while (n--) *p++ = *q++;
    else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++) if (*p != *q) return *p - *q;
    return 0;
}
void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
int atoi(const char *s) {
    int v = 0, neg = *s == '-';
    if (neg) s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

/* ---- output: buffered, so one write() call per line instead of per letter ---------- */

static char obuf[512];
static size_t olen;

static void flush(void) { if (olen) write(1, obuf, olen); olen = 0; }

void putchar(char c) {
    obuf[olen++] = c;
    if (c == '\n' || olen == sizeof obuf) flush();
}

void puts(const char *s) { while (*s) putchar(*s++); flush(); }

/* where formatted text goes: the screen, or a buffer (for snprintf) */
static char *sbuf;
static size_t slen, scap;
static void emit(char c) {
    if (!sbuf) { putchar(c); return; }
    if (slen + 1 < scap) sbuf[slen] = c;
    slen++;
}

static void put_num(unsigned long v, int base, int width, char pad) {
    char tmp[24];
    int i = 0;
    do { tmp[i++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
    while (width-- > i) emit(pad);
    while (i) emit(tmp[--i]);
}

static void format(const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit(*fmt); continue; }
        fmt++;
        char pad = ' ';
        int width = 0;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int prec = 6;
        if (*fmt == '.') { prec = 0; fmt++; while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0'); }
        if (*fmt == 'l') fmt++;
        switch (*fmt) {
        case 'd': { long v = va_arg(ap, long); if (v < 0) { emit('-'); v = -v; width--; } put_num(v, 10, width, pad); break; }
        case 'u': put_num(va_arg(ap, unsigned long), 10, width, pad); break;
        case 'x': put_num(va_arg(ap, unsigned long), 16, width, pad); break;
        case 'p': emit('0'); emit('x'); put_num(va_arg(ap, unsigned long), 16, 16, '0'); break;
        case 's': { const char *s = va_arg(ap, const char *); int n = strlen(s); while (*s) emit(*s++); while (width-- > n) emit(' '); break; }
        case 'c': emit((char)va_arg(ap, int)); break;
        case 'f': {                      /* a double: whole part, dot, then prec digits */
            double v = va_arg(ap, double);
            if (v < 0) { emit('-'); v = -v; }
            double round = 0.5;
            for (int i = 0; i < prec; i++) round /= 10;
            v += round;
            unsigned long whole = (unsigned long)v;
            put_num(whole, 10, 0, ' ');
            if (prec) emit('.');
            v -= (double)whole;
            for (int i = 0; i < prec; i++) { v *= 10; int d = (int)v; emit('0' + d); v -= d; }
            break;
        }
        case '%': emit('%'); break;
        }
    }
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    format(fmt, ap);
    va_end(ap);
    flush();
    return 0;
}

/* Like printf, but into buf (at most n bytes, always ending in 0). */
int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sbuf = buf; scap = n; slen = 0;
    format(fmt, ap);
    if (n) buf[slen < n ? slen : n - 1] = 0;
    sbuf = NULL;
    va_end(ap);
    return (int)slen;
}



/* ---- input ---------------------------------------------------------------------------- */

int getchar(void) {
    char c;
    return read(0, &c, 1) == 1 ? (unsigned char)c : -1;
}

int readline(char *buf, int size) {
    int n = 0;
    for (;;) {
        int c = getchar();
        if (c < 0) { if (!n) return -1; break; }
        if (c == '\n') { putchar('\n'); flush(); break; }
        if (c == '\b') { if (n) { n--; puts("\b"); } continue; }
        if (c >= 32 && c < 127 && n < size - 1) { buf[n++] = c; putchar(c); flush(); }
    }
    buf[n] = 0;
    return n;
}

static unsigned seed;
unsigned rand(void) {
    if (!seed) seed = uptime_ms() * 2654435761u | 1;
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}
