/* The desktop: a compositor and window manager.
 *
 * Layers, back to front:  wallpaper, windows, the top bar and the dock (frosted
 * glass), menus and notifications, then the pointer. Full-screen modes (the
 * launcher and the lock screen) draw over a blurred snapshot instead.
 *
 * Only the parts of the screen that changed ("damage") are rebuilt each frame. */
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
#include "fs/ramfs.h"

#define MAX_WIN    16
#define TITLE_H    WM_TITLE_H
#define PANEL_H    WM_PANEL_H
#define RADIUS     12
#define EDGE       6           /* how close to a window's edge counts as grabbing it to resize */
#define DOCK_ICON  46
#define DOCK_GAP   10
#define DOCK_PAD   11
#define DOCK_H     (DOCK_ICON + 2 * DOCK_PAD)
#define DOCK_BOTTOM 10
#define FADE_STEP  64          /* 4 frames to fade in or out */
#define NOTE_MAX   4
#define NOTE_W     340
#define NOTE_H     66
#define NOTE_LIFE  5           /* seconds */

enum { THEN_NOTHING, THEN_CLOSE, THEN_HIDE, THEN_MINIMIZE };
enum { BTN_NONE, BTN_MIN, BTN_MAX, BTN_CLOSE };
enum { POP_NONE, POP_POWER, POP_CALENDAR, POP_CONTEXT };
enum { SNAP_NONE, SNAP_LEFT, SNAP_RIGHT, SNAP_MAX };
enum { CUR_ARROW, CUR_EW, CUR_NS, CUR_NWSE, CUR_NESW };

static bool active;
static struct surface *screen, *wall, *glass, *backdrop;

static struct window *z[MAX_WIN];      /* stacking order: z[0] at the back */
static int nz;
static struct window *focus, *drag, *pressed, *sizing;
static int drag_dx, drag_dy, size_edges;
static struct rect size_start;
static int size_mx, size_my, pend_w, pend_h;
static bool size_pending;
static int snap;

static struct window *hover_win;
static int hover_btn;

static int mx, my, cursor_kind;
static uint8_t mbuttons;

static struct rect dmg[24];
static int ndmg;

static uint64_t last_tick, last_sec_tick;
static char clock_text[40];

/* ---- colours ---------------------------------------------------------------------- */

uint32_t ui(int which) {
    uint32_t fg = theme->pal[PAL_FG];
    uint32_t bg = gfx_mix(theme->bar, theme->bg_top, 90);
    switch (which) {
    case UI_BG:        return bg;
    case UI_SIDEBAR:   return gfx_mix(bg, 0x000000, 40);
    case UI_RAISED:    return gfx_mix(bg, fg, 20);
    case UI_HOVER:     return gfx_mix(bg, fg, 34);
    case UI_LINE:      return gfx_mix(bg, fg, 30);
    case UI_SEL:       return gfx_mix(bg, theme->pal[PAL_ACCENT], 72);
    case UI_TEXT:      return fg;
    case UI_DIM:       return theme->pal[PAL_DIM];
    case UI_ACCENT:    return theme->pal[PAL_ACCENT];
    case UI_OK:        return theme->pal[PAL_OK];
    case UI_WARN:      return theme->pal[PAL_WARN];
    case UI_ERR:       return theme->pal[PAL_ERR];
    case UI_ON_ACCENT: return gfx_mix(theme->bg_bottom, 0x000000, 60);
    }
    return fg;
}

int wm_screen_w(void) { return screen ? screen->w : (int)fb.width; }
int wm_screen_h(void) { return screen ? screen->h : (int)fb.height; }

/* ---- where things are --------------------------------------------------------------- */

static struct rect bar_rect(void) { return (struct rect){ 0, 0, screen->w, PANEL_H }; }
static struct rect frame_of(struct window *w) { return (struct rect){ w->x, w->y, w->w, w->h + TITLE_H }; }
static struct rect outer_of(struct window *w) {     /* frame plus shadow */
    struct rect f = frame_of(w);
    return (struct rect){ f.x - 36, f.y - 36, f.w + 72, f.h + 36 + 50 };
}
static struct rect title_of(struct window *w) { return (struct rect){ w->x, w->y, w->w, TITLE_H }; }
static struct rect cursor_rect(void) { return (struct rect){ mx - 12, my - 12, 36, 36 }; }

/* the dock: apps pinned to it, then anything else that's open */
struct ditem { const struct app_entry *e; const struct app *app; int x; };
static struct ditem ditems[24];
static int nditems, dock_sep_x, hover_dock = -1;
static struct rect dock;

static const struct app *app_for(const struct app_entry *e);
static struct window *window_for(const struct app_entry *e);

static void layout_dock(void) {
    nditems = 0;
    dock_sep_x = -1;
    for (int i = 0; i < app_count; i++)
        if (app_list[i].in_dock) ditems[nditems++] = (struct ditem){ &app_list[i], app_for(&app_list[i]), 0 };
    int pinned = nditems;
    for (int i = 0; i < app_count; i++) {
        struct window *w = window_for(&app_list[i]);
        if (!app_list[i].in_dock && w && (!w->hidden || w->minimized))
            ditems[nditems++] = (struct ditem){ &app_list[i], app_for(&app_list[i]), 0 };
    }
    int sep = nditems > pinned ? DOCK_GAP + 1 : 0;
    int width = nditems * DOCK_ICON + (nditems - 1) * DOCK_GAP + 2 * DOCK_PAD + sep;
    dock = (struct rect){ (screen->w - width) / 2, screen->h - DOCK_H - DOCK_BOTTOM, width, DOCK_H };
    int x = dock.x + DOCK_PAD;
    for (int i = 0; i < nditems; i++) {
        if (i == pinned && sep) { dock_sep_x = x; x += sep; }
        ditems[i].x = x;
        x += DOCK_ICON + DOCK_GAP;
    }
}

static struct rect work_area(void) {           /* where maximized windows go */
    return (struct rect){ 0, PANEL_H, screen->w, screen->h - PANEL_H - DOCK_H - DOCK_BOTTOM * 2 };
}

/* ---- damage ------------------------------------------------------------------------- */

static bool locked, launcher;
static struct rect popup_rect, switcher_rect;
static int popup;

static void damage(struct rect r) {
    if (!rect_intersect(r, (struct rect){ 0, 0, screen->w, screen->h }, &r)) return;
    if (!locked && !launcher) {
        /* frosted glass blurs what's behind it, so if anything under the bar or the
           dock changes, the whole of it has to be rebuilt */
        if (rect_intersect(r, bar_rect(), NULL)) r = rect_union(r, bar_rect());
        if (rect_intersect(r, dock, NULL)) r = rect_union(r, dock);
    }
    for (int i = 0; i < ndmg; i++) {
        struct rect near = { dmg[i].x - 32, dmg[i].y - 32, dmg[i].w + 64, dmg[i].h + 64 };
        if (rect_intersect(r, near, NULL)) { dmg[i] = rect_union(dmg[i], r); return; }
    }
    if (ndmg < 24) { dmg[ndmg++] = r; return; }
    for (int i = 1; i < ndmg; i++) dmg[0] = rect_union(dmg[0], dmg[i]);
    dmg[0] = rect_union(dmg[0], r);
    ndmg = 1;
}

static void damage_all(void) { damage((struct rect){ 0, 0, screen->w, screen->h }); }
static void damage_dock(void) {
    damage((struct rect){ dock.x - 120, dock.y - 60, dock.w + 240, dock.h + 60 + DOCK_BOTTOM });
}
static void relayout_dock(void) { damage_dock(); layout_dock(); damage_dock(); }

/* ---- frosted glass ------------------------------------------------------------------------- */

/* Copy what's on screen under `r` aside, blur it, tint it, and put it back,
   trimmed to rounded corners. That's all "glass" is. */
static void draw_glass(struct rect r, int radius, uint32_t tint, int tint_alpha, int blur) {
    if (!glass) { gfx_round_a(screen, r.x, r.y, r.w, r.h, radius, tint, 235); return; }
    struct rect c;
    if (!rect_intersect(r, (struct rect){ 0, 0, screen->w, screen->h }, &c)) return;
    for (int y = c.y; y < c.y + c.h; y++) memcpy(gfx_at(glass, c.x, y), gfx_at(screen, c.x, y), c.w * 4);
    gfx_blur(glass, c, blur);
    for (int y = c.y; y < c.y + c.h; y++) {
        uint32_t *p = gfx_at(glass, c.x, y);
        for (int x = 0; x < c.w; x++) p[x] = gfx_mix(p[x], tint, tint_alpha);
    }
    /* back onto the screen through a rounded mask (this one respects the clip) */
    struct rect save = screen->clip, cl;
    if (!rect_intersect(c, save, &cl)) return;
    for (int y = cl.y; y < cl.y + cl.h; y++) {
        int j = y - r.y;
        int dy = j < radius ? j : (j >= r.h - radius ? r.h - 1 - j : -1);
        int inset = dy >= 0 && radius ? gfx_corner_inset(radius, dy) : 0;
        int full = inset >> 8, frac = inset & 255;
        int x0 = r.x + full, x1 = r.x + r.w - full;
        for (int x = cl.x; x < cl.x + cl.w; x++) {
            if (x < x0 || x >= x1) continue;
            uint32_t *d = gfx_at(screen, x, y), s = *gfx_at(glass, x, y);
            *d = (frac && (x == x0 || x == x1 - 1)) ? gfx_mix(*d, s, 256 - frac) : s;
        }
    }
}

