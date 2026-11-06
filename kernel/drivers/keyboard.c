/* PS/2 keyboard. Each keypress fires IRQ1; we read a "scancode" from port 0x60,
   turn it into a character and drop it into a ring buffer the shell reads from. */
#include "drivers/keyboard.h"
#include "arch/cpu.h"
#include "arch/io.h"
#include "kernel.h"

/* scancode set 1 (what QEMU's controller sends by default) -> ASCII */
static const char map[128] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ',
};
static const char map_shift[128] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ',
};

static volatile uint8_t ring[128];
static volatile unsigned head, tail;
static bool shift, caps, ctrl, alt, extended;

bool kbd_alt(void) { return alt; }

static void push(int k) {
    unsigned next = (head + 1) % sizeof ring;
    if (next != tail) { ring[head] = (uint8_t)k; head = next; }
}

static void on_key(struct regs *r) {
    (void)r;
    uint8_t sc = inb(0x60);
    if (sc == 0xe0) { extended = true; return; }
    bool released = sc & 0x80;
    sc &= 0x7f;

    if (extended) {
        extended = false;
        if (sc == 0x1d) { ctrl = !released; return; }   /* right ctrl */
        if (sc == 0x38) { alt = !released; return; }    /* right alt (alt gr) */
        if (released) return;
        switch (sc) {
        case 0x48: push(KEY_UP);    break;
        case 0x50: push(KEY_DOWN);  break;
        case 0x4b: push(KEY_LEFT);  break;
        case 0x4d: push(KEY_RIGHT); break;
        case 0x47: push(KEY_HOME);  break;
        case 0x4f: push(KEY_END);   break;
        case 0x49: push(KEY_PGUP);  break;
        case 0x51: push(KEY_PGDN);  break;
        case 0x53: push(KEY_DEL);   break;
        case 0x5b: case 0x5c: push(KEY_SUPER); break;   /* left/right "windows" key */
        }
        return;
    }
    switch (sc) {
    case 0x2a: case 0x36: shift = !released; return;
    case 0x1d:            ctrl  = !released; return;
    case 0x38:            alt   = !released; return;
    case 0x3a:            if (!released) caps = !caps; return;
    }
    if (released) return;

    if (sc >= 0x3b && sc <= 0x44) {                        /* F1..F10 */
        if (alt && sc == 0x3e) { push(KEY_ALTF4); return; }
        push(KEY_F1 + sc - 0x3b);
        return;
    }
    if (sc == 0x57 || sc == 0x58) { push(KEY_F1 + 10 + (sc - 0x57)); return; }   /* F11, F12 */
    if (alt && sc == 0x0f) { push(KEY_ALTTAB); return; }

    char c = shift ? map_shift[sc] : map[sc];
    if (!c) return;
    if (caps && c >= 'a' && c <= 'z' && !shift) c -= 32;
    else if (caps && c >= 'A' && c <= 'Z' && shift) c += 32;
    if (ctrl && (c | 32) >= 'a' && (c | 32) <= 'z') c = (c | 32) - 'a' + 1;   /* ^A..^Z */
    push(c);
}

void kbd_init(void) {
    while (inb(0x64) & 1) inb(0x60);    /* drain anything left from the firmware */
    irq_register(1, on_key);
    pic_unmask(1);
}

int kbd_poll(void) {
    if (head == tail) return -1;
    int k = ring[tail];
    tail = (tail + 1) % sizeof ring;
    return k;
}
