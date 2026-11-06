#pragma once
#include <stdint.h>

/* Register snapshot pushed by isr.S on every interrupt — order matches the pushes. */
struct regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;   /* pushed by the CPU itself */
};

typedef void (*irq_handler_t)(struct regs *r);

void gdt_init(void);
void tss_set_rsp0(uint64_t rsp);
void idt_init(void);
void pic_init(void);
void pic_unmask(int irq);
void irq_register(int irq, irq_handler_t h);

void     pit_init(uint32_t hz);
uint64_t pit_ticks(void);
uint32_t pit_hz(void);
