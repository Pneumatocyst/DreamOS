/* PS/2 mouse.
 *
 * The mouse hangs off the same controller chip as the keyboard (the "8042"),
 * on its second ("auxiliary") port. Setting it up:
 *   1. turn on the aux port, and tell the controller to raise IRQ 12 for it
 *   2. send the mouse a couple of commands (each prefixed with 0xd4, meaning
 *      "this byte is for the mouse, not the keyboard"); it answers 0xfa (ACK)
 *   3. it then streams packets every time it moves:
 *
 *        byte 0:  Y-overflow X-overflow Y-sign X-sign  1  middle right left
 *        byte 1:  X movement (low 8 bits; the sign bit is in byte 0)
 *        byte 2:  Y movement (positive = up, the opposite of the screen!)
 *        byte 3:  scroll wheel, only if we unlocked it (see below)
 */
#include "drivers/mouse.h"
#include "drivers/fb.h"
#include "arch/cpu.h"
#include "arch/io.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

static bool present, wheel;
static volatile int mx, my;

static struct mouse_event queue[64];
static volatile unsigned head, tail;

/* ---- talking to the controller --------------------------------------------------- */

static bool can_write(void) {
    for (int i = 0; i < 100000; i++) if (!(inb(PS2_STATUS) & 2)) return true;
    return false;
}

static bool can_read(void) {
    for (int i = 0; i < 100000; i++) if (inb(PS2_STATUS) & 1) return true;
    return false;
}

static void controller(uint8_t cmd) { can_write(); outb(PS2_CMD, cmd); }
static int  read_byte(void)         { return can_read() ? inb(PS2_DATA) : -1; }

static bool send(uint8_t b) {       /* a byte for the mouse; true if it ACKed */
    controller(0xd4);
    can_write();
    outb(PS2_DATA, b);
    return read_byte() == 0xfa;
}

static bool set_rate(uint8_t r) { return send(0xf3) && send(r); }

/* ---- packets ---------------------------------------------------------------------- */

static uint8_t pkt[4];
static int idx;

static void on_irq(struct regs *r) {
    (void)r;
    if (!(inb(PS2_STATUS) & 1)) return;
    uint8_t b = inb(PS2_DATA);

    /* Byte 0 always has bit 3 set. If it doesn't, we've lost our place in the
       stream: drop bytes until we're lined up again. */
    if (idx == 0 && !(b & 0x08)) return;
    pkt[idx++] = b;
    if (idx < (wheel ? 4 : 3)) return;
    idx = 0;

    if (pkt[0] & 0xc0) return;                        /* overflowed: junk */
    int dx = (int)pkt[1] - ((pkt[0] << 4) & 0x100);    /* 9-bit signed values */
    int dy = (int)pkt[2] - ((pkt[0] << 3) & 0x100);
    int dz = wheel ? (int8_t)(pkt[3] << 4) >> 4 : 0;   /* low 4 bits, signed */

    int x = mx + dx, y = my - dy;                       /* mouse up = screen up */
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= (int)fb.width)  x = fb.width - 1;
    if (y >= (int)fb.height) y = fb.height - 1;
    mx = x;
    my = y;

    unsigned next = (head + 1) % 64;
    if (next == tail) return;                           /* nobody's reading: drop it */
    queue[head] = (struct mouse_event){ x, y, dx, -dy, dz, pkt[0] & 7 };
    head = next;
}

/* ---- setup ------------------------------------------------------------------------- */

void mouse_init(void) {
    controller(0xa8);                       /* enable the aux port */
    controller(0x20);                       /* read the controller's config byte... */
    int cfg = read_byte();
    if (cfg < 0) return;
    cfg |= 0x02;                            /* ...IRQ 12 on */
    cfg &= ~0x20;                           /* ...aux clock on */
    controller(0x60);
    can_write();
    outb(PS2_DATA, cfg);

    if (!send(0xf6)) return;                /* defaults; no ACK means no mouse */

    /* The scroll wheel is hidden behind a secret knock: set the sample rate to
       200, 100, 80, then ask for the device ID. A wheel mouse answers 3. */
    set_rate(200); set_rate(100); set_rate(80);
    if (send(0xf2)) wheel = read_byte() == 3;
    set_rate(100);

    if (!send(0xf4)) return;                /* start streaming packets */
    present = true;
    mx = fb.width / 2;
    my = fb.height / 2;
    irq_register(12, on_irq);
    pic_unmask(12);
}

bool mouse_present(void)   { return present; }
bool mouse_has_wheel(void) { return wheel; }
void mouse_where(int *x, int *y) { *x = mx; *y = my; }

bool mouse_poll(struct mouse_event *e) {
    if (head == tail) return false;
    *e = queue[tail];
    tail = (tail + 1) % 64;
    return true;
}
