/* A small printf. Enough for kernel logs: %s %c %d %u %x %p %%,
   the 'l' length modifier, a field width, and the '0' and '-' flags. */
#include "kernel.h"
#include "drivers/console.h"
#include "drivers/serial.h"
#include "arch/cpu.h"

struct out { char *buf; size_t n, len; };

static void emit(struct out *o, char c) {
    if (o->len + 1 < o->n) o->buf[o->len] = c;
    o->len++;
}

static void emit_num(struct out *o, uint64_t v, int base, bool neg,
                     int width, bool zero, bool left) {
    char tmp[24];
    int i = 0;
    do { tmp[i++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
    if (neg) {
        if (zero) { emit(o, '-'); neg = false; }
        width--;
    }
    int pad = width - i;
    if (!left) while (pad-- > 0) emit(o, zero ? '0' : ' ');
    if (neg) emit(o, '-');
    while (i) emit(o, tmp[--i]);
    if (left) while (pad-- > 0) emit(o, ' ');
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap) {
    struct out o = { buf, n, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit(&o, *fmt); continue; }
        fmt++;
        bool zero = false, left = false, lng = false;
        int width = 0;
        for (;; fmt++) {
            if (*fmt == '0') zero = true;
            else if (*fmt == '-') left = true;
            else break;
        }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l') { lng = true; fmt++; }

        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int pad = width - (int)strlen(s);
            if (!left) while (pad-- > 0) emit(&o, ' ');
            while (*s) emit(&o, *s++);
            if (left) while (pad-- > 0) emit(&o, ' ');
            break;
        }
        case 'c': emit(&o, (char)va_arg(ap, int)); break;
        case 'd': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            emit_num(&o, v < 0 ? -(uint64_t)v : (uint64_t)v, 10, v < 0, width, zero, left);
            break;
        }
        case 'u': emit_num(&o, lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 10, false, width, zero, left); break;
        case 'x': emit_num(&o, lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16, false, width, zero, left); break;
        case 'p':
            emit(&o, '0'); emit(&o, 'x');
            emit_num(&o, (uint64_t)va_arg(ap, void *), 16, false, 16, true, false);
            break;
        case '%': emit(&o, '%'); break;
        case '\0': fmt--; break;
        default: emit(&o, '%'); emit(&o, *fmt); break;
        }
    }
    if (n) buf[o.len < n ? o.len : n - 1] = '\0';
    return (int)o.len;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

/* ---- output: where does printed text go? ------------------------------------- */

static void (*out_hook)(const char *, size_t);
unsigned kerror_count;

void kout_redirect(void (*hook)(const char *, size_t)) { out_hook = hook; }
void (*kout_current(void))(const char *, size_t) { return out_hook; }

static void to_screen(const char *s, size_t n) {
    static bool esc;
    for (size_t i = 0; i < n; i++) {
        con_putc(s[i]);
        /* the serial log gets the same text, minus our colour escapes */
        if (esc) { esc = false; continue; }
        if (s[i] == '\033') { esc = true; continue; }
        serial_putc(s[i] == BLOCK[0] ? '#' : s[i]);
    }
    con_flush();
}

void kwrite(const char *s, size_t n) {
    if (out_hook) out_hook(s, n);   /* the shell is capturing: a pipe or a > file */
    else to_screen(s, n);
}

void kprintf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    kwrite(buf, n < (int)sizeof buf ? (size_t)n : sizeof buf - 1);
}

void kerrorf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    kerror_count++;
    to_screen(buf, n < (int)sizeof buf ? (size_t)n : sizeof buf - 1);
}

/* ---- the kernel log ------------------------------------------------------------- */

static char logbuf[8192];
static size_t loglen;

void klog(const char *msg) {
    uint32_t hz = pit_hz() ? pit_hz() : 100;
    uint64_t t = pit_ticks();
    char line[192];
    int n = snprintf(line, sizeof line, "[%5lu.%02lu] %s\n", t / hz, (t % hz) * 100 / hz, msg);
    if (n > (int)sizeof line - 1) n = sizeof line - 1;
    if (loglen + n <= sizeof logbuf) { memcpy(logbuf + loglen, line, n); loglen += n; }
}

const char *klog_buf(size_t *len) { *len = loglen; return logbuf; }
