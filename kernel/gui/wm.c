#include "gui/wm.h"
#include "drivers/fb.h"
#include "drivers/mouse.h"
#include "drivers/keyboard.h"
#include "drivers/theme.h"
#include "drivers/rtc.h"
#include "drivers/console.h"
#include "arch/cpu.h"
#include "arch/io.h"
#include "mm/heap.h"
#include "shell/shell.h"
#include "sched/task.h"

#define MAX_WIN  16
#define TITLE_H  WM_TITLE_H
#define PANEL_H  WM_PANEL_H
#define RADIUS   10
#define SHADOW   18           /* how far the shadow spreads */
#define SHADOW_Y 6            /* light from above: shadow sits a little lower */
#define CLOSE_R  8

static bool active;
static struct surface *screen, *wall;

static struct window *z[MAX_WIN];      /* stacking order: z[0] at the back */
static int nz;
static struct window *focus, *drag, *pressed, *hover_close;
static int drag_dx, drag_dy;

static int mx, my;
static uint8_t mbuttons;

static struct rect dmg[24];            /* parts of the screen to redraw */
static int ndmg;

static uint64_t last_tick;
static char clock_text[40];

struct pbtn { int x, w; const char *label; const struct app *app; void (*open)(void); };
static struct pbtn pbtns[6];
static int npbtn, hover_btn = -1;

extern const struct app term_app, paint_app, clock_app, about_app, files_app;
static void request_exit(void);

/* ---- damage -------------------------------------------------------------------- */

static void damage(struct rect r) {
    if (!rect_intersect(r, (struct rect){ 0, 0, screen->w, screen->h }, &r)) return;
    for (int i = 0; i < ndmg; i++) {        /* close to an existing area? merge */
        struct rect near = { dmg[i].x - 32, dmg[i].y - 32, dmg[i].w + 64, dmg[i].h + 64 };
        if (rect_intersect(r, near, NULL)) { dmg[i] = rect_union(dmg[i], r); return; }
    }
    if (ndmg < 24) { dmg[ndmg++] = r; return; }
    for (int i = 1; i < ndmg; i++) dmg[0] = rect_union(dmg[0], dmg[i]);
    dmg[0] = rect_union(dmg[0], r);
    ndmg = 1;
}

static struct rect frame_of(struct window *w) { return (struct rect){ w->x, w->y, w->w, w->h + TITLE_H }; }

static struct rect outer_of(struct window *w) {     /* frame plus shadow */
    struct rect f = frame_of(w);
    return (struct rect){ f.x - SHADOW, f.y - SHADOW + SHADOW_Y, f.w + 2 * SHADOW, f.h + 2 * SHADOW };
}

static struct rect title_of(struct window *w) { return (struct rect){ w->x, w->y, w->w, TITLE_H }; }
static struct rect cursor_rect(void) { return (struct rect){ mx, my, CURSOR_W, CURSOR_H }; }
static void damage_all(void) { damage((struct rect){ 0, 0, screen->w, screen->h }); }

/* ---- the wallpaper --------------------------------------------------------------- */

static void build_wallpaper(void) {
    surface_free(wall);
    wall = surface_new(screen->w, screen->h);
    if (!wall) return;
    int W = wall->w, H = wall->h;
    struct { int x, y, r; uint32_t c; int a; } glow[3] = {    /* soft blobs of colour */
        { W * 4 / 5, H / 4,      H * 3 / 5, theme->pal[PAL_ACCENT], 80 },
        { W / 6,     H * 5 / 6,  H / 2,     theme->pal[PAL_OK],     50 },
        { W * 3 / 5, H + H / 10, H / 2,     theme->pal[PAL_ERR],    40 },
    };
    uint32_t seed = 0x9e3779b9;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint32_t c = gfx_mix(theme->bg_top, theme->bg_bottom, x * 128 / W + y * 128 / H);
            for (int g = 0; g < 3; g++) {
                int64_t dx = x - glow[g].x, dy = y - glow[g].y;
                int64_t d2 = dx * dx + dy * dy, r2 = (int64_t)glow[g].r * glow[g].r;
                if (d2 >= r2) continue;
                int64_t f = 256 - d2 * 256 / r2;              /* 256 at the centre, 0 at the edge */
                c = gfx_mix(c, glow[g].c, (int)(f * f / 256 * glow[g].a / 256));
            }
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;   /* a little film grain */
            if ((seed & 7) == 0) c = gfx_mix(c, 0xffffff, 4);
            else if ((seed & 7) == 1) c = gfx_mix(c, 0x000000, 4);
            wall->px[y * W + x] = c;
        }
    }
    gfx_text_scaled(wall, W - 32 - 5 * FONT_W * 5, H - 32 - FONT_H * 5, "dream", 5, theme->pal[PAL_FG], 16);
}