/* ---- drawing a window ------------------------------------------------------------------ */

static int btn_x(struct window *w, int which) {      /* centre of a title bar button */
    int slot = BTN_CLOSE - which;
    if (which == BTN_MIN && !w->min_w) slot = 1;         /* no maximize button: close the gap */
    return w->x + w->w - 22 - slot * 28;
}

static void draw_title_button(struct window *w, int which, bool foc) {
    int cx = btn_x(w, which), cy = w->y + TITLE_H / 2;
    bool hot = hover_win == w && hover_btn == which;
    uint32_t bg = ui(UI_BG);
    if (hot) gfx_circle(screen, cx, cy, 11, which == BTN_CLOSE ? ui(UI_ERR) : ui(UI_HOVER));
    else gfx_circle(screen, cx, cy, 11, gfx_mix(bg, ui(UI_TEXT), foc ? 18 : 10));
    uint32_t c = hot && which == BTN_CLOSE ? 0xffffff : (foc ? ui(UI_TEXT) : ui(UI_DIM));
    if (which == BTN_CLOSE) {
        gfx_line(screen, cx - 4, cy - 4, cx + 4, cy + 4, 2, c);
        gfx_line(screen, cx + 4, cy - 4, cx - 4, cy + 4, 2, c);
    } else if (which == BTN_MAX) {
        if (w->maximized) {
            gfx_ring(screen, cx - 4, cy - 2, 8, 8, 2, c, 256);
            gfx_fill(screen, cx - 2, cy - 5, 7, 2, c);
            gfx_fill(screen, cx + 4, cy - 5, 2, 7, c);
        } else gfx_ring(screen, cx - 5, cy - 5, 10, 10, 2, c, 256);
    } else {
        gfx_fill(screen, cx - 5, cy, 10, 2, c);
    }
}

static void draw_window(struct window *w, struct rect clip) {
    struct rect f = frame_of(w), r;
    bool foc = w == focus;
    int a = w->anim;
    if (a <= 0) return;
    if (!w->maximized) gfx_shadow(screen, f, RADIUS, foc ? 34 : 24, foc ? 12 : 6, (foc ? 150 : 95) * a / 256);
    if (!rect_intersect(f, clip, &r)) return;

    int rad = w->maximized ? 0 : RADIUS;
    uint32_t title_bg = foc ? ui(UI_BG) : gfx_mix(ui(UI_BG), theme->bg_bottom, 60);

    /* the body, row by row, trimming the rounded corners (with soft edges) */
    for (int y = r.y; y < r.y + r.h; y++) {
        int j = y - f.y;
        int dy = j < rad ? j : (j >= f.h - rad ? f.h - 1 - j : -1);
        int inset = dy >= 0 ? gfx_corner_inset(rad, dy) : 0;
        int full = inset >> 8, frac = inset & 255;
        int x0 = f.x + full, x1 = f.x + f.w - full;
        int lo = x0 > r.x ? x0 : r.x, hi = x1 < r.x + r.w ? x1 : r.x + r.w;
        if (hi <= lo) continue;
        uint32_t *dst = gfx_at(screen, 0, y);
        uint32_t *src = j >= TITLE_H ? gfx_at(w->surf, 0, j - TITLE_H) - f.x : NULL;
        if (a >= 256 && !frac) {
            if (src) memcpy(dst + lo, src + lo, (size_t)(hi - lo) * 4);
            else for (int x = lo; x < hi; x++) dst[x] = title_bg;
            continue;
        }
        for (int x = lo; x < hi; x++) {
            uint32_t c = src ? src[x] : title_bg;
            int alpha = (frac && (x == x0 || x == x1 - 1)) ? 256 - frac : 256;
            dst[x] = gfx_mix(dst[x], c, alpha * a / 256);
        }
    }
    if (a < 256) return;                       /* mid-fade: just the body */

    /* a hairline edge, lighter for the focused window, and the title bar's contents */
    gfx_ring(screen, f.x, f.y, f.w, f.h, rad + 1, ui(UI_TEXT), foc ? 46 : 26);
    int title_w = f.w - 3 * 28 - 30 - 44;
    if (w->app->icon) icon_app(screen, w->app->icon, f.x + 13, f.y + (TITLE_H - 20) / 2, 20);
    gfx_utext_fit(screen, &font_bold, f.x + (w->app->icon ? 42 : 16), f.y + (TITLE_H - font_bold.height) / 2,
                  w->title, foc ? ui(UI_TEXT) : ui(UI_DIM), title_w);
    draw_title_button(w, BTN_MIN, foc);
    if (w->min_w) draw_title_button(w, BTN_MAX, foc);
    draw_title_button(w, BTN_CLOSE, foc);
}

/* ---- the top bar --------------------------------------------------------------------------- */

static struct rect logo_btn, clock_btn, power_btn, cpu_btn;
static int hover_bar;     /* 1 logo, 2 clock, 3 power, 4 cpu */

static void layout_bar(void) {
    int cw = gfx_utext_width(&font_bold, clock_text) + 24;
    logo_btn  = (struct rect){ 6, 3, 86, PANEL_H - 6 };
    clock_btn = (struct rect){ (screen->w - cw) / 2, 3, cw, PANEL_H - 6 };
    power_btn = (struct rect){ screen->w - 6 - 34, 3, 34, PANEL_H - 6 };
    cpu_btn   = (struct rect){ power_btn.x - 6 - 104, 3, 104, PANEL_H - 6 };
}

static int cpu_hist[60];
static int ncpu_hist;

static void draw_bar(struct rect clip) {
    if (!rect_intersect(bar_rect(), clip, NULL)) return;
    draw_glass(bar_rect(), 0, theme->bar, 150, 10);
    gfx_fill(screen, 0, PANEL_H - 1, screen->w, 1, gfx_mix(theme->bar, ui(UI_TEXT), 36));
    uint32_t fg = ui(UI_TEXT), dim = ui(UI_DIM), acc = ui(UI_ACCENT);
    struct rect *btns[] = { NULL, &logo_btn, &clock_btn, &power_btn, &cpu_btn };
    if (hover_bar || popup) {
        int h = hover_bar ? hover_bar : popup == POP_POWER ? 3 : popup == POP_CALENDAR ? 2 : 0;
        if (h) gfx_round_a(screen, btns[h]->x, btns[h]->y, btns[h]->w, btns[h]->h, 8, fg, 38);
    }
    int ty = (PANEL_H - font_bold.height) / 2;

    /* the logo: a crescent moon (it's called dream, after all) */
    gfx_circle(screen, 22, PANEL_H / 2, 7, acc);
    gfx_circle(screen, 26, PANEL_H / 2 - 3, 6, gfx_mix(theme->bar, 0x000000, 20));
    gfx_utext(screen, &font_bold, 36, ty, "dream", fg);

    if (focus && !focus->hidden)
        gfx_utext_fit(screen, &font_ui, 112, ty, focus->title, dim, clock_btn.x - 130);

    gfx_utext(screen, &font_bold, clock_btn.x + 12, ty, clock_text, fg);

    /* a little live CPU graph */
    char pct[8];
    snprintf(pct, sizeof pct, "%d%%", cpu_usage());
    gfx_utext(screen, &font_small, cpu_btn.x + 10, (PANEL_H - font_small.height) / 2, "cpu", dim);
    int gx = cpu_btn.x + 36, gw = 36, gy = 9, gh = PANEL_H - 18;
    for (int i = 0; i < ncpu_hist && i < gw / 3; i++) {
        int v = cpu_hist[ncpu_hist - 1 - i], h = v * gh / 100;
        if (h < 1) h = 1;
        gfx_fill(screen, gx + gw - 3 - i * 3, gy + gh - h, 2, h, gfx_mix(dim, acc, 128 + v));
    }
    gfx_utext(screen, &font_small, gx + gw + 6, (PANEL_H - font_small.height) / 2, pct, fg);

    icon_glyph(screen, GLYPH_POWER, power_btn.x + 9, power_btn.y + 4, 16, fg);
}

static void update_clock(void) {
    struct rtc_time t;
    rtc_now(&t);
    char now[40];
    snprintf(now, sizeof now, "%s %u %s  %02u:%02u", rtc_day_names[rtc_weekday(&t)], t.day,
             rtc_month_names[(t.mon + 11) % 12], t.hour, t.min);
    if (strcmp(now, clock_text)) {
        strlcpy(clock_text, now, sizeof clock_text);
        layout_bar();
        damage(bar_rect());
    }
}

/* ---- the dock ---------------------------------------------------------------------------- */

static const struct app *app_for(const struct app_entry *e) {
    for (int i = 0; i < nz; i++)
        if (z[i]->app->icon == e->icon) return z[i]->app;
    return NULL;
}

