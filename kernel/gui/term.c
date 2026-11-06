/* The terminal: a window the console draws into. The shell doesn't know it's
   in a window; it just reads keys from here instead of from the keyboard. */
#include "gui/wm.h"
#include "drivers/console.h"

static volatile uint8_t queue[256];     /* filled by the desktop task, emptied by the shell */
static volatile unsigned head, tail;

static void push(int k) {
    unsigned next = (head + 1) % sizeof queue;
    if (next != tail) { queue[head] = (uint8_t)k; head = next; }
}

static void key(struct window *w, int k) { (void)w; push(k); }
static void redraw(struct window *w)     { (void)w; }   /* the shell repaints its own console */
static bool close(struct window *w)      { (void)w; return false; }   /* hide: the shell lives on */

const struct app term_app = { "terminal", NULL, key, NULL, redraw, close };

void term_open(void) {
    struct window *w = wm_find(&term_app);
    if (w) { wm_show(w); return; }
    w = wm_open(&term_app, "terminal", 740, 456, NULL);
    if (!w) return;
    wm_move(w, 48, WM_PANEL_H + 36);
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
