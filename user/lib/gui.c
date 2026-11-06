/* The program side of dream's window system (see include/dream/abi.h and
   kernel/gui/uwin.c for the kernel side). */
#include "gui.h"

static long sys(long n, long a, long b, long c) {
    long ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(n), "D"(a), "S"(b), "d"(c) : "memory");
    return ret;
}

/* ---- the batch of drawing commands -------------------------------------------------- */

static uint8_t batch[32 * 1024] __attribute__((aligned(8)));
static size_t used;
static int target = -1;                    /* the window the batch is for */

void win_flush(int win) {
    if (used && win >= 0) sys(SYS_DRAW, win, (long)batch, used);
    used = 0;
}

static struct dcmd *add(int op, const char *text) {
    size_t tl = text ? strlen(text) + 1 : 0;
    if (tl > 255) tl = 255;
    size_t size = (sizeof(struct dcmd) + tl + 3) & ~(size_t)3;   /* keep the next one aligned */
    if (used + size > sizeof batch) win_flush(target);              /* full: send what we have */
    struct dcmd *c = (struct dcmd *)(batch + used);
    memset(c, 0, sizeof *c);
    c->op = op;
    c->size = size;
    if (tl) { memcpy(c->text, text, tl - 1); c->text[tl - 1] = 0; }
    used += size;
    return c;
}

static struct dcmd *box(int op, int x, int y, int w, int h, uint32_t c1, const char *text) {
    struct dcmd *c = add(op, text);
    c->x = x; c->y = y; c->w = w; c->h = h; c->c1 = c1;
    return c;
}

void draw_fill(int x, int y, int w, int h, uint32_t c) { box(DRAW_FILL, x, y, w, h, c, NULL); }
void draw_round(int x, int y, int w, int h, int r, uint32_t c) { box(DRAW_ROUND, x, y, w, h, c, NULL)->r = r; }
void draw_round_a(int x, int y, int w, int h, int r, uint32_t c, int alpha) {
    struct dcmd *d = box(DRAW_ROUND, x, y, w, h, c, NULL);
    d->r = r;
    d->c2 = alpha < 1 ? 1 : alpha;
}
void draw_ring(int x, int y, int w, int h, int r, uint32_t c) { box(DRAW_RING, x, y, w, h, c, NULL)->r = r; }
void draw_circle(int x, int y, int r, uint32_t c) { box(DRAW_CIRCLE, x, y, 0, 0, c, NULL)->r = r; }
void draw_line(int x0, int y0, int x1, int y1, int thick, uint32_t c) { box(DRAW_LINE, x0, y0, x1, y1, c, NULL)->r = thick; }
void draw_text(int font, int x, int y, const char *s, uint32_t c) { box(DRAW_TEXT, x, y, 0, 0, c, s)->font = font; }
void draw_text_fit(int font, int x, int y, const char *s, uint32_t c, int max_w) {
    box(DRAW_TEXT, x, y, max_w, 0, c, s)->font = font;
}
void draw_glyph(int glyph, int x, int y, int size, uint32_t c) {
    struct dcmd *d = box(DRAW_GLYPH, x, y, 0, 0, c, NULL);
    d->r = size;
    d->arg = glyph;
}
void draw_icon(int icon, int x, int y, int size) {
    struct dcmd *d = box(DRAW_ICON, x, y, 0, 0, 0, NULL);
    d->r = size;
    d->arg = icon;
}
void draw_button(int x, int y, int w, int h, const char *label, bool hot, bool primary) {
    box(DRAW_BUTTON, x, y, w, h, 0, label)->arg = (hot ? 1 : 0) | (primary ? 2 : 0);
}
void draw_field(int x, int y, int w, int h, const char *text, bool focused, bool secret) {
    box(DRAW_FIELD, x, y, w, h, 0, text)->arg = (focused ? 1 : 0) | (secret ? 2 : 0);
}
void draw_ring_a(int x, int y, int w, int h, int r, uint32_t c, int alpha) {
    struct dcmd *d = box(DRAW_RING, x, y, w, h, c, NULL);
    d->r = r;
    d->c2 = alpha < 1 ? 1 : alpha;
}
void draw_gradient(int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom) {
    struct dcmd *d = box(DRAW_GRADIENT, x, y, w, h, top, NULL);
    d->r = r;
    d->c2 = bottom;
}
void draw_text_center(int font, int cx, int y, const char *s, uint32_t c) {
    draw_text(font, cx - text_width(font, s) / 2, y, s, c);
}
void draw_toggle(int x, int y, bool on) {
    draw_round(x, y, 40, 22, 11, on ? COLOR_UI(UI_ACCENT) : COLOR_UI(UI_RAISED));
    draw_circle(on ? x + 29 : x + 11, y + 11, 8, on ? 0xffffff : COLOR_UI(UI_DIM));
}
void draw_wallpaper(int x, int y, int w, int h, const char *style) { box(DRAW_WALLPAPER, x, y, w, h, 0, style); }
void draw_scrollbar(int x, int y, int h, int total, int visible, int pos) {
    struct dcmd *d = box(DRAW_SCROLLBAR, x, y, 0, h, total, NULL);
    d->c2 = visible;
    d->arg = pos;
}