/* ---- drawing a window ------------------------------------------------------------ */

static void draw_shadow(struct rect f, struct rect clip) {
    struct rect sh = { f.x, f.y + SHADOW_Y, f.w, f.h };
    struct rect area = { sh.x - SHADOW, sh.y - SHADOW, sh.w + 2 * SHADOW, sh.h + 2 * SHADOW }, r;
    if (!rect_intersect(area, clip, &r)) return;
    for (int y = r.y; y < r.y + r.h; y++) {
        for (int x = r.x; x < r.x + r.w; x++) {
            /* inside the window (away from its round corners): it'll be covered anyway */
            if (x >= f.x + RADIUS && x < f.x + f.w - RADIUS && y >= f.y && y < f.y + f.h) continue;
            if (y >= f.y + RADIUS && y < f.y + f.h - RADIUS && x >= f.x && x < f.x + f.w) continue;
            int dx = x < sh.x ? sh.x - x : (x >= sh.x + sh.w ? x - sh.x - sh.w + 1 : 0);
            int dy = y < sh.y ? sh.y - y : (y >= sh.y + sh.h ? y - sh.y - sh.h + 1 : 0);
            int d = (dx > dy ? dx + dy / 2 : dy + dx / 2);          /* roughly the distance */
            if (d >= SHADOW) continue;
            int t = SHADOW - d;
            uint32_t *p = gfx_at(screen, x, y);
            *p = gfx_mix(*p, 0x000000, t * t * 120 / (SHADOW * SHADOW));
        }
    }
}

static void draw_window(struct window *w, struct rect clip) {
    struct rect f = frame_of(w), r;
    draw_shadow(f, clip);
    if (!rect_intersect(f, clip, &r)) return;

    bool foc = w == focus;
    uint32_t title_bg = foc ? theme->bar : gfx_mix(theme->bar, theme->bg_top, 110);

    /* the body, row by row, trimming the rounded corners (with soft edges) */
    for (int y = r.y; y < r.y + r.h; y++) {
        int j = y - f.y;
        int dy = j < RADIUS ? j : (j >= f.h - RADIUS ? f.h - 1 - j : -1);
        int inset = dy >= 0 ? gfx_corner_inset(RADIUS, dy) : 0;
        int full = inset >> 8, frac = inset & 255;
        int x0 = f.x + full, x1 = f.x + f.w - full;
        int a = x0 > r.x ? x0 : r.x, b = x1 < r.x + r.w ? x1 : r.x + r.w;
        uint32_t *src = j >= TITLE_H ? gfx_at(w->surf, 0, j - TITLE_H) : NULL;
        for (int x = a; x < b; x++) {
            uint32_t c = src ? src[x - f.x] : title_bg;
            uint32_t *p = gfx_at(screen, x, y);
            *p = (frac && (x == x0 || x == x1 - 1)) ? gfx_mix(*p, c, 256 - frac) : c;
        }
    }

    /* title bar: dot, title, close button */
    gfx_fill(screen, f.x, f.y + TITLE_H - 1, f.w, 1, gfx_mix(title_bg, theme->pal[PAL_DIM], 70));
    int ty = f.y + (TITLE_H - FONT_H) / 2;
    gfx_circle(screen, f.x + 16, f.y + TITLE_H / 2, 4, foc ? theme->pal[PAL_ACCENT] : theme->pal[PAL_DIM]);
    gfx_text(screen, f.x + 28, ty, w->title, foc ? theme->pal[PAL_FG] : theme->pal[PAL_DIM], -1);

    int cx = f.x + f.w - 20, cy = f.y + TITLE_H / 2;
    bool hot = hover_close == w;
    if (hot) gfx_circle(screen, cx, cy, CLOSE_R, theme->pal[PAL_ERR]);
    uint32_t xc = hot ? title_bg : (foc ? theme->pal[PAL_FG] : theme->pal[PAL_DIM]);
    gfx_line(screen, cx - 3, cy - 3, cx + 3, cy + 3, 2, xc);
    gfx_line(screen, cx + 3, cy - 3, cx - 3, cy + 3, 2, xc);
}