static struct window *window_for(const struct app_entry *e) {
    for (int i = nz - 1; i >= 0; i--)
        if (z[i]->app->icon == e->icon) return z[i];
    return NULL;
}

static void draw_dock(struct rect clip) {
    gfx_shadow(screen, dock, 22, 26, 8, 110);
    if (!rect_intersect(dock, clip, NULL) && hover_dock < 0) return;
    if (rect_intersect(dock, clip, NULL)) {
        draw_glass(dock, 22, theme->bar, 120, 14);
        gfx_ring(screen, dock.x, dock.y, dock.w, dock.h, 22, ui(UI_TEXT), 40);
    }
    if (dock_sep_x >= 0) gfx_fill(screen, dock_sep_x, dock.y + 16, 1, dock.h - 32, gfx_mix(theme->bar, ui(UI_TEXT), 70));
    for (int i = 0; i < nditems; i++) {
        bool hot = i == hover_dock;
        int y = dock.y + DOCK_PAD - (hot ? 4 : 0);
        icon_app(screen, ditems[i].e->icon, ditems[i].x, y, DOCK_ICON);
        struct window *w = window_for(ditems[i].e);
        if (w && (!w->hidden || w->minimized)) {
            uint32_t c = w == focus && !w->hidden ? ui(UI_ACCENT) : ui(UI_TEXT);
            int dw = w == focus && !w->hidden ? 14 : 5;
            gfx_round_a(screen, ditems[i].x + (DOCK_ICON - dw) / 2, dock.y + dock.h - 7, dw, 4, 2, c, 230);
        }
    }
    if (hover_dock >= 0 && hover_dock < nditems) {          /* the name, in a pill above */
        const char *name = ditems[hover_dock].e->name;
        int tw = gfx_utext_width(&font_bold, name) + 24, th = 28;
        int tx = ditems[hover_dock].x + DOCK_ICON / 2 - tw / 2, ty = dock.y - th - 12;
        gfx_shadow(screen, (struct rect){ tx, ty, tw, th }, 10, 12, 3, 90);
        gfx_round_a(screen, tx, ty, tw, th, 10, gfx_mix(ui(UI_BG), 0x000000, 30), 245);
        gfx_ring(screen, tx, ty, tw, th, 10, ui(UI_TEXT), 40);
        gfx_utext(screen, &font_bold, tx + 12, ty + (th - font_bold.height) / 2, name, ui(UI_TEXT));
    }
}

/* ---- menus ----------------------------------------------------------------------------------- */

struct mitem { const char *label; int glyph; void (*act)(void); };
static const struct mitem *menu;
static int nmenu, hover_item = -1;

#define MENU_W   210
#define MENU_ROW 32

static void request_exit(void);
static void do_lock(void) { wm_lock(); }
static void do_reboot(void) { power_reboot(); }
static void do_poweroff(void) { power_off(); }
static void do_terminal(void) { term_open(); }
static void do_note(void) { notes_open(); }
static void do_settings(void) { settings_open(); }
static void do_about(void) { about_open(); }
static void do_wallpaper(void) {
    const char *cur = config_get("wallpaper", wallpaper_names[0]);
    int i = 0;
    while (i < wallpaper_count && strcmp(wallpaper_names[i], cur)) i++;
    config_set("wallpaper", wallpaper_names[(i + 1) % wallpaper_count]);
    wm_wallpaper_changed();
}

static const struct mitem power_menu[] = {
    { "lock screen", GLYPH_LOCK, do_lock },     { "leave desktop", GLYPH_EXIT, request_exit },
    { "restart", GLYPH_RESTART, do_reboot },    { "shut down", GLYPH_POWER, do_poweroff },
};
static const struct mitem context_menu[] = {
    { "open terminal", GLYPH_PROGRAM, do_terminal }, { "new note", GLYPH_PEN, do_note },
    { "next wallpaper", GLYPH_PALETTE, do_wallpaper }, { "settings", GLYPH_CHIP, do_settings },
    { "about dream", GLYPH_INFO, do_about },
};

static void close_popup(void) {
    if (!popup) return;
    damage((struct rect){ popup_rect.x - 30, popup_rect.y - 30, popup_rect.w + 60, popup_rect.h + 60 });
    damage(bar_rect());
    popup = POP_NONE;
    menu = NULL;
    hover_item = -1;
}

static void open_menu(int kind, const struct mitem *items, int n, int x, int y) {
    close_popup();
    popup = kind;
    menu = items;
    nmenu = n;
    int h = n * MENU_ROW + 12;
    if (x + MENU_W > screen->w - 8) x = screen->w - 8 - MENU_W;
    if (y + h > screen->h - 8) y = screen->h - 8 - h;
    popup_rect = (struct rect){ x, y, MENU_W, h };
    damage((struct rect){ x - 30, y - 30, MENU_W + 60, h + 60 });
}

static void open_calendar(void) {
    close_popup();
    popup = POP_CALENDAR;
    popup_rect = (struct rect){ (screen->w - 300) / 2, PANEL_H + 8, 300, 300 };
    damage((struct rect){ popup_rect.x - 30, popup_rect.y - 30, popup_rect.w + 60, popup_rect.h + 60 });
}

static void popup_card(struct rect r) {
    gfx_shadow(screen, r, 14, 26, 8, 130);
    gfx_round_a(screen, r.x, r.y, r.w, r.h, 14, gfx_mix(ui(UI_BG), 0x000000, 20), 250);
    gfx_ring(screen, r.x, r.y, r.w, r.h, 14, ui(UI_TEXT), 44);
}

static int days_in(int mon, int year) {
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return mon == 2 && leap ? 29 : d[mon - 1];
}

static void draw_calendar(struct rect r) {
    struct rtc_time t;
    rtc_now(&t);
    char head[40];
    static const char *const months[12] = { "january", "february", "march", "april", "may", "june",
        "july", "august", "september", "october", "november", "december" };
    snprintf(head, sizeof head, "%s %u", months[(t.mon + 11) % 12], t.year);
    gfx_utext(screen, &font_title, r.x + 20, r.y + 16, head, ui(UI_TEXT));
    char today[40];
    static const char *const dn[7] = { "sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday" };
    snprintf(today, sizeof today, "today is %s the %u%s", dn[rtc_weekday(&t)], t.day,
             t.day % 10 == 1 && t.day != 11 ? "st" : t.day % 10 == 2 && t.day != 12 ? "nd" : t.day % 10 == 3 && t.day != 13 ? "rd" : "th");
    gfx_utext(screen, &font_ui, r.x + 20, r.y + 44, today, ui(UI_DIM));
    static const char *const wd[7] = { "mo", "tu", "we", "th", "fr", "sa", "su" };
    int cw = (r.w - 40) / 7, gx = r.x + 20, gy = r.y + 80;
    for (int i = 0; i < 7; i++) gfx_utext_center(screen, &font_small, gx + i * cw + cw / 2, gy, wd[i], ui(UI_DIM));
    /* what weekday the 1st falls on (rtc_weekday: 0 = sunday) */
    struct rtc_time first = t;
    first.day = 1;
    int start = (rtc_weekday(&first) + 6) % 7;
    int n = days_in(t.mon, t.year);
    for (int d = 1; d <= n; d++) {
        int cell = start + d - 1, cx = gx + (cell % 7) * cw + cw / 2, cy = gy + 26 + (cell / 7) * 30;
        char num[4];
        snprintf(num, sizeof num, "%d", d);
        bool is_today = d == (int)t.day;
        if (is_today) gfx_circle(screen, cx, cy + 8, 13, ui(UI_ACCENT));
        gfx_utext_center(screen, is_today ? &font_bold : &font_ui, cx, cy, num, is_today ? ui(UI_ON_ACCENT) : ui(UI_TEXT));
    }
}

static void draw_popup(struct rect clip) {
    if (!popup || !rect_intersect((struct rect){ popup_rect.x - 30, popup_rect.y - 30, popup_rect.w + 60,
                                                 popup_rect.h + 60 }, clip, NULL)) return;
    struct rect r = popup_rect;
    popup_card(r);
    if (popup == POP_CALENDAR) { draw_calendar(r); return; }
    for (int i = 0; i < nmenu; i++) {
        int y = r.y + 6 + i * MENU_ROW;
        if (i == hover_item) gfx_round_a(screen, r.x + 6, y, r.w - 12, MENU_ROW, 8, ui(UI_ACCENT), 200);
        uint32_t c = i == hover_item ? ui(UI_ON_ACCENT) : ui(UI_TEXT);
        icon_glyph(screen, menu[i].glyph, r.x + 16, y + 8, 16, i == hover_item ? c : ui(UI_DIM));
        gfx_utext(screen, &font_ui, r.x + 44, y + (MENU_ROW - font_ui.height) / 2, menu[i].label, c);
    }
}

/* ---- notifications ----------------------------------------------------------------------------- */

struct note { char title[40], body[96]; uint64_t born; int anim; bool live; };
static struct note notes[NOTE_MAX];

/* notifications can come from any task, so they wait in a little queue first */
static struct { char title[40], body[96]; } note_q[4];
static volatile unsigned nq_head, nq_tail;

