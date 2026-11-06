/* The terminal: a window the console draws into. The shell doesn't know it's
   in a window; it just reads keys from here instead of from the keyboard. */
#include "gui/wm.h"
#include "drivers/console.h"
#include "arch/cpu.h"

static volatile uint8_t queue[256];     /* filled by the desktop task, emptied by the shell */
static volatile unsigned head, tail;

static void push(int k) {
    unsigned next = (head + 1) % sizeof queue;
    if (next != tail) { queue[head] = (uint8_t)k; head = next; }
}

static void key(struct window *w, int k)  { (void)w; push(k); }
static void redraw(struct window *w)      { (void)w; con_redraw(); }
static void resize(struct window *w)      { con_reattach(w->surf); }
static bool close(struct window *w)       { (void)w; return false; }   /* hide: the shell lives on */

const struct app term_app = { .name = "terminal", .key = key, .redraw = redraw, .close = close,
                              .resize = resize, .icon = ICON_TERMINAL };

void term_open(void) {
    struct window *w = wm_find(&term_app);
    if (w) { wm_show(w); return; }
    w = wm_open(&term_app, "terminal", 760, 470, NULL);
    if (!w) return;
    wm_resizable(w, 360, 160);
    wm_move(w, 60, WM_PANEL_H + 40);
    head = tail = 0;
    con_attach(w->surf, false);
}

int term_key(void) {
    if (head == tail) return -1;
    int k = queue[tail];
    tail = (tail + 1) % sizeof queue;
    return k;
}

void term_inject(const char *keys) { while (*keys) push((uint8_t)*keys++); }