/* ---- the panel along the top ----------------------------------------------------- */

static void layout_panel(void) {
    static const struct { const char *label; const struct app *app; void (*open)(void); } apps[] = {
        { "terminal", &term_app, term_open }, { "files", &files_app, files_open },
        { "paint", &paint_app, paint_open },
        { "clock", &clock_app, clock_open },  { "about", &about_app, about_open },
    };
    int x = 110;
    npbtn = 0;
    for (unsigned i = 0; i < sizeof apps / sizeof apps[0]; i++) {
        int w = gfx_text_width(apps[i].label) + 20;
        pbtns[npbtn++] = (struct pbtn){ x, w, apps[i].label, apps[i].app, apps[i].open };
        x += w + 4;
    }
    int cw = gfx_text_width(clock_text);
    pbtns[npbtn++] = (struct pbtn){ screen->w - 20 - cw - 24 - 56, 56, "exit", NULL, request_exit };
}

static bool app_visible(const struct app *a) {
    struct window *w = wm_find(a);
    return w && !w->hidden;
}

static void draw_panel(struct rect clip) {
    if (!rect_intersect((struct rect){ 0, 0, screen->w, PANEL_H }, clip, NULL)) return;
    uint32_t fg = theme->pal[PAL_FG], dim = theme->pal[PAL_DIM], acc = theme->pal[PAL_ACCENT];
    gfx_blend(screen, 0, 0, screen->w, PANEL_H, theme->bar, 210);
    gfx_fill(screen, 0, PANEL_H - 1, screen->w, 1, gfx_mix(theme->bar, acc, 80));

    int ty = (PANEL_H - FONT_H) / 2;
    gfx_circle(screen, 22, PANEL_H / 2, 5, acc);
    gfx_text(screen, 36, ty, "dream", fg, -1);

    for (int i = 0; i < npbtn; i++) {
        struct pbtn *b = &pbtns[i];
        bool open = b->app && app_visible(b->app);
        if (i == hover_btn) gfx_round(screen, b->x, 5, b->w, PANEL_H - 10, 7, gfx_mix(theme->bar, fg, 40));
        gfx_text(screen, b->x + 10, ty, b->label, open || i == hover_btn ? fg : dim, -1);
        if (open) gfx_round(screen, b->x + b->w / 2 - 8, PANEL_H - 5, 16, 3, 1, acc);
    }
    gfx_text(screen, screen->w - 20 - gfx_text_width(clock_text), ty, clock_text, fg, -1);
}

static void update_clock(void) {
    struct rtc_time t;
    rtc_now(&t);
    char now[40];
    snprintf(now, sizeof now, "%s %u %s   %02u:%02u", rtc_day_names[rtc_weekday(&t)], t.day,
             rtc_month_names[(t.mon + 11) % 12], t.hour, t.min);
    if (strcmp(now, clock_text)) {
        strlcpy(clock_text, now, sizeof clock_text);
        layout_panel();
        damage((struct rect){ 0, 0, screen->w, PANEL_H });
    }
}

/* ---- putting a frame together ------------------------------------------------------ */

