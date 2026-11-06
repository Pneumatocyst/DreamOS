/* The PIT ticks at 1.193182 MHz; we divide it down into a steady heartbeat. */
#include "arch/cpu.h"
#include "arch/io.h"

static volatile uint64_t ticks;
static uint32_t hz;

static void on_tick(struct regs *r) { (void)r; ticks++; }

void pit_init(uint32_t freq) {
    hz = freq;
    uint32_t div = 1193182 / freq;
    outb(0x43, 0x36);              /* channel 0, lo/hi byte, square wave */
    outb(0x40, div & 0xff);
    outb(0x40, div >> 8);
    irq_register(0, on_tick);
    pic_unmask(0);
}

uint64_t pit_ticks(void) { return ticks; }
uint32_t pit_hz(void)    { return hz; }
