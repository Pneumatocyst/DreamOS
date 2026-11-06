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
long readfile(const char *p, void *b, size_t n) { return syscall3(6, (long)p, (long)b, n); }

/* ---- strings ----------------------------------------------------------------------- */

size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return (unsigned char)*a - (unsigned char)*b; }
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

static void put_num(unsigned long v, int base, int width, char pad) {
    char tmp[24];
    int i = 0;
    do { tmp[i++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
    while (width-- > i) putchar(pad);
    while (i) putchar(tmp[--i]);
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { putchar(*fmt); continue; }
        fmt++;
        char pad = ' ';
        int width = 0;
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        int prec = 6;
        if (*fmt == '.') { prec = 0; fmt++; while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0'); }
        if (*fmt == 'l') fmt++;
        switch (*fmt) {
        case 'd': { long v = va_arg(ap, long); if (v < 0) { putchar('-'); v = -v; width--; } put_num(v, 10, width, pad); break; }
        case 'u': put_num(va_arg(ap, unsigned long), 10, width, pad); break;
        case 'x': put_num(va_arg(ap, unsigned long), 16, width, pad); break;
        case 'p': putchar('0'); putchar('x'); put_num(va_arg(ap, unsigned long), 16, 16, '0'); break;
        case 's': { const char *s = va_arg(ap, const char *); int n = strlen(s); while (*s) putchar(*s++); while (width-- > n) putchar(' '); break; }
        case 'c': putchar((char)va_arg(ap, int)); break;
        case 'f': {                      /* a double: whole part, dot, then prec digits */
            double v = va_arg(ap, double);
            if (v < 0) { putchar('-'); v = -v; }
            double round = 0.5;
            for (int i = 0; i < prec; i++) round /= 10;
            v += round;
            unsigned long whole = (unsigned long)v;
            put_num(whole, 10, 0, ' ');
            if (prec) putchar('.');
            v -= (double)whole;
            for (int i = 0; i < prec; i++) { v *= 10; int d = (int)v; putchar('0' + d); v -= d; }
            break;
        }
        case '%': putchar('%'); break;
        }
    }
    va_end(ap);
    flush();
    return 0;
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