static void compose(struct rect r) {
    gfx_clip(screen, &r);
    gfx_blit(screen, r.x, r.y, wall, r.x, r.y, r.w, r.h);
    for (int i = 0; i < nz; i++)
        if (!z[i]->hidden && rect_intersect(outer_of(z[i]), r, NULL)) draw_window(z[i], r);
    draw_panel(r);
    gfx_cursor(screen, mx, my);
    gfx_clip(screen, NULL);
    fb_present_rect(r);
}

/* ---- window bookkeeping ------------------------------------------------------------ */

static int index_of(struct window *w) {
    for (int i = 0; i < nz; i++) if (z[i] == w) return i;
    return -1;
}

static void set_focus(struct window *w) {
    if (focus == w) return;
    if (focus) damage(title_of(focus));
    focus = w;
    if (w) damage(outer_of(w));
}

static void raise(struct window *w) {
    int i = index_of(w);
    if (i < 0 || i == nz - 1) return;
    memmove(&z[i], &z[i + 1], sizeof z[0] * (nz - 1 - i));
    z[nz - 1] = w;
    damage(outer_of(w));
}

static struct window *topmost_visible(void) {
    for (int i = nz - 1; i >= 0; i--) if (!z[i]->hidden) return z[i];
    return NULL;
}

static struct window *window_at(int x, int y) {
    for (int i = nz - 1; i >= 0; i--)
        if (!z[i]->hidden && rect_contains(frame_of(z[i]), x, y)) return z[i];
    return NULL;
}

struct window *wm_open(const struct app *app, const char *title, int w, int h, void *data) {
    if (!active || nz == MAX_WIN) return NULL;
    struct window *win = kzalloc(sizeof *win);
    if (!win) return NULL;
    win->surf = surface_new(w, h);
    if (!win->surf) { kfree(win); return NULL; }
    gfx_fill(win->surf, 0, 0, w, h, theme->bg_bottom);
    strlcpy(win->title, title, sizeof win->title);
    win->app = app;
    win->data = data;
    win->w = w;
    win->h = h;

    static int cascade;                         /* each new window a little further along */
    int step = (cascade++ % 5) * 28;
    win->x = (screen->w - w) / 2 - 56 + step;
    win->y = PANEL_H + 40 + step;
    if (win->x + w > screen->w - 8) win->x = screen->w - 8 - w;
    if (win->x < 8) win->x = 8;
    if (win->y + h + TITLE_H > screen->h - 8) win->y = screen->h - 8 - h - TITLE_H;
    if (win->y < PANEL_H + 4) win->y = PANEL_H + 4;

    z[nz++] = win;
    set_focus(win);
    damage(outer_of(win));
    return win;
}

void wm_close(struct window *w) {
    int i = index_of(w);
    if (i < 0) return;
    damage(outer_of(w));
    memmove(&z[i], &z[i + 1], sizeof z[0] * (nz - 1 - i));
    nz--;
    if (drag == w) drag = NULL;
    if (pressed == w) pressed = NULL;
    if (hover_close == w) hover_close = NULL;
    if (focus == w) { focus = NULL; set_focus(topmost_visible()); }
    surface_free(w->surf);
    kfree(w);
    damage((struct rect){ 0, 0, screen->w, PANEL_H });   /* the panel shows what's open */
}

void wm_show(struct window *w) {
    w->hidden = false;
    raise(w);
    set_focus(w);
    damage(outer_of(w));
    damage((struct rect){ 0, 0, screen->w, PANEL_H });
}

void wm_move(struct window *w, int x, int y) {
    if (y < PANEL_H) y = PANEL_H;
    if (y > screen->h - TITLE_H) y = screen->h - TITLE_H;
    if (x < 60 - w->w) x = 60 - w->w;               /* keep a bit of it grabbable */
    if (x > screen->w - 60) x = screen->w - 60;
    if (x == w->x && y == w->y) return;
    damage(outer_of(w));
    w->x = x;
    w->y = y;
    damage(outer_of(w));
}

struct window *wm_find(const struct app *app) {
    for (int i = 0; i < nz; i++) if (z[i]->app == app) return z[i];
    return NULL;
}