static struct rect note_rect(int i) {
    return (struct rect){ screen->w - NOTE_W - 14, PANEL_H + 12 + i * (NOTE_H + 10), NOTE_W, NOTE_H };
}
static void damage_notes(void) {
    damage((struct rect){ screen->w - NOTE_W - 50, PANEL_H, NOTE_W + 50, NOTE_MAX * (NOTE_H + 10) + 50 });
}

void wm_notify(const char *title, const char *body) {
    if (!active) return;
    uint64_t f = irq_save();
    unsigned next = (nq_head + 1) % 4;
    if (next != nq_tail) {
        strlcpy(note_q[nq_head].title, title, sizeof note_q[0].title);
        strlcpy(note_q[nq_head].body, body ? body : "", sizeof note_q[0].body);
        nq_head = next;
    }
    irq_restore(f);
}

static void take_notes(void) {
    while (nq_tail != nq_head) {
        uint64_t f = irq_save();
        struct note n = { "", "", pit_ticks(), 0, true };
        strlcpy(n.title, note_q[nq_tail].title, sizeof n.title);
        strlcpy(n.body, note_q[nq_tail].body, sizeof n.body);
        nq_tail = (nq_tail + 1) % 4;
        irq_restore(f);
        memmove(&notes[1], &notes[0], sizeof notes[0] * (NOTE_MAX - 1));   /* newest on top */
        notes[0] = n;
        damage_notes();
    }
}

static void draw_notes(struct rect clip) {
    for (int i = 0; i < NOTE_MAX; i++) {
        if (!notes[i].live) continue;
        struct rect r = note_rect(i);
        if (!rect_intersect((struct rect){ r.x - 30, r.y - 30, r.w + 60, r.h + 60 }, clip, NULL)) continue;
        int a = notes[i].anim;
        r.x += (256 - a) * 40 / 256;                  /* slides in from the right */
        gfx_shadow(screen, r, 14, 22, 6, 120 * a / 256);
        gfx_round_a(screen, r.x, r.y, r.w, r.h, 14, gfx_mix(ui(UI_BG), 0x000000, 20), 248 * a / 256);
        if (a < 200) continue;
        gfx_ring(screen, r.x, r.y, r.w, r.h, 14, ui(UI_TEXT), 44);
        gfx_round_a(screen, r.x + 14, r.y + 15, 36, 36, 10, ui(UI_ACCENT), 60);
        icon_glyph(screen, GLYPH_BELL, r.x + 22, r.y + 23, 20, ui(UI_ACCENT));
        gfx_utext_fit(screen, &font_bold, r.x + 64, r.y + 13, notes[i].title, ui(UI_TEXT), r.w - 80);
        gfx_utext_fit(screen, &font_ui, r.x + 64, r.y + 35, notes[i].body, ui(UI_DIM), r.w - 80);
    }
}

static void age_notes(void) {
    uint64_t now = pit_ticks();
    bool changed = false;
    for (int i = 0; i < NOTE_MAX; i++) {
        struct note *n = &notes[i];
        if (!n->live) continue;
        bool dying = now - n->born > NOTE_LIFE * pit_hz();
        int want = dying ? 0 : 256;
        if (n->anim != want) {
            n->anim += dying ? -FADE_STEP : FADE_STEP;
            if (n->anim < 0) n->anim = 0;
            if (n->anim > 256) n->anim = 256;
            changed = true;
        }
        if (dying && n->anim == 0) n->live = false;
    }
    /* close the gaps left by expired cards */
    int j = 0;
    for (int i = 0; i < NOTE_MAX; i++) if (notes[i].live) notes[j++] = notes[i];
    if (j != NOTE_MAX) { for (int i = j; i < NOTE_MAX; i++) notes[i].live = false; }
    if (changed) damage_notes();
}

/* ---- the launcher -------------------------------------------------------------------------------- */

static char query[32];
static int lsel, lmatch[32], nlmatch, lhover = -1;
#define LCOLS 5
#define LCELL 120

static bool matches(const char *name, const char *q) {
    size_t n = strlen(q);
    for (const char *p = name; *p; p++) if (!strncmp(p, q, n)) return true;
    return false;
}

static void filter_launcher(void) {
    nlmatch = 0;
    for (int i = 0; i < app_count && nlmatch < 32; i++)
        if (app_list[i].icon != ICON_APPS && (!query[0] || matches(app_list[i].name, query) || matches(app_list[i].about, query))) lmatch[nlmatch++] = i;
    if (lsel >= nlmatch) lsel = nlmatch - 1;
    if (lsel < 0) lsel = 0;
}

static struct rect launch_cell(int i) {
    int cols = nlmatch < LCOLS ? nlmatch : LCOLS;
    int gw = cols * LCELL, x0 = (screen->w - gw) / 2, y0 = screen->h / 2 - 120;
    return (struct rect){ x0 + (i % LCOLS) * LCELL, y0 + (i / LCOLS) * (LCELL + 10), LCELL, LCELL + 4 };
}

static void compose(struct rect r);

static void snapshot_backdrop(bool from_wallpaper, int darken) {
    if (!backdrop) return;
    for (int i = 0; i < ndmg; i++) compose(dmg[i]);   /* make sure the screen is current first */
    ndmg = 0;
    struct surface *src = from_wallpaper ? wall : screen;
    memcpy(backdrop->px, src->px, (size_t)screen->w * screen->h * 4);
    gfx_blur(backdrop, (struct rect){ 0, 0, screen->w, screen->h }, from_wallpaper ? 24 : 16);
    gfx_blend(backdrop, 0, 0, screen->w, screen->h, theme->bg_bottom, darken);
}

static void open_launcher(void) {
    close_popup();
    if (launcher) return;
    snapshot_backdrop(false, 110);
    launcher = true;
    query[0] = 0;
    lsel = 0;
    lhover = -1;
    filter_launcher();
    damage_all();
}

void launcher_open(void) { open_launcher(); }

static void close_launcher(void) {
    if (!launcher) return;
    launcher = false;
    damage_all();
}

static void launch(int i) {
    if (i < 0 || i >= nlmatch) return;
    close_launcher();
    app_list[lmatch[i]].open();
}

static void draw_launcher(void) {
    int fw = 460, fx = (screen->w - fw) / 2, fy = screen->h / 2 - 210;
    gfx_utext_center(screen, &font_title, screen->w / 2, fy - 54, "what would you like to open?", ui(UI_TEXT));
    gfx_round_a(screen, fx, fy, fw, 46, 23, gfx_mix(ui(UI_BG), 0x000000, 20), 230);
    gfx_ring(screen, fx, fy, fw, 46, 23, ui(UI_TEXT), 60);
    icon_glyph(screen, GLYPH_SEARCH, fx + 18, fy + 14, 18, ui(UI_DIM));
    int tx = fx + 48, ty = fy + (46 - font_title.height) / 2;
    if (query[0]) tx = gfx_utext(screen, &font_title, tx, ty, query, ui(UI_TEXT));
    else gfx_utext(screen, &font_title, tx, ty, "type to search", gfx_mix(ui(UI_DIM), ui(UI_BG), 60));
    gfx_fill(screen, query[0] ? tx + 2 : fx + 47, fy + 12, 2, 22, ui(UI_ACCENT));

    for (int i = 0; i < nlmatch; i++) {
        struct rect c = launch_cell(i);
        const struct app_entry *e = &app_list[lmatch[i]];
        if (i == lsel || i == lhover)
            gfx_round_a(screen, c.x + 6, c.y, c.w - 12, c.h, 18, ui(UI_TEXT), i == lsel ? 40 : 22);
        icon_app(screen, e->icon, c.x + (c.w - 64) / 2, c.y + 12, 64);
        gfx_utext_center(screen, &font_bold, c.x + c.w / 2, c.y + 86, e->name, ui(UI_TEXT));
    }
    if (!nlmatch) gfx_utext_center(screen, &font_ui, screen->w / 2, screen->h / 2 - 60, "nothing matches that", ui(UI_DIM));
    gfx_utext_center(screen, &font_small, screen->w / 2, screen->h - 50,
                     "enter opens   arrows choose   esc closes", ui(UI_DIM));
}

/* ---- the lock screen ----------------------------------------------------------------------------- */

static char pw[32];
static int lock_err;          /* ticks left showing "wrong password" */
static int lock_sec = -1;

void wm_lock(void) { if (active) wm_post(WM_REQ_LOCK); }

static void do_lock_now(void) {
    close_popup();
    close_launcher();
    snapshot_backdrop(true, 70);
    locked = true;
    pw[0] = 0;
    lock_err = 0;
    damage_all();
}

static void unlock_try(void) {
    if (passwd_set() && !passwd_check(pw)) {
        pw[0] = 0;
        lock_err = 30;
        damage_all();
        return;
    }
    locked = false;
    pw[0] = 0;
    damage_all();
}