/* ---- windows ---------------------------------------------------------------------------- */

/* ---- the theme's colours ------------------------------------------------------------- */

static struct theme_info palette;
static void refresh_palette(void) { theme_get(-1, &palette); }
uint32_t ui(int slot) { return slot >= 0 && slot < 16 ? palette.ui[slot] : 0; }
const struct theme_info *current_theme(void) { return &palette; }

uint32_t mix(uint32_t a, uint32_t b, int t) {
    if (t <= 0) return a;
    if (t >= 256) return b;
    uint32_t rb = (((a & 0xff00ff) * (256 - t) + (b & 0xff00ff) * t) >> 8) & 0xff00ff;
    uint32_t g  = (((a & 0x00ff00) * (256 - t) + (b & 0x00ff00) * t) >> 8) & 0x00ff00;
    return rb | g;
}

int win_open(const char *title, int w, int h, int icon, int min_w, int min_h) {
    refresh_palette();
    struct win_open o = { title, w, h, icon, min_w, min_h };
    long r = sys(SYS_WINDOW, (long)&o, 0, 0);
    if (r >= 0) target = (int)r;
    return (int)r;
}

int win_event(int win, struct event *e, int wait_ms) {
    win_flush(win);                         /* whatever was drawn, show it before waiting */
    target = win;
    int got = (int)sys(SYS_EVENT, win, (long)e, wait_ms);
    if (got && e->type == EV_REDRAW) refresh_palette();   /* the theme changed */
    return got;
}

void app_run(int win, const struct app_loop *a) {
    unsigned long next_tick = uptime_ms() + a->tick_ms;
    if (a->draw) a->draw();
    for (;;) {
        struct event e;
        int wait = 1000000;
        if (a->tick_ms) {
            unsigned long now = uptime_ms();
            wait = next_tick > now ? (int)(next_tick - now) : 0;
        }
        if (!win_event(win, &e, wait)) {               /* nothing happened: it's tick time */
            next_tick = uptime_ms() + a->tick_ms;
            if (a->tick) a->tick();
            continue;
        }
        switch (e.type) {
        case EV_CLOSE:  if (!a->close || a->close()) return; break;
        case EV_KEY:    if (a->key) a->key(e.key); break;
        case EV_MOUSE:  if (a->mouse) a->mouse(e.x, e.y, e.buttons, e.wheel); break;
        case EV_RESIZE: if (a->resize) a->resize(e.w, e.h); else if (a->draw) a->draw(); break;
        case EV_REDRAW: if (a->draw) a->draw(); break;
        }
        if (a->tick_ms && uptime_ms() >= next_tick) {  /* busy with events: still keep time */
            next_tick = uptime_ms() + a->tick_ms;
            if (a->tick) a->tick();
        }
    }
}

void win_title(int win, const char *t) { sys(SYS_WINCTL, win, WINCTL_TITLE, (long)t); }
void win_close(int win) { win_flush(win); sys(SYS_WINCTL, win, WINCTL_CLOSE, 0); }
void win_front(int win) { sys(SYS_WINCTL, win, WINCTL_FRONT, 0); }

void win_size(int win, int *w, int *h) {
    long r = sys(SYS_WINCTL, win, WINCTL_SIZE, 0);
    *w = (int)(r & 0xffffffff);
    *h = (int)((unsigned long)r >> 32);
}

void notify(int win, const char *title, const char *body) {
    char msg[160];
    size_t n = 0;
    for (const char *s = title; *s && n < 60; s++) msg[n++] = *s;
    msg[n++] = '\n';
    for (const char *s = body; *s && n < sizeof msg - 1; s++) msg[n++] = *s;
    msg[n] = 0;
    sys(SYS_WINCTL, win, WINCTL_NOTIFY, (long)msg);
}

int text_width(int font, const char *s) { return (int)sys(SYS_MEASURE, font, (long)s, 0); }
