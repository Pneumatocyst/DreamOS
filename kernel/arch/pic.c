/* The 8259 PIC routes hardware IRQs to the CPU. By default it maps them onto
   vectors 8-15, which collide with CPU exceptions, so we remap to 32-47. */
#include "arch/cpu.h"
#include "arch/io.h"

#define PIC1_CMD 0x20
#define PIC1_DAT 0x21
#define PIC2_CMD 0xa0
#define PIC2_DAT 0xa1

void pic_init(void) {
    outb(PIC1_CMD, 0x11); io_wait();   /* begin init, expect ICW4 */
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DAT, 32);   io_wait();   /* master offset -> vector 32 */
    outb(PIC2_DAT, 40);   io_wait();   /* slave offset  -> vector 40 */
    outb(PIC1_DAT, 4);    io_wait();   /* slave sits on IRQ2 */
    outb(PIC2_DAT, 2);    io_wait();
    outb(PIC1_DAT, 0x01); io_wait();   /* 8086 mode */
    outb(PIC2_DAT, 0x01); io_wait();
    outb(PIC1_DAT, 0xfb);              /* mask everything except the cascade */
    outb(PIC2_DAT, 0xff);
}

void pic_unmask(int irq) {
    uint16_t port = irq < 8 ? PIC1_DAT : PIC2_DAT;
    outb(port, inb(port) & ~(1 << (irq & 7)));
}