static void close_clicked(struct window *w) {
    if (w->app->close && !w->app->close(w)) {       /* the app would rather just hide */
        w->hidden = true;
        damage(outer_of(w));
        damage((struct rect){ 0, 0, screen->w, PANEL_H });
        if (focus == w) { focus = NULL; set_focus(topmost_visible()); }
        return;
    }
    wm_close(w);
}

/* ---- input ------------------------------------------------------------------------- */

static bool on_close_button(struct window *w, int x, int y) {
    int dx = x - (w->x + w->w - 20), dy = y - (w->y + TITLE_H / 2);
    return dx * dx + dy * dy <= (CLOSE_R + 3) * (CLOSE_R + 3);
}

static void update_hover(void) {
    int hb = -1;
    if (my < PANEL_H)
        for (int i = 0; i < npbtn; i++) if (mx >= pbtns[i].x && mx < pbtns[i].x + pbtns[i].w) hb = i;
    if (hb != hover_btn) { hover_btn = hb; damage((struct rect){ 0, 0, screen->w, PANEL_H }); }

    struct window *w = drag ? NULL : window_at(mx, my);
    struct window *hc = w && on_close_button(w, mx, my) ? w : NULL;
    if (hc != hover_close) {
        if (hover_close) damage(title_of(hover_close));
        if (hc) damage(title_of(hc));
        hover_close = hc;
    }
}

static void handle_mouse(struct mouse_event *e) {
    if (e->x != mx || e->y != my) {
        damage(cursor_rect());
        mx = e->x;
        my = e->y;
        damage(cursor_rect());
    }
    uint8_t down = e->buttons & ~mbuttons;
    update_hover();

    if (drag) {
        if (e->buttons & MOUSE_LEFT) wm_move(drag, mx - drag_dx, my - drag_dy);
        else drag = NULL;
    } else if (down) {
        if (my < PANEL_H) {
            if ((down & MOUSE_LEFT) && hover_btn >= 0) pbtns[hover_btn].open();
        } else {
            struct window *w = window_at(mx, my);
            if (w) {
                raise(w);
                set_focus(w);
                if (my < w->y + TITLE_H) {
                    if (down & MOUSE_LEFT) {
                        if (on_close_button(w, mx, my)) { close_clicked(w); mbuttons = e->buttons; return; }
                        drag = w;
                        drag_dx = mx - w->x;
                        drag_dy = my - w->y;
                    }
                } else {
                    pressed = w;                  /* it gets the mouse until the button is let go */
                }
            } else {
                set_focus(NULL);
            }
        }
    }

    /* pass it on to the app: the one we pressed in, or whatever is under the pointer */
    struct window *t = pressed ? pressed : (drag ? NULL : window_at(mx, my));
    if (t && t->app->mouse && (pressed || my >= t->y + TITLE_H))
        t->app->mouse(t, mx - t->x, my - t->y - TITLE_H, e->buttons, e->wheel);
    if (!e->buttons) pressed = NULL;
    mbuttons = e->buttons;
}

/* The panel's exit button types `exit` into the terminal, as if you had. */
static void request_exit(void) {
    if (sh_at_prompt()) term_inject("\003exit\n");
}

/* ---- the desktop task ---------------------------------------------------------------- */

static volatile uint8_t requests[16];
static volatile unsigned rq_head, rq_tail;
static volatile bool stopping, stopped;

void wm_post(int r) {
    unsigned next = (rq_head + 1) % 16;
    if (next != rq_tail) { requests[rq_head] = r; rq_head = next; }
}

static void rebuild_for_theme(void) {
    build_wallpaper();
    for (int i = 0; i < nz; i++) if (z[i]->app->redraw) z[i]->app->redraw(z[i]);
    damage_all();
}

static void handle_requests(void) {
    while (rq_tail != rq_head) {
        int r = requests[rq_tail];
        rq_tail = (rq_tail + 1) % 16;
        if (r == WM_REQ_PAINT) paint_open();
        if (r == WM_REQ_FILES) files_open();
        if (r == WM_REQ_THEME) rebuild_for_theme();
        if (r == WM_REQ_STOP)  stopping = true;
    }
}