static void draw_lock(void) {
    struct rtc_time t;
    rtc_now(&t);
    char time[8], date[48];
    snprintf(time, sizeof time, "%02u:%02u", t.hour, t.min);
    static const char *const months[12] = { "january", "february", "march", "april", "may", "june",
        "july", "august", "september", "october", "november", "december" };
    static const char *const days[7] = { "sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday" };
    snprintf(date, sizeof date, "%s, %u %s", days[rtc_weekday(&t)], t.day, months[(t.mon + 11) % 12]);
    int cy = screen->h / 2 - 170;
    gfx_utext_center(screen, &font_display, screen->w / 2, cy, time, ui(UI_TEXT));
    gfx_utext_center(screen, &font_title, screen->w / 2, cy + 120, date, gfx_mix(ui(UI_TEXT), ui(UI_DIM), 100));

    int ay = screen->h / 2 + 50;
    gfx_circle(screen, screen->w / 2, ay, 34, gfx_mix(ui(UI_ACCENT), theme->bg_bottom, 60));
    char initial[2] = { sh_hostname()[0], 0 };
    if (initial[0] >= 'a' && initial[0] <= 'z') initial[0] -= 32;
    gfx_utext_center(screen, &font_big, screen->w / 2, ay - 22, initial, ui(UI_ON_ACCENT));
    gfx_utext_center(screen, &font_bold, screen->w / 2, ay + 44, sh_hostname(), ui(UI_TEXT));

    int fw = 260, fx = (screen->w - fw) / 2, fy = ay + 76;
    if (passwd_set()) {
        gfx_round_a(screen, fx, fy, fw, 40, 20, 0x000000, 90);
        gfx_ring(screen, fx, fy, fw, 40, 20, lock_err ? ui(UI_ERR) : ui(UI_TEXT), lock_err ? 200 : 70);
        int n = (int)strlen(pw);
        if (!n) gfx_utext_center(screen, &font_ui, screen->w / 2, fy + 11, "password", ui(UI_DIM));
        for (int i = 0; i < n && i < 16; i++)
            gfx_circle(screen, screen->w / 2 - (n - 1) * 8 + i * 16, fy + 20, 4, ui(UI_TEXT));
        if (lock_err) gfx_utext_center(screen, &font_ui, screen->w / 2, fy + 52, "that's not it. try again.", ui(UI_ERR));
    } else {
        gfx_utext_center(screen, &font_ui, screen->w / 2, fy + 10, "press enter or click to unlock", ui(UI_DIM));
    }
}

/* ---- alt+tab --------------------------------------------------------------------------------------- */

static bool switching;
static int sw_sel;
static struct window *sw_list[MAX_WIN];
static int nsw;

static void build_switch_list(void) {
    nsw = 0;
    for (int i = nz - 1; i >= 0; i--)
        if (!z[i]->hidden || z[i]->minimized) sw_list[nsw++] = z[i];
    int w = nsw * 110 + 40, h = 170;
    switcher_rect = (struct rect){ (screen->w - w) / 2, (screen->h - h) / 2, w, h };
}

static void draw_switcher(struct rect clip) {
    if (!switching || !rect_intersect(switcher_rect, clip, NULL)) return;
    struct rect r = switcher_rect;
    gfx_shadow(screen, r, 22, 30, 10, 150);
    draw_glass(r, 22, theme->bar, 140, 14);
    gfx_ring(screen, r.x, r.y, r.w, r.h, 22, ui(UI_TEXT), 50);
    for (int i = 0; i < nsw; i++) {
        int x = r.x + 20 + i * 110;
        if (i == sw_sel) gfx_round_a(screen, x + 4, r.y + 18, 102, 102, 18, ui(UI_TEXT), 44);
        icon_app(screen, sw_list[i]->app->icon ? sw_list[i]->app->icon : ICON_ABOUT, x + 23, r.y + 37, 64);
    }
    if (sw_sel < nsw)
        gfx_utext_fit(screen, &font_bold, r.x + 20, r.y + 134, sw_list[sw_sel]->title, ui(UI_TEXT), r.w - 40);
}

/* ---- the pointer ------------------------------------------------------------------------------------ */

static void draw_cursor(void) {
    if (cursor_kind == CUR_ARROW) { gfx_cursor(screen, mx, my); return; }
    /* a double-headed arrow for resizing, drawn twice: dark outline, light middle */
    int dx = 0, dy = 0;
    if (cursor_kind == CUR_EW) dx = 8;
    else if (cursor_kind == CUR_NS) dy = 8;
    else if (cursor_kind == CUR_NWSE) { dx = 6; dy = 6; }
    else { dx = 6; dy = -6; }
    for (int pass = 0; pass < 2; pass++) {
        uint32_t c = pass ? 0xf4f4f8 : 0x101014;
        int t = pass ? 2 : 4;
        gfx_line(screen, mx - dx, my - dy, mx + dx, my + dy, t, c);
        for (int s = -1; s <= 1; s += 2) {
            int ex = mx + s * dx, ey = my + s * dy;
            /* the two strokes of each arrowhead, at right angles to each other */
            gfx_line(screen, ex, ey, ex - s * (dx + dy) / 2, ey - s * (dy - dx) / 2, t, c);
            gfx_line(screen, ex, ey, ex - s * (dx - dy) / 2, ey - s * (dy + dx) / 2, t, c);
        }
    }
}

/* ---- putting a frame together ------------------------------------------------------------------------- */

static struct rect snap_rect(void) {
    struct rect a = work_area();
    if (snap == SNAP_LEFT)  return (struct rect){ a.x + 8, a.y + 8, a.w / 2 - 12, a.h - 16 };
    if (snap == SNAP_RIGHT) return (struct rect){ a.x + a.w / 2 + 4, a.y + 8, a.w / 2 - 12, a.h - 16 };
    return (struct rect){ a.x + 8, a.y + 8, a.w - 16, a.h - 16 };
}

static void compose(struct rect r) {
    gfx_clip(screen, &r);
    if (locked || launcher) {
        gfx_blit(screen, r.x, r.y, backdrop, r.x, r.y, r.w, r.h);
        if (locked) draw_lock(); else { draw_launcher(); draw_notes(r); }
        draw_cursor();
        gfx_clip(screen, NULL);
        fb_present_rect(r);
        return;
    }
    gfx_blit(screen, r.x, r.y, wall, r.x, r.y, r.w, r.h);
    for (int i = 0; i < nz; i++)
        if ((!z[i]->hidden || z[i]->anim_step < 0) && rect_intersect(outer_of(z[i]), r, NULL)) draw_window(z[i], r);
    if (snap) {
        struct rect s = snap_rect();
        gfx_round_a(screen, s.x, s.y, s.w, s.h, 16, ui(UI_ACCENT), 50);
        gfx_ring(screen, s.x, s.y, s.w, s.h, 16, ui(UI_ACCENT), 160);
    }
    draw_bar(r);
    draw_dock(r);
    draw_popup(r);
    draw_switcher(r);
    draw_notes(r);
    draw_cursor();
    gfx_clip(screen, NULL);
    fb_present_rect(r);
}

/* ---- other tasks asking the desktop to do things ------------------------------------------- */

struct mutex wm_surface_lock;
static struct task *desktop;

struct job { void (*fn)(void *); void *arg; volatile bool done, ok; };
static struct job *jobs[16];
static volatile unsigned jq_head, jq_tail;
static volatile bool stopping, stopped;

bool wm_call(void (*fn)(void *), void *arg) {
    if (!active) return false;
    if (task_current() == desktop) { fn(arg); return true; }   /* we're already the desktop */
    struct job j = { fn, arg, false, false };
    uint64_t f = irq_save();
    unsigned next = (jq_head + 1) % 16;
    bool queued = next != jq_tail && !stopping;
    if (queued) { jobs[jq_head] = &j; jq_head = next; }
    irq_restore(f);
    if (!queued) return false;
    while (!j.done) task_sleep(5);                /* the desktop runs it within a frame */
    return j.ok;
}

static void run_jobs(bool really) {
    while (jq_tail != jq_head) {
        struct job *j = jobs[jq_tail];
        jq_tail = (jq_tail + 1) % 16;
        if (really) j->fn(j->arg);
        j->ok = really;
        j->done = true;
    }
}

/* ---- window bookkeeping ------------------------------------------------------------------------------- */

static int index_of(struct window *w) {
    for (int i = 0; i < nz; i++) if (z[i] == w) return i;
    return -1;
}

static void set_focus(struct window *w) {
    if (focus == w) return;
    if (focus) damage(outer_of(focus));
    focus = w;
    if (w) damage(outer_of(w));
    damage(bar_rect());
    damage_dock();
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
    for (int i = nz - 1; i >= 0; i--) {
        struct rect f = frame_of(z[i]);
        if (z[i]->min_w && !z[i]->maximized) f = (struct rect){ f.x - EDGE, f.y, f.w + 2 * EDGE, f.h + EDGE };
        if (!z[i]->hidden && rect_contains(f, x, y)) return z[i];
    }
    return NULL;
}

static void start_fade(struct window *w, int step, int then) {
    w->anim_step = step;
    w->anim_then = then;
    damage(outer_of(w));
}

struct window *wm_open(const struct app *app, const char *title, int w, int h, void *data) {
    if (!active || nz == MAX_WIN) return NULL;
    struct window *win = kzalloc(sizeof *win);
    if (!win) return NULL;
    win->surf = surface_new(w, h);
    if (!win->surf) { kfree(win); return NULL; }
    gfx_fill(win->surf, 0, 0, w, h, ui(UI_BG));
    strlcpy(win->title, title, sizeof win->title);
    win->app = app;
    win->data = data;
    win->w = w;
    win->h = h;

