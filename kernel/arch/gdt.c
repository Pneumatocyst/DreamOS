/* Global Descriptor Table. In 64-bit mode segmentation is mostly vestigial,
   but the CPU still needs code/data descriptors and the privilege levels they carry.
   Limine gave us a GDT already; owning our own is step one toward user mode. */
#include <stdint.h>
#include "arch/cpu.h"

static uint64_t gdt[] = {
    0,                      /* 0x00 null */
    0x00af9a000000ffff,     /* 0x08 kernel code: present, ring 0, 64-bit */
    0x00cf92000000ffff,     /* 0x10 kernel data */
    0x00cff2000000ffff,     /* 0x18 user data   (ring 3) */
    0x00affa000000ffff,     /* 0x20 user code   (ring 3, 64-bit) */
    0, 0,                   /* 0x28 the TSS (a 16-byte entry, filled in below) */
};

/* The Task State Segment. In 64-bit mode it has one job we care about: rsp0,
   the kernel stack the CPU switches to when an interrupt or system call arrives
   while a user program (ring 3) is running. The scheduler updates it on every
   task switch, so each task lands on its own kernel stack. */
static struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3, iomap_base;
} tss;

void tss_set_rsp0(uint64_t rsp) { tss.rsp0 = rsp; }

struct __attribute__((packed)) { uint16_t limit; uint64_t base; } gdtr;

void gdt_init(void) {
    uint64_t base = (uint64_t)&tss, limit = sizeof tss - 1;
    tss.iomap_base = sizeof tss;          /* no I/O permission map: user code gets no ports */
    gdt[5] = (limit & 0xffff) | (base & 0xffffff) << 16 | 0x89ull << 40   /* present, 64-bit TSS */
           | ((limit >> 16) & 0xf) << 48 | ((base >> 24) & 0xff) << 56;
    gdt[6] = base >> 32;

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (uint64_t)gdt;
    asm volatile(
        "lgdt %0\n"
        /* reload CS with a far return: push selector + target, then lretq */
        "pushq $0x08\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        "movw $0x10, %%ax\n"
        "movw %%ax, %%ds\n"
        "movw %%ax, %%es\n"
        "movw %%ax, %%ss\n"
        "movw %%ax, %%fs\n"
        "movw %%ax, %%gs\n"
        "movw $0x28, %%ax\n"
        "ltr %%ax\n"                       /* load the TSS */
        :: "m"(gdtr) : "rax", "memory");
}