/* One step: handle input, run app ticks, redraw what changed. */
static void pump(void) {
    struct mouse_event e;
    while (mouse_poll(&e)) handle_mouse(&e);

    int k;
    while ((k = kbd_poll()) >= 0)
        if (focus && !focus->hidden && focus->app->key) focus->app->key(focus, k);

    uint64_t t = pit_ticks();
    if (t - last_tick >= pit_hz() / 10) {
        last_tick = t;
        for (int i = 0; i < nz; i++) if (z[i]->app->tick) z[i]->app->tick(z[i]);
        update_clock();
    }

    /* windows whose contents changed (the shell draws into the terminal from its
       own task; it just marks damage, and we pick it up here) */
    for (int i = 0; i < nz; i++) {
        struct surface *s = z[i]->surf;
        uint64_t f = irq_save();
        struct rect d = s->damage;
        s->damage = (struct rect){ 0, 0, 0, 0 };
        irq_restore(f);
        if (!rect_empty(d) && !z[i]->hidden)
            damage((struct rect){ z[i]->x + d.x, z[i]->y + TITLE_H + d.y, d.w, d.h });
    }

    for (int i = 0; i < ndmg; i++) compose(dmg[i]);
    ndmg = 0;
}

static void desktop_task(void *arg) {
    (void)arg;
    while (!stopping) {
        handle_requests();
        if (stopping) break;
        pump();
        task_sleep(10);                   /* ~100 frames a second at most; others get the CPU */
    }
    while (nz) wm_close(z[nz - 1]);
    surface_free(wall);
    wall = NULL;
    ndmg = 0;
    stopped = true;
}

/* ---- starting and stopping --------------------------------------------------------- */

bool wm_active(void) { return active; }

void wm_start(void) {
    if (active) return;
    screen = fb_screen();
    build_wallpaper();
    if (!wall) { kerrorf(C_ERR "startx" C_DIM ": not enough memory for the desktop\n"); return; }
    nz = 0;
    focus = drag = pressed = hover_close = NULL;
    clock_text[0] = 0;
    rq_head = rq_tail = 0;
    stopping = stopped = false;
    struct mouse_event e;
    while (mouse_poll(&e)) {}
    mouse_where(&mx, &my);
    active = true;
    update_clock();
    term_open();                          /* the shell's console moves into this window */
    clock_open();
    wm_show(wm_find(&term_app));          /* the terminal gets the keyboard */
    damage_all();
    if (!task_spawn("desktop", desktop_task, NULL)) {
        kerrorf(C_ERR "startx" C_DIM ": couldn't start the desktop task\n");
        stopping = true;
        desktop_task(NULL);               /* tidy up right here */
        active = false;
        con_attach(fb_screen(), true);
    }
}

void wm_stop(void) {
    if (!active) return;
    wm_post(WM_REQ_STOP);
    while (!stopped) task_sleep(10);      /* wait for the desktop task to wind down */
    active = false;
    con_attach(fb_screen(), true);
    fb_present_all();
}

void wm_theme_changed(void) {
    if (active) wm_post(WM_REQ_THEME);
}

/* ---- shell commands ---------------------------------------------------------------- */

SH_CMD(startx) {
    (void)argc; (void)argv;
    if (active) { kprintf(C_DIM "the desktop is already running.\n"); return; }
    wm_start();
    if (!active) return;
    kprintf(C_ACCENT "welcome to the dream desktop.\n" C_DIM
            "drag windows by their title bars. open apps from the panel.\n"
            "type " C_FG "exit" C_DIM " (or click exit) to go back to text mode.\n\n");
}

SH_CMD(files) {
    (void)argc; (void)argv;
    if (!active) wm_start();
    wm_post(WM_REQ_FILES);
}

SH_CMD(exit) {
    (void)argc; (void)argv;
    if (!active) { kprintf(C_DIM "not in the desktop. (" C_FG "shutdown" C_DIM " powers off)\n"); return; }
    wm_stop();
    kprintf(C_DIM "left the desktop. " C_FG "startx" C_DIM " to go back.\n");
}