    static int cascade;                         /* each new window a little further along */
    int step = (cascade++ % 5) * 30;
    struct rect a = work_area();
    win->x = (screen->w - w) / 2 - 60 + step;
    win->y = a.y + 30 + step;
    if (win->x + w > screen->w - 8) win->x = screen->w - 8 - w;
    if (win->x < 8) win->x = 8;
    if (win->y + h + TITLE_H > a.y + a.h) win->y = a.y + a.h - h - TITLE_H;
    if (win->y < PANEL_H + 6) win->y = PANEL_H + 6;
    win->restore = (struct rect){ win->x, win->y, w, h };

    z[nz++] = win;
    set_focus(win);
    start_fade(win, FADE_STEP, THEN_NOTHING);
    relayout_dock();
    return win;
}

void wm_close(struct window *w) {
    int i = index_of(w);
    if (i < 0) return;
    damage(outer_of(w));
    memmove(&z[i], &z[i + 1], sizeof z[0] * (nz - 1 - i));
    nz--;
    if (drag == w) drag = NULL;
    if (switching) { switching = false; damage(switcher_rect); }  /* it might be in the list */
    if (sizing == w) sizing = NULL;
    if (pressed == w) pressed = NULL;
    if (hover_win == w) hover_win = NULL;
    if (focus == w) { focus = NULL; set_focus(topmost_visible()); }
    mutex_lock(&wm_surface_lock);              /* a program might be drawing into it */
    surface_free(w->surf);
    w->surf = NULL;
    mutex_unlock(&wm_surface_lock);
    kfree(w);
    relayout_dock();
    damage(bar_rect());
}

void wm_show(struct window *w) {
    bool was_hidden = w->hidden;
    w->hidden = false;
    w->minimized = false;
    raise(w);
    set_focus(w);
    if (was_hidden) { w->anim = 0; start_fade(w, FADE_STEP, THEN_NOTHING); }
    damage(outer_of(w));
    relayout_dock();
}

void wm_move(struct window *w, int x, int y) {
    if (y < PANEL_H) y = PANEL_H;
    if (y > screen->h - TITLE_H) y = screen->h - TITLE_H;
    if (x < 80 - w->w) x = 80 - w->w;               /* keep a bit of it grabbable */
    if (x > screen->w - 80) x = screen->w - 80;
    if (x == w->x && y == w->y) return;
    damage(outer_of(w));
    w->x = x;
    w->y = y;
    damage(outer_of(w));
}

void wm_resizable(struct window *w, int min_w, int min_h) { w->min_w = min_w; w->min_h = min_h; }

void wm_set_title(struct window *w, const char *title) {
    if (!strcmp(w->title, title)) return;
    strlcpy(w->title, title, sizeof w->title);
    damage(title_of(w));
    if (w == focus) damage(bar_rect());
}

struct window *wm_find_icon(int icon) {
    for (int i = nz - 1; i >= 0; i--) if (z[i]->app->icon == icon) return z[i];
    return NULL;
}

struct window *wm_find(const struct app *app) {
    for (int i = 0; i < nz; i++) if (z[i]->app == app) return z[i];
    return NULL;
}

/* Give a window a new frame rectangle (x, y, contents width, contents height). */
static void place(struct window *w, struct rect r) {
    if (r.w < w->min_w) r.w = w->min_w;
    if (r.h < w->min_h) r.h = w->min_h;
    if (r.w != w->w || r.h != w->h) {
        struct surface *s = surface_new(r.w, r.h);
        if (!s) return;                         /* out of memory: keep the old size */
        gfx_fill(s, 0, 0, r.w, r.h, ui(UI_BG));
        damage(outer_of(w));
        mutex_lock(&wm_surface_lock);           /* a program might be drawing into it */
        struct surface *old = w->surf;
        uint64_t f = irq_save();                /* the shell might be drawing into it */
        w->surf = s;
        w->w = r.w;
        w->h = r.h;
        irq_restore(f);
        if (w->app->resize) w->app->resize(w);
        else if (w->app->redraw) w->app->redraw(w);
        surface_free(old);
        mutex_unlock(&wm_surface_lock);
    }
    damage(outer_of(w));
    w->x = r.x;
    w->y = r.y;
    damage(outer_of(w));
}

static void toggle_max(struct window *w) {
    if (!w->min_w) return;
    if (w->maximized) {
        w->maximized = false;
        place(w, w->restore);
    } else {
        w->restore = (struct rect){ w->x, w->y, w->w, w->h };
        w->maximized = true;
        struct rect a = work_area();
        place(w, (struct rect){ a.x, a.y, a.w, a.h - TITLE_H });
    }
}

static void minimize(struct window *w) {
    start_fade(w, -FADE_STEP, THEN_MINIMIZE);
    if (focus == w) {
        focus = NULL;
        for (int i = nz - 1; i >= 0; i--) if (!z[i]->hidden && z[i] != w) { set_focus(z[i]); break; }
    }
}

static void close_clicked(struct window *w) {
    bool destroy = !w->app->close || w->app->close(w);
    start_fade(w, -FADE_STEP, destroy ? THEN_CLOSE : THEN_HIDE);
    if (focus == w) {
        focus = NULL;
        for (int i = nz - 1; i >= 0; i--) if (!z[i]->hidden && z[i] != w) { set_focus(z[i]); break; }
    }
}

static void animate(void) {
    for (int i = 0; i < nz; i++) {
        struct window *w = z[i];
        if (!w->anim_step) continue;
        w->anim += w->anim_step;
        damage(outer_of(w));
        if (w->anim >= 256) { w->anim = 256; w->anim_step = 0; }
        if (w->anim <= 0) {
            w->anim = 0;
            w->anim_step = 0;
            if (w->anim_then == THEN_CLOSE) { wm_close(w); i--; continue; }
            w->hidden = true;
            w->minimized = w->anim_then == THEN_MINIMIZE;
            w->anim = 256;
            relayout_dock();
            damage(bar_rect());
        }
    }
}

/* ---- input ---------------------------------------------------------------------------------------------- */

static int title_button_at(struct window *w, int x, int y) {
    if (y < w->y || y >= w->y + TITLE_H) return BTN_NONE;
    for (int b = BTN_MIN; b <= BTN_CLOSE; b++) {
        if (b == BTN_MAX && !w->min_w) continue;
        int dx = x - btn_x(w, b), dy = y - (w->y + TITLE_H / 2);
        if (dx * dx + dy * dy <= 13 * 13) return b;
    }
    return BTN_NONE;
}

static int edges_at(struct window *w, int x, int y) {
    if (!w->min_w || w->maximized) return 0;
    struct rect f = frame_of(w);
    int e = 0;
    if (x < f.x + EDGE / 2) e |= 1;
    if (x >= f.x + f.w - EDGE / 2) e |= 2;
    if (y >= f.y + f.h - EDGE / 2) e |= 8;
    return e;
}

static int cursor_for(int e) {
    if ((e & 8) && (e & 2)) return CUR_NWSE;
    if ((e & 8) && (e & 1)) return CUR_NESW;
    if (e & 8) return CUR_NS;
    return e ? CUR_EW : CUR_ARROW;
}

static int menu_item_at(int x, int y) {
    if (!menu || !rect_contains(popup_rect, x, y)) return -1;
    int i = (y - popup_rect.y - 6) / MENU_ROW;
    return i >= 0 && i < nmenu ? i : -1;
}

static int dock_item_at(int x, int y) {
    if (!rect_contains(dock, x, y)) return -1;
    for (int i = 0; i < nditems; i++)
        if (x >= ditems[i].x - DOCK_GAP / 2 && x < ditems[i].x + DOCK_ICON + DOCK_GAP / 2) return i;
    return -1;
}

static void update_hover(void) {
    int hb = 0;
    if (my < PANEL_H) {
        if (rect_contains(logo_btn, mx, my)) hb = 1;
        else if (rect_contains(clock_btn, mx, my)) hb = 2;
        else if (rect_contains(power_btn, mx, my)) hb = 3;
        else if (rect_contains(cpu_btn, mx, my)) hb = 4;
    }
    if (hb != hover_bar) { hover_bar = hb; damage(bar_rect()); }

    int hi = menu_item_at(mx, my);
    if (hi != hover_item) { hover_item = hi; damage(popup_rect); }

    int hd = popup || drag || sizing ? -1 : dock_item_at(mx, my);
    if (hd != hover_dock) { damage_dock(); hover_dock = hd; damage_dock(); }

    struct window *w = drag || sizing || hd >= 0 || hb ? NULL : window_at(mx, my);
    int btn = w ? title_button_at(w, mx, my) : BTN_NONE;
    if (w != hover_win || btn != hover_btn) {
        if (hover_win) damage(title_of(hover_win));
        if (w) damage(title_of(w));
        hover_win = w;
        hover_btn = btn;
    }
    int ck = sizing ? cursor_for(size_edges) : (w && !btn ? cursor_for(edges_at(w, mx, my)) : CUR_ARROW);
    if (ck != cursor_kind) { cursor_kind = ck; damage(cursor_rect()); }
}

