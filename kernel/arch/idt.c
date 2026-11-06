/* Interrupt Descriptor Table: tells the CPU which stub to run for each vector. */
#include <stdint.h>
#include "arch/cpu.h"
#include "arch/io.h"
#include "kernel.h"
#include "sched/task.h"
#include "proc/process.h"

struct __attribute__((packed)) idt_entry {
    uint16_t off_lo;
    uint16_t selector;   /* kernel code segment */
    uint8_t  ist;
    uint8_t  flags;      /* 0x8e = present, ring 0, interrupt gate */
    uint16_t off_mid;
    uint32_t off_hi;
    uint32_t zero;
};

static struct idt_entry idt[256];
static struct __attribute__((packed)) { uint16_t limit; uint64_t base; } idtr;
extern const uint64_t isr_table[48];
extern void isr128(void);
static irq_handler_t irq_handlers[16];

void idt_init(void) {
    for (int v = 0; v < 48; v++) {
        uint64_t a = isr_table[v];
        idt[v] = (struct idt_entry){
            .off_lo = a & 0xffff, .selector = 0x08, .ist = 0, .flags = 0x8e,
            .off_mid = (a >> 16) & 0xffff, .off_hi = a >> 32, .zero = 0,
        };
    }
    /* int 0x80: same kind of gate, but marked callable from ring 3 (0xee, not 0x8e) */
    uint64_t a = (uint64_t)isr128;
    idt[0x80] = (struct idt_entry){
        .off_lo = a & 0xffff, .selector = 0x08, .ist = 0, .flags = 0xee,
        .off_mid = (a >> 16) & 0xffff, .off_hi = a >> 32, .zero = 0,
    };
    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)idt;
    asm volatile("lidt %0" :: "m"(idtr));
}

void irq_register(int irq, irq_handler_t h) { irq_handlers[irq] = h; }

/* Called from isr_common with interrupts disabled. */
void isr_dispatch(struct regs *r) {
    bool from_user = (r->cs & 3) == 3;
    if (r->vector == 0x80) { syscall_dispatch(r); return; }
    if (r->vector < 32) {
        if (from_user) process_fault(r);   /* a program crashed: kill it, not us */
        panic_exception(r);          /* the kernel crashed: show the panic screen */
    }
    int irq = r->vector - 32;
    if (irq_handlers[irq]) irq_handlers[irq](r);
    if (irq >= 8) outb(0xa0, 0x20);  /* end-of-interrupt to the slave PIC... */
    outb(0x20, 0x20);                /* ...and the master */
    if (irq == 0) sched_tick();      /* the timer: maybe switch to another task */
    if (from_user) process_check_killed();   /* Ctrl+C'd while running? stop here */
}
