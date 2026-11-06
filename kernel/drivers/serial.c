/* COM1 serial port. QEMU pipes it to your terminal (-serial stdio), so
   kernel logs survive even when the screen code is broken. */
#include "drivers/serial.h"
#include "arch/io.h"

#define COM1 0x3f8

void serial_init(void) {
    outb(COM1 + 1, 0x00);   /* no interrupts */
    outb(COM1 + 3, 0x80);   /* DLAB on to set the baud divisor */
    outb(COM1 + 0, 0x01);   /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8 data bits, no parity, 1 stop bit */
    outb(COM1 + 2, 0xc7);   /* FIFO on */
    outb(COM1 + 4, 0x03);
}

void serial_putc(char c) {
    if (c == '\n') serial_putc('\r');
    while (!(inb(COM1 + 5) & 0x20)) {}   /* wait for the transmit buffer */
    outb(COM1, c);
}
