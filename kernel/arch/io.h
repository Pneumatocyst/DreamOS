#pragma once
#include <stdint.h>

/* x86 port I/O: the old-school way to talk to the PIC, PIT, keyboard, serial. */
static inline void outb(uint16_t port, uint8_t v)  { asm volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline void outw(uint16_t port, uint16_t v) { asm volatile("outw %0, %1" :: "a"(v), "Nd"(port)); }
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    asm volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    asm volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
/* read `count` 16-bit words from a port into memory, in one instruction */
static inline void insw(uint16_t port, void *buf, uint32_t count) {
    asm volatile("rep insw" : "+D"(buf), "+c"(count) : "d"(port) : "memory");
}
static inline void io_wait(void) { outb(0x80, 0); }

static inline void cli(void) { asm volatile("cli"); }
static inline void sti(void) { asm volatile("sti"); }
static inline void hlt(void) { asm volatile("hlt"); }

/* The simplest lock on a single CPU: switch interrupts off, so no task switch
   (and no interrupt handler) can cut in. Keep these sections short! */
static inline uint64_t irq_save(void) {
    uint64_t flags;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void irq_restore(uint64_t flags) {
    if (flags & 0x200) asm volatile("sti" ::: "memory");   /* only if they were on before */
}

static inline uint64_t read_cr0(void) { uint64_t v; asm volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline void write_cr0(uint64_t v) { asm volatile("mov %0, %%cr0" :: "r"(v) : "memory"); }
static inline uint64_t read_cr3(void) { uint64_t v; asm volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline void write_cr3(uint64_t v) { asm volatile("mov %0, %%cr3" :: "r"(v) : "memory"); }
static inline uint64_t read_cr4(void) { uint64_t v; asm volatile("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void write_cr4(uint64_t v) { asm volatile("mov %0, %%cr4" :: "r"(v) : "memory"); }

static inline uint64_t read_cr2(void) {
    uint64_t v;
    asm volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

__attribute__((noreturn)) static inline void halt_forever(void) {
    for (;;) asm volatile("cli; hlt");
}
