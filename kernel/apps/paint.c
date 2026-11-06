/* paint: draw with the mouse, in a window.
 *
 *   left button   draw          right button   erase
 *   scroll wheel  brush size    [ and ]        brush size too
 *   c             clear         esc            close
 *
 * Strokes are smooth lines (gfx_line) from where the mouse was to where it is,
 * so fast movements don't leave gaps. */
#include "shell/shell.h"
#include "gui/wm.h"
#include "drivers/mouse.h"
#include "drivers/theme.h"

#define TOOLBAR_H 40
#define SWATCH    22
#define NCOLORS   8

static int brush = 4, selected = 2;
static uint32_t colors[NCOLORS];
static uint8_t held;
static int lx, ly;
static struct rect clear_btn;

static uint32_t canvas_bg(void) { return gfx_mix(theme->bg_top, theme->bg_bottom, 128); }

static void pick_colors(void) {
    uint32_t c[NCOLORS] = {
        theme->pal[PAL_FG], theme->pal[PAL_ACCENT], theme->pal[PAL_OK], theme->pal[PAL_WARN],
        theme->pal[PAL_ERR], theme->pal[PAL_DIM], 0xffffff, 0x0c0c10,
    };
    memcpy(colors, c, sizeof colors);
}

static void draw_toolbar(struct surface *s) {
    gfx_fill(s, 0, 0, s->w, TOOLBAR_H, ui(UI_BG));
    gfx_fill(s, 0, TOOLBAR_H - 1, s->w, 1, ui(UI_LINE));
    int sy = (TOOLBAR_H - SWATCH) / 2;
    for (int i = 0; i < NCOLORS; i++) {
        int sx = 14 + i * (SWATCH + 8);
        if (i == selected) gfx_round(s, sx - 3, sy - 3, SWATCH + 6, SWATCH + 6, 8, theme->pal[PAL_FG]);
        gfx_round(s, sx, sy, SWATCH, SWATCH, 6, colors[i]);
    }

    int tx = 14 + NCOLORS * (SWATCH + 8) + 12, ty = (TOOLBAR_H - font_ui.height) / 2;
    char info[32];
    snprintf(info, sizeof info, "brush %2d", brush);
    gfx_utext(s, &font_ui, tx, ty, info, ui(UI_TEXT));
    int r = brush < 12 ? brush : 12;
    gfx_circle(s, tx + 9 * FONT_W + 14, TOOLBAR_H / 2, r, colors[selected]);
    snprintf(info, sizeof info, "%4d, %4d", lx, ly - TOOLBAR_H);
    gfx_utext(s, &font_ui, tx + 9 * FONT_W + 40, ty, info, ui(UI_DIM));

    clear_btn = (struct rect){ s->w - 14 - 64, sy, 64, SWATCH };
    ui_button(s, clear_btn, "clear", false, false);
}

static void clear_canvas(struct surface *s) {
    gfx_fill(s, 0, TOOLBAR_H, s->w, s->h - TOOLBAR_H, canvas_bg());
}

static void stroke(struct surface *s, int x0, int y0, int x1, int y1, uint32_t c) {
    struct rect canvas = { 0, TOOLBAR_H, s->w, s->h - TOOLBAR_H };
    gfx_clip(s, &canvas);                     /* never paint over the toolbar */
    gfx_line(s, x0, y0, x1, y1, brush * 2, c);
    gfx_clip(s, NULL);
}

static void mouse(struct window *w, int x, int y, uint8_t buttons, int wheel) {
    struct surface *s = w->surf;
    if (wheel) {
        brush -= wheel;
        if (brush < 1) brush = 1;
        if (brush > 40) brush = 40;
    }
    bool click = (buttons & MOUSE_LEFT) && !(held & MOUSE_LEFT);
    if (y < TOOLBAR_H) {
        if (click) {
            for (int i = 0; i < NCOLORS; i++) {
                int sx = 14 + i * (SWATCH + 8);
                if (x >= sx - 3 && x < sx + SWATCH + 3) selected = i;
            }
            if (rect_contains(clear_btn, x, y)) clear_canvas(s);
        }
    } else if (buttons & (MOUSE_LEFT | MOUSE_RIGHT)) {
        uint32_t c = (buttons & MOUSE_LEFT) ? colors[selected] : canvas_bg();
        if ((held & (MOUSE_LEFT | MOUSE_RIGHT)) && ly >= TOOLBAR_H) stroke(s, lx, ly, x, y, c);
        else stroke(s, x, y, x, y, c);
    }
    held = buttons;
    lx = x;
    ly = y;
    draw_toolbar(s);
}

static void key(struct window *w, int k) {
    if (k == 27) { wm_close(w); return; }
    if (k == 'c') clear_canvas(w->surf);
    if (k == '[' && brush > 1) brush--;
    if (k == ']' && brush < 40) brush++;
    draw_toolbar(w->surf);
}

static void redraw(struct window *w) { pick_colors(); draw_toolbar(w->surf); }

const struct app paint_app = { .name = "paint", .mouse = mouse, .key = key, .redraw = redraw, .icon = ICON_PAINT };

void paint_open(void) {
    struct window *w = wm_find(&paint_app);
    if (w) { wm_show(w); return; }
    w = wm_open(&paint_app, "paint", 760, 500, NULL);
    if (!w) return;
    wm_move(w, 440, WM_PANEL_H + 150);
    pick_colors();
    held = 0;
    clear_canvas(w->surf);
    draw_toolbar(w->surf);
}

/* `paint` from the shell: open the desktop if needed, then the app. */
SH_CMD(paint) {
    (void)argc; (void)argv;
    if (!mouse_present()) { kerrorf(C_ERR "paint" C_DIM ": no mouse found\n"); return; }
    if (!wm_active()) wm_start();
    wm_post(WM_REQ_PAINT);                /* the desktop task opens it */
}
