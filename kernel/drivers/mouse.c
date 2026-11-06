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
 *
 * In a virtual machine there's a better way: the "vmmouse" (VMware's protocol, which
 * QEMU and VirtualBox also speak). It reports where the pointer is in the VM's window
 * (absolute), so the host's mouse just slides into the window and works: nothing has
 * to be "captured". It still rides on the PS/2 mouse's interrupt; see vmmouse_* below.
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

/* ---- vmmouse ---------------------------------------------------------------------------
 * A "backdoor": put a magic number in eax and a command in ecx, then read from I/O
 * port 0x5658. On real hardware nothing is there and nothing happens; in a VM, the
 * hypervisor catches the read and answers in the registers. */

#define VM_MAGIC        0x564d5868
#define VM_PORT         0x5658
#define VM_GETVERSION   10
#define VM_ABS_DATA     39
#define VM_ABS_STATUS   40
#define VM_ABS_COMMAND  41
#define VM_CMD_ENABLE   0x45414552
#define VM_CMD_ABSOLUTE 0x53424152
#define VM_VERSION_ID   0x3442554a
#define VM_ERROR        0xffff0000
#define VM_LEFT 0x20
#define VM_RIGHT 0x10
#define VM_MIDDLE 0x08

static bool vmmouse;
static uint8_t vm_buttons;

struct vmregs { uint32_t a, b, c, d; };

static struct vmregs backdoor(uint32_t cmd, uint32_t arg) {
    struct vmregs r;
    asm volatile("inl %%dx, %%eax"
                 : "=a"(r.a), "=b"(r.b), "=c"(r.c), "=d"(r.d)
                 : "a"(VM_MAGIC), "b"(arg), "c"(cmd), "d"(VM_PORT)
                 : "rsi", "rdi", "memory");
    return r;
}

static bool vmmouse_init(void) {
    if (backdoor(VM_GETVERSION, ~VM_MAGIC).b != VM_MAGIC) return false;   /* not in a VM */
    backdoor(VM_ABS_COMMAND, VM_CMD_ENABLE);
    uint32_t st = backdoor(VM_ABS_STATUS, 0).a;
    if ((st & VM_ERROR) == VM_ERROR || (st & 0xffff) == 0) return false;
    if (backdoor(VM_ABS_DATA, 1).a != VM_VERSION_ID) return false;      /* answers with its ID */
    backdoor(VM_ABS_COMMAND, VM_CMD_ABSOLUTE);
    return true;
}

static void push(int x, int y, int dx, int dy, int dz, uint8_t buttons);

/* Read whatever the vmmouse has queued (4 words per event). false = nothing there. */
static bool vmmouse_drain(void) {
    bool any = false;
    for (;;) {
        uint32_t st = backdoor(VM_ABS_STATUS, 0).a;
        if ((st & VM_ERROR) == VM_ERROR) { vmmouse = vmmouse_init(); return any; }
        if ((st & 0xffff) < 4) return any;
        struct vmregs d = backdoor(VM_ABS_DATA, 4);
        /* x and y come as 0..65535 across the whole window */
        int x = (int)((uint64_t)(d.b & 0xffff) * fb.width / 65536);
        int y = (int)((uint64_t)(d.c & 0xffff) * fb.height / 65536);
        vm_buttons = (d.a & VM_LEFT ? MOUSE_LEFT : 0) | (d.a & VM_RIGHT ? MOUSE_RIGHT : 0) |
                     (d.a & VM_MIDDLE ? MOUSE_MIDDLE : 0);
        push(x, y, x - mx, y - my, (int8_t)d.d, vm_buttons);
        any = true;
    }
}

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

    /* With the vmmouse on, QEMU sends a dummy PS/2 packet just to raise this
       interrupt; the real news is waiting in the vmmouse queue. */
    if (vmmouse && vmmouse_drain()) return;

    if (pkt[0] & 0xc0) return;                        /* overflowed: junk */
    int dx = (int)pkt[1] - ((pkt[0] << 4) & 0x100);    /* 9-bit signed values */
    int dy = (int)pkt[2] - ((pkt[0] << 3) & 0x100);
    int dz = wheel ? (int8_t)(pkt[3] << 4) >> 4 : 0;   /* low 4 bits, signed */

    push(mx + dx, my - dy, dx, -dy, dz, pkt[0] & 7);  /* mouse up = screen up */
}

static void push(int x, int y, int dx, int dy, int dz, uint8_t buttons) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= (int)fb.width)  x = fb.width - 1;
    if (y >= (int)fb.height) y = fb.height - 1;
    mx = x;
    my = y;
    unsigned next = (head + 1) % 64;
    if (next == tail) return;                           /* nobody's reading: drop it */
    queue[head] = (struct mouse_event){ x, y, dx, dy, dz, buttons };
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
    vmmouse = vmmouse_init();
    mx = fb.width / 2;
    my = fb.height / 2;
    irq_register(12, on_irq);
    pic_unmask(12);
}

bool mouse_present(void)   { return present; }
bool mouse_has_wheel(void) { return wheel; }
bool mouse_absolute(void)  { return vmmouse; }
void mouse_where(int *x, int *y) { *x = mx; *y = my; }

bool mouse_poll(struct mouse_event *e) {
    if (head == tail) return false;
    *e = queue[tail];
    tail = (tail + 1) % 64;
    return true;
}