static void dock_click(int i) {
    const struct app_entry *e = ditems[i].e;
    struct window *w = window_for(e);
    if (!w) { e->open(); return; }
    if (w->minimized || w->hidden) { wm_show(w); return; }
    if (w == focus && w == topmost_visible()) { minimize(w); return; }
    raise(w);
    set_focus(w);
}

static uint64_t last_title_click;
static struct window *last_title_win;

static void launcher_mouse(uint8_t down) {
    int h = -1;
    for (int i = 0; i < nlmatch; i++) if (rect_contains(launch_cell(i), mx, my)) h = i;
    if (h != lhover) { lhover = h; damage_all(); }
    if (down & MOUSE_LEFT) { if (h >= 0) launch(h); else close_launcher(); }
}

static void handle_mouse(struct mouse_event *e) {
    if (e->x != mx || e->y != my) {
        damage(cursor_rect());
        mx = e->x;
        my = e->y;
        damage(cursor_rect());
    }
    uint8_t down = e->buttons & ~mbuttons;
    mbuttons = e->buttons;

    if (locked) { if (down & MOUSE_LEFT && !passwd_set()) unlock_try(); return; }
    if (launcher) { launcher_mouse(down); return; }
    update_hover();

    if (sizing) {
        if (!(e->buttons & MOUSE_LEFT)) { sizing = NULL; size_pending = false; update_hover(); return; }
        struct rect r = size_start;
        int dx = mx - size_mx, dy = my - size_my;
        if (size_edges & 2) r.w += dx;
        if (size_edges & 8) r.h += dy;
        if (size_edges & 1) {
            r.w -= dx;
            if (r.w < sizing->min_w) r.w = sizing->min_w;
            r.x = size_start.x + size_start.w - r.w;
        }
        pend_w = r.w; pend_h = r.h; size_pending = true;   /* applied once per frame, in pump() */
        sizing->restore = r;
        return;
    }
    if (drag) {
        if (e->buttons & MOUSE_LEFT) {
            if (drag->maximized) {                  /* pulling a maximized window loose */
                drag->maximized = false;
                int nx = mx - drag->restore.w / 2;
                place(drag, (struct rect){ nx, drag->y, drag->restore.w, drag->restore.h });
                drag_dx = mx - drag->x;
            }
            wm_move(drag, mx - drag_dx, my - drag_dy);
            int s = !drag->min_w ? SNAP_NONE : mx <= 2 ? SNAP_LEFT : mx >= screen->w - 3 ? SNAP_RIGHT
                  : my <= PANEL_H + 1 ? SNAP_MAX : SNAP_NONE;
            if (s != snap) { if (snap) damage(snap_rect()); snap = s; if (snap) damage(snap_rect()); }
        } else {
            if (snap) {
                damage(snap_rect());
                if (snap == SNAP_MAX) toggle_max(drag);
                else {
                    drag->restore = (struct rect){ drag->x, drag->y, drag->w, drag->h };
                    struct rect s = snap_rect();
                    place(drag, (struct rect){ s.x, s.y, s.w, s.h - TITLE_H });
                    if (drag->x + drag->w > screen->w - 8)        /* wider than half (its minimum size) */
                        wm_move(drag, screen->w - 8 - drag->w, drag->y);
                }
                snap = SNAP_NONE;
            }
            drag = NULL;
        }
        return;
    }

    if (down) {
        if (popup) {
            int item = menu_item_at(mx, my);
            bool inside = rect_contains(popup_rect, mx, my);
            if (item >= 0 && (down & MOUSE_LEFT)) {
                void (*act)(void) = menu[item].act;
                close_popup();
                act();
                return;
            }
            if (!inside) {
                bool was_power = popup == POP_POWER, was_cal = popup == POP_CALENDAR;
                close_popup();
                if ((was_power && hover_bar == 3) || (was_cal && hover_bar == 2)) return;   /* toggled off */
            } else return;
        }
        if (my < PANEL_H) {
            if (!(down & MOUSE_LEFT)) return;
            if (hover_bar == 1) open_launcher();
            else if (hover_bar == 2) open_calendar();
            else if (hover_bar == 3) open_menu(POP_POWER, power_menu, 4, power_btn.x + power_btn.w - MENU_W, PANEL_H + 6);
            else if (hover_bar == 4) monitor_open();
            return;
        }
        if (hover_dock >= 0) { if (down & MOUSE_LEFT) dock_click(hover_dock); return; }
        struct window *w = window_at(mx, my);
        if (!w) {
            set_focus(NULL);
            if (down & MOUSE_RIGHT) open_menu(POP_CONTEXT, context_menu, 5, mx, my);
            return;
        }
        raise(w);
        set_focus(w);
        int edges = edges_at(w, mx, my);
        if ((down & MOUSE_LEFT) && edges) {
            sizing = w;
            size_edges = edges;
            size_start = (struct rect){ w->x, w->y, w->w, w->h };
            size_mx = mx;
            size_my = my;
            return;
        }
        if (my < w->y + TITLE_H) {
            if (!(down & MOUSE_LEFT)) return;
            int b = title_button_at(w, mx, my);
            if (b == BTN_CLOSE) { close_clicked(w); return; }
            if (b == BTN_MAX) { toggle_max(w); return; }
            if (b == BTN_MIN) { minimize(w); return; }
            uint64_t now = pit_ticks();
            if (last_title_win == w && now - last_title_click < pit_hz() * 2 / 5) {   /* double-click */
                toggle_max(w);
                last_title_win = NULL;
                return;
            }
            last_title_click = now;
            last_title_win = w;
            drag = w;
            drag_dx = mx - w->x;
            drag_dy = my - w->y;
            return;
        }
        pressed = w;                            /* it gets the mouse until the button is let go */
    }

    /* pass it on to the app: the one we pressed in, or whatever is under the pointer */
    struct window *t = pressed ? pressed : window_at(mx, my);
    if (t && t->app->mouse && (pressed || (my >= t->y + TITLE_H && !edges_at(t, mx, my) && hover_dock < 0)))
        t->app->mouse(t, mx - t->x, my - t->y - TITLE_H, e->buttons, e->wheel);
    if (!e->buttons) pressed = NULL;
}

static void cycle_windows(void) {
    if (!switching) {
        build_switch_list();
        if (nsw < 2) return;
        switching = true;
        sw_sel = 1;
    } else {
        sw_sel = (sw_sel + 1) % nsw;
    }
    damage((struct rect){ switcher_rect.x - 40, switcher_rect.y - 40, switcher_rect.w + 80, switcher_rect.h + 80 });
}

static void handle_key(int k) {
    if (locked) {
        size_t n = strlen(pw);
        if (k == '\n') unlock_try();
        else if (k == '\b') { if (n) pw[n - 1] = 0; }
        else if (k == 27) pw[0] = 0;
        else if (k >= 32 && k < 127 && n < sizeof pw - 1) { pw[n] = (char)k; pw[n + 1] = 0; }
        damage_all();
        return;
    }
    /* super opens the launcher; so does f1, for hosts that keep the super key for themselves */
    if (k == KEY_SUPER || k == KEY_F1) { if (launcher) close_launcher(); else open_launcher(); return; }
    if (launcher) {
        size_t n = strlen(query);
        if (k == 27) close_launcher();
        else if (k == '\n') launch(lsel);
        else if (k == KEY_RIGHT || k == '\t') lsel = (lsel + 1) % (nlmatch ? nlmatch : 1);
        else if (k == KEY_LEFT) lsel = lsel > 0 ? lsel - 1 : 0;
        else if (k == KEY_DOWN) { if (lsel + LCOLS < nlmatch) lsel += LCOLS; }
        else if (k == KEY_UP) { if (lsel >= LCOLS) lsel -= LCOLS; }
        else if (k == '\b') { if (n) query[n - 1] = 0; lsel = 0; filter_launcher(); }
        else if (k >= 32 && k < 127 && n < sizeof query - 1) { query[n] = (char)k; query[n + 1] = 0; lsel = 0; filter_launcher(); }
        if (launcher) damage_all();
        return;
    }
    if (k == KEY_ALTTAB) { cycle_windows(); return; }
    if (k == KEY_ALTF4) { if (focus) close_clicked(focus); return; }
    if (k == KEY_F1 + 10) { if (focus) toggle_max(focus); return; }     /* F11 */
    if (kbd_alt() && k == 20) { term_open(); return; }                  /* ctrl+alt+t */
    if (popup && k == 27) { close_popup(); return; }
    if (focus && !focus->hidden && focus->app->key) focus->app->key(focus, k);
}

/* The power menu's "leave desktop" types `exit` into the terminal, as if you had. */
static void request_exit(void) {
    if (sh_at_prompt()) term_inject("\003exit\n");
    else wm_notify("the terminal is busy", "finish what's running there first");
}

/* ---- the desktop task ----------------------------------------------------------------------------------- */

static volatile uint8_t requests[16];
static volatile unsigned rq_head, rq_tail;
static char open_q[4][24];
static volatile unsigned oq_head, oq_tail;

