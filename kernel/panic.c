/* When something goes badly wrong, stop everything and say so clearly. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/io.h"
#include "drivers/fb.h"
#include "drivers/serial.h"

static const char *names[32] = {
    "Divide error", "Debug", "NMI", "Breakpoint", "Overflow", "Bound range",
    "Invalid opcode", "Device not available", "Double fault", "Coprocessor overrun",
    "Invalid TSS", "Segment not present", "Stack fault", "General protection fault",
    "Page fault", "Reserved", "x87 FPU error", "Alignment check", "Machine check",
    "SIMD exception", "Virtualization", "Control protection",
};

#define RED   0xff5a6e
#define WHITE 0xf2f2f2
#define GREY  0x9a8a90

static int y;

static void line(uint32_t color, const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    gfx_text(fb_screen(), 48, y, buf, color, -1);
    for (char *p = buf; *p; p++) serial_putc(*p);
    serial_putc('\n');
    y += FONT_H + 4;
}

static void screen(const char *title) {
    cli();
    struct surface *s = fb_screen();
    gfx_clip(s, NULL);
    gfx_gradient(s, 0, 0, fb.width, fb.height, 0x2a0a12, 0x0c0306);
    gfx_fill(s, 0, 0, fb.width, 4, RED);
    y = 48;
    line(RED, "KERNEL PANIC");
    y += 8;
    line(WHITE, "%s", title);
    y += 12;
}

void panic(const char *msg) {
    screen(msg);
    line(GREY, "the system has been halted.");
    fb_present_all();
    halt_forever();
}

void panic_exception(struct regs *r) {
    const char *name = r->vector < 32 && names[r->vector] ? names[r->vector] : "Unknown exception";
    char title[96];
    snprintf(title, sizeof title, "%s  (vector %lu, error %lx)", name, r->vector, r->error);
    screen(title);

    if (r->vector == 14) line(WHITE, "faulting address  cr2 = %p", (void *)read_cr2());
    line(WHITE, "instruction       rip = %p", (void *)r->rip);
    y += 8;
    line(GREY, "rax %016lx  rbx %016lx  rcx %016lx", r->rax, r->rbx, r->rcx);
    line(GREY, "rdx %016lx  rsi %016lx  rdi %016lx", r->rdx, r->rsi, r->rdi);
    line(GREY, "rbp %016lx  rsp %016lx  r8  %016lx", r->rbp, r->rsp, r->r8);
    line(GREY, "r9  %016lx  r10 %016lx  r11 %016lx", r->r9, r->r10, r->r11);
    line(GREY, "r12 %016lx  r13 %016lx  r14 %016lx", r->r12, r->r13, r->r14);
    line(GREY, "r15 %016lx  rfl %016lx  cs  %04lx", r->r15, r->rflags, r->cs);
    y += 12;
    line(GREY, "tip: `addr2line -e build/dream.elf %p` names the line that crashed.", (void *)r->rip);
    fb_present_all();
    halt_forever();
}
