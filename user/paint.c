/* paint: draw with the mouse.
 *
 *   left button   draw          right button   erase
 *   scroll wheel  brush size    [ and ]        brush size too
 *   ctrl+z        undo          c              clear          esc   close
 *
 * Every stroke is remembered as a list of short line segments, so the picture can be
 * drawn again whenever it's needed: when the window is resized, when the theme
 * changes (the colours follow it), and for undo. */
#include "gui.h"

#define TOOLBAR_H 40
#define SWATCH    22
#define NCOLORS   8
#define MAXSEG    40000
#define ERASER    255
#define C(slot) COLOR_UI(slot)

struct seg { int16_t x0, y0, x1, y1; uint8_t size, colour; bool starts_stroke; };
static struct seg segs[MAXSEG];
static int nsegs;

static int win, W = 760, H = 500;
static int brush = 4, selected = 2;
static uint8_t held;
static int lx, ly;

static uint32_t canvas_bg(void);

static uint32_t colour_of(int i) {
    const struct theme_info *t = current_theme();
    uint32_t c[NCOLORS] = { t->pal[0], t->pal[2], t->pal[3], t->pal[4], t->pal[5], t->pal[1], 0xffffff, 0x0c0c10 };
    return i == ERASER ? canvas_bg() : c[i];
}

static uint32_t canvas_bg(void) { return mix(current_theme()->bg_top, current_theme()->bg_bottom, 128); }


static void draw_seg(const struct seg *s) {
    draw_line(s->x0, s->y0 + TOOLBAR_H, s->x1, s->y1 + TOOLBAR_H, s->size * 2, colour_of(s->colour));
}

static void draw_toolbar(void) {
    draw_fill(0, 0, W, TOOLBAR_H, C(UI_BG));
    draw_fill(0, TOOLBAR_H - 1, W, 1, C(UI_LINE));
    int sy = (TOOLBAR_H - SWATCH) / 2;
    for (int i = 0; i < NCOLORS; i++) {
        int sx = 14 + i * (SWATCH + 8);
        if (i == selected) draw_round(sx - 3, sy - 3, SWATCH + 6, SWATCH + 6, 8, C(UI_TEXT));
        draw_round(sx, sy, SWATCH, SWATCH, 6, colour_of(i));
    }
    int tx = 14 + NCOLORS * (SWATCH + 8) + 12, ty = (TOOLBAR_H - 18) / 2;
    char info[32];
    snprintf(info, sizeof info, "brush %2d", brush);
    draw_text(FONT_UI, tx, ty, info, C(UI_TEXT));
    int r = brush < 12 ? brush : 12;
    draw_circle(tx + 9 * 8 + 14, TOOLBAR_H / 2, r, colour_of(selected));
    snprintf(info, sizeof info, "%4d, %4d", lx, ly - TOOLBAR_H);
    draw_text(FONT_UI, tx + 9 * 8 + 40, ty, info, C(UI_DIM));
    draw_text(FONT_SMALL, W - 14 - 64 - 12 - text_width(FONT_SMALL, "ctrl+z undo"), ty + 2, "ctrl+z undo", C(UI_DIM));
    draw_button(W - 14 - 64, sy, 64, SWATCH, "clear", false, false);
}

/* everything, from scratch: the toolbar, a clean canvas, then every stroke again */
static void draw(void) {
    draw_toolbar();
    draw_fill(0, TOOLBAR_H, W, H - TOOLBAR_H, canvas_bg());
    for (int i = 0; i < nsegs; i++) draw_seg(&segs[i]);
}

static void add(int x0, int y0, int x1, int y1, int colour, bool starts) {
    if (nsegs == MAXSEG) return;                    /* full: stop remembering (and drawing) */
    segs[nsegs] = (struct seg){ x0, y0 - TOOLBAR_H, x1, y1 - TOOLBAR_H, brush, colour, starts };
    draw_seg(&segs[nsegs++]);
}

static void undo(void) {
    while (nsegs > 0 && !segs[--nsegs].starts_stroke) {}   /* back to where that stroke began */
    draw();
}

static void clear(void) { nsegs = 0; draw(); }

static void mouse(int x, int y, uint8_t buttons, int wheel) {
    if (wheel) {
        brush -= wheel;
        if (brush < 1) brush = 1;
        if (brush > 40) brush = 40;
    }
    bool click = (buttons & 1) && !(held & 1);
    if (y < TOOLBAR_H) {
        if (click) {
            for (int i = 0; i < NCOLORS; i++) {
                int sx = 14 + i * (SWATCH + 8);
                if (x >= sx - 3 && x < sx + SWATCH + 3) selected = i;
            }
            if (x >= W - 14 - 64 && x < W - 14) clear();
        }
    } else if (buttons & 3) {
        int c = (buttons & 1) ? selected : ERASER;
        if ((held & 3) && ly >= TOOLBAR_H) add(lx, ly, x, y, c, false);
        else add(x, y, x, y, c, true);
    }
    held = buttons;
    lx = x;
    ly = y;
    draw_toolbar();
}

static void key(int k) {
    if (k == 27) exit(0);
    if (k == 26) { undo(); return; }                 /* ctrl+z */
    if (k == 'c') { clear(); return; }
    if (k == '[' && brush > 1) brush--;
    if (k == ']' && brush < 40) brush++;
    draw_toolbar();
}

static void resize(int w, int h) { W = w; H = h; draw(); }

int main(void) {
    win = win_open("paint", W, H, ICON_PAINT, 520, 260);
    if (win < 0) { puts("paint needs the desktop: type startx first.\n"); return 1; }
    app_run(win, &(struct app_loop){ .draw = draw, .key = key, .mouse = mouse, .resize = resize });
    return 0;
}