void wm_post(int r) {
    uint64_t f = irq_save();
    unsigned next = (rq_head + 1) % 16;
    if (next != rq_tail) { requests[rq_head] = r; rq_head = next; }
    irq_restore(f);
}

void wm_open_app(const char *name) {
    if (!active) return;
    uint64_t f = irq_save();
    unsigned next = (oq_head + 1) % 4;
    if (next != oq_tail) { strlcpy(open_q[oq_head], name, sizeof open_q[0]); oq_head = next; }
    irq_restore(f);
    wm_post(WM_REQ_OPEN);
}

static void rebuild_wallpaper(void) {
    if (!wall) return;
    wallpaper_draw(wall, config_get("wallpaper", wallpaper_names[0]));
}

static void rebuild_for_theme(void) {
    rebuild_wallpaper();
    for (int i = 0; i < nz; i++) {
        gfx_fill(z[i]->surf, 0, 0, z[i]->w, z[i]->h, ui(UI_BG));
        if (z[i]->app->redraw) z[i]->app->redraw(z[i]);
    }
    if (locked) snapshot_backdrop(true, 70);
    damage_all();
}

static void handle_requests(void) {
    while (rq_tail != rq_head) {
        int r = requests[rq_tail];
        rq_tail = (rq_tail + 1) % 16;
        if (r == WM_REQ_PAINT) paint_open();
        if (r == WM_REQ_FILES) files_open();
        if (r == WM_REQ_THEME) rebuild_for_theme();
        if (r == WM_REQ_WALLPAPER) { rebuild_wallpaper(); damage_all(); }
        if (r == WM_REQ_LOCK)  do_lock_now();
        if (r == WM_REQ_STOP)  stopping = true;
        while (r == WM_REQ_OPEN && oq_tail != oq_head) {
            char name[24];
            strlcpy(name, open_q[oq_tail], sizeof name);
            oq_tail = (oq_tail + 1) % 4;
            const struct app_entry *e = app_by_name(name);
            if (e) e->open();
            else if (name[0] == '/') notes_open_file(name);
        }
    }
}

/* One step: handle input, run app ticks, redraw what changed. */
static void pump(void) {
    struct mouse_event e;
    while (mouse_poll(&e)) handle_mouse(&e);

    int k;
    while ((k = kbd_poll()) >= 0) handle_key(k);
    if (switching && !kbd_alt()) {                 /* alt let go: switch to the chosen one */
        switching = false;
        damage((struct rect){ switcher_rect.x - 40, switcher_rect.y - 40, switcher_rect.w + 80, switcher_rect.h + 80 });
        if (sw_sel < nsw && index_of(sw_list[sw_sel]) >= 0) wm_show(sw_list[sw_sel]);
    }
    if (size_pending && sizing) {
        size_pending = false;
        place(sizing, (struct rect){ sizing->restore.x, sizing->y, pend_w, pend_h });
    }
    take_notes();
    run_jobs(true);
    uwin_pump();
    animate();

    uint64_t t = pit_ticks();
    if (t - last_tick >= pit_hz() / 10) {
        last_tick = t;
        for (int i = 0; i < nz; i++) if (z[i]->app->tick && !z[i]->hidden) z[i]->app->tick(z[i]);
        update_clock();
        age_notes();
        if (lock_err && !--lock_err) damage_all();
    }
    if (t - last_sec_tick >= pit_hz() / 2) {
        last_sec_tick = t;
        cpu_sample();
        if (ncpu_hist == 60) { memmove(cpu_hist, cpu_hist + 1, sizeof cpu_hist[0] * 59); ncpu_hist--; }
        cpu_hist[ncpu_hist++] = cpu_usage();
        damage(cpu_btn);
        struct rtc_time rt;
        rtc_now(&rt);
        if (locked && (int)rt.min != lock_sec) { lock_sec = rt.min; damage_all(); }
    }

    /* windows whose contents changed (the shell draws into the terminal from its
       own task; it just marks damage, and we pick it up here). Programs draw from
       their own tasks too, holding wm_surface_lock for a whole batch; holding it
       here means we only ever see (and show) finished frames, never half of one. */
    mutex_lock(&wm_surface_lock);
    for (int i = 0; i < nz; i++) {
        struct surface *s = z[i]->surf;
        uint64_t f = irq_save();
        struct rect d = s->damage;
        s->damage = (struct rect){ 0, 0, 0, 0 };
        irq_restore(f);
        if (!rect_empty(d) && !z[i]->hidden && !locked && !launcher)
            damage((struct rect){ z[i]->x + d.x, z[i]->y + TITLE_H + d.y, d.w, d.h });
    }

    for (int i = 0; i < ndmg; i++) compose(dmg[i]);
    ndmg = 0;
    mutex_unlock(&wm_surface_lock);
}

static void desktop_task(void *arg) {
    (void)arg;
    while (!stopping) {
        handle_requests();
        if (stopping) break;
        pump();
        task_sleep(10);                   /* ~100 frames a second at most; others get the CPU */
    }
    uwin_desktop_stopping();
    run_jobs(false);                      /* anyone still waiting: the answer is no */
    while (nz) wm_close(z[nz - 1]);
    ndmg = 0;
    stopped = true;
}

/* ---- starting and stopping ------------------------------------------------------------------------------ */

bool wm_active(void) { return active; }

void wm_start(void) {
    if (active) return;
    screen = fb_screen();
    if (!wall) wall = surface_new(screen->w, screen->h);
    if (!glass) glass = surface_new(screen->w, screen->h);
    if (!backdrop) backdrop = surface_new(screen->w, screen->h);
    if (!wall || !backdrop) { kerrorf(C_ERR "startx" C_DIM ": not enough memory for the desktop\n"); return; }
    rebuild_wallpaper();
    nz = 0;
    focus = drag = pressed = sizing = hover_win = NULL;
    popup = POP_NONE;
    locked = launcher = switching = false;
    snap = SNAP_NONE;
    memset(notes, 0, sizeof notes);
    clock_text[0] = 0;
    rq_head = rq_tail = 0;
    stopping = stopped = false;
    struct mouse_event e;
    while (mouse_poll(&e)) {}
    mouse_where(&mx, &my);
    active = true;
    update_clock();
    layout_dock();
    term_open();                          /* the shell's console moves into this window */
    for (int i = 0; i < nz; i++) z[i]->anim = 256, z[i]->anim_step = 0;
    damage_all();
    jq_head = jq_tail = 0;
    if (!(desktop = task_spawn("desktop", desktop_task, NULL))) {
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

void wm_theme_changed(void) { if (active) wm_post(WM_REQ_THEME); }
void wm_wallpaper_changed(void) { if (active) wm_post(WM_REQ_WALLPAPER); }

/* ---- shell commands ---------------------------------------------------------------------------------------- */

SH_CMD(startx) {
    (void)argc; (void)argv;
    if (active) { kprintf(C_DIM "the desktop is already running.\n"); return; }
    wm_start();
    if (!active) return;
    kprintf(C_ACCENT "welcome to the dream desktop.\n" C_DIM
            "the dock is at the bottom; the moon (or the super key) opens everything else.\n"
            "type " C_FG "exit" C_DIM " to go back to text mode.\n\n");
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

SH_CMD(lock) {
    (void)argc; (void)argv;
    if (!active) { kprintf(C_DIM "the lock screen is part of the desktop: " C_FG "startx" C_DIM " first.\n"); return; }
    wm_lock();
}

SH_CMD(notify) {
    if (argc < 2) { kprintf("usage: notify <title> [message]\n"); return; }
    if (!active) { kprintf(C_DIM "notifications show up on the desktop (" C_FG "startx" C_DIM ")\n"); return; }
    wm_notify(argv[1], argc > 2 ? argv[2] : "");
}

SH_CMD(open) {
    if (argc < 2) {
        kprintf("usage: open <app or file>\n" C_DIM "apps:");
        for (int i = 0; i < app_count; i++) kprintf(" " C_FG "%s", app_list[i].name);
        kprintf("\n");
        return;
    }
    const struct app_entry *e = app_by_name(argv[1]);
    char path[200];
    if (!e) {
        int n = fs_lookup(sh_cwd(), argv[1]);
        if (n < 0 || fs_get(n)->dir) { kerrorf(C_ERR "open" C_DIM ": no app or file called " C_FG "%s\n", argv[1]); return; }
        fs_path(n, path, sizeof path);
    }
    if (!active) wm_start();
    wm_open_app(e ? e->name : path);
}

SH_CMD(wallpaper) {
    if (argc < 2) {
        const char *cur = config_get("wallpaper", wallpaper_names[0]);
        for (int i = 0; i < wallpaper_count; i++)
            kprintf("  %s%s\n", !strcmp(cur, wallpaper_names[i]) ? C_ACCENT "* " : C_DIM "  ", wallpaper_names[i]);
        kprintf(C_DIM "usage: wallpaper <name>\n");
        return;
    }
    for (int i = 0; i < wallpaper_count; i++) {
        if (strcmp(argv[1], wallpaper_names[i])) continue;
        config_set("wallpaper", argv[1]);
        wm_wallpaper_changed();
        return;
    }
    kerrorf(C_ERR "wallpaper" C_DIM ": no wallpaper called " C_FG "%s\n", argv[1]);
}
