/* Icons, drawn from circles, lines and rounded rectangles instead of images.
 * That way they're sharp at any size and pick up the theme's colours.
 *
 * Each one is designed on a 48 x 48 grid; P(n) scales a grid position to the
 * size we're actually drawing at. */
#include "gui/wm.h"
#include "drivers/theme.h"

#define P(n) ((n) * size / 48)

static uint32_t lighter(uint32_t c, int t) { return gfx_mix(c, 0xffffff, t); }
static uint32_t darker(uint32_t c, int t)  { return gfx_mix(c, 0x000000, t); }

/* the rounded tile every app icon sits on, with a soft top light */
static void tile(struct surface *s, int x, int y, int size, uint32_t c) {
    int r = size * 12 / 48;
    gfx_round_ex(s, x, y + P(1), size, size, r, darker(c, 120), darker(c, 120), 90);   /* a touch of depth */
    gfx_round_ex(s, x, y, size, size, r, lighter(c, 40), darker(c, 40), 256);
    gfx_round_ex(s, x + P(2), y + P(1), size - P(4), size / 2, r, 0xffffff, c, 28);
}

static void bars(struct surface *s, int x, int y, int size, uint32_t c) {   /* three text lines */
    for (int i = 0; i < 3; i++) gfx_round(s, x + P(12), y + P(16 + i * 7), P(i == 2 ? 15 : 24), P(3), P(1) + 1, c);
}

void icon_app(struct surface *s, int icon, int x, int y, int size) {
    const uint32_t *pal = theme->pal;
    uint32_t white = 0xf8f8fc, ink = darker(theme->bg_bottom, 40);
    switch (icon) {
    case ICON_TERMINAL:
        tile(s, x, y, size, gfx_mix(theme->bg_bottom, 0x202028, 80));
        gfx_line(s, x + P(12), y + P(17), x + P(20), y + P(24), P(3) + 1, pal[PAL_ACCENT]);
        gfx_line(s, x + P(20), y + P(24), x + P(12), y + P(31), P(3) + 1, pal[PAL_ACCENT]);
        gfx_round(s, x + P(23), y + P(29), P(13), P(3) + 1, P(1) + 1, pal[PAL_FG]);
        break;
    case ICON_FILES:
        tile(s, x, y, size, pal[PAL_ACCENT]);
        gfx_round(s, x + P(9), y + P(13), P(14), P(8), P(2), darker(pal[PAL_ACCENT], 90));
        gfx_round(s, x + P(9), y + P(16), P(30), P(21), P(3), lighter(pal[PAL_ACCENT], 150));
        gfx_round(s, x + P(9), y + P(20), P(30), P(17), P(3), white);
        break;
    case ICON_NOTES:
        tile(s, x, y, size, pal[PAL_WARN]);
        gfx_round(s, x + P(10), y + P(8), P(28), P(32), P(4), white);
        gfx_fill(s, x + P(10), y + P(8), P(28), P(5), darker(pal[PAL_WARN], 30));
        for (int i = 0; i < 3; i++) gfx_round(s, x + P(14), y + P(19 + i * 6), P(i == 2 ? 12 : 20), P(2) + 1, 1, gfx_mix(white, ink, 110));
        break;
    case ICON_CALC: {
        tile(s, x, y, size, gfx_mix(pal[PAL_DIM], 0x303040, 120));
        gfx_round(s, x + P(10), y + P(9), P(28), P(9), P(2), lighter(pal[PAL_OK], 120));
        for (int r = 0; r < 2; r++)
            for (int c = 0; c < 3; c++)
                gfx_round(s, x + P(10 + c * 10), y + P(22 + r * 9), P(8), P(7), P(2),
                          c == 2 && r == 1 ? pal[PAL_WARN] : lighter(pal[PAL_DIM], 90));
        break;
    }
    case ICON_PAINT:
        tile(s, x, y, size, gfx_mix(pal[PAL_ERR], pal[PAL_WARN], 70));
        gfx_circle(s, x + P(22), y + P(23), P(13), white);
        gfx_circle(s, x + P(16), y + P(20), P(3), pal[PAL_ACCENT]);
        gfx_circle(s, x + P(23), y + P(15), P(3), pal[PAL_OK]);
        gfx_circle(s, x + P(29), y + P(21), P(3), pal[PAL_ERR]);
        gfx_line(s, x + P(26), y + P(38), x + P(38), y + P(26), P(4), darker(pal[PAL_WARN], 60));
        break;
    case ICON_MONITOR:
        tile(s, x, y, size, gfx_mix(pal[PAL_OK], 0x103020, 90));
        gfx_line(s, x + P(9), y + P(28), x + P(17), y + P(28), P(3), white);
        gfx_line(s, x + P(17), y + P(28), x + P(22), y + P(16), P(3), white);
        gfx_line(s, x + P(22), y + P(16), x + P(28), y + P(34), P(3), white);
        gfx_line(s, x + P(28), y + P(34), x + P(32), y + P(25), P(3), white);
        gfx_line(s, x + P(32), y + P(25), x + P(39), y + P(25), P(3), white);
        break;
    case ICON_SETTINGS: {
        tile(s, x, y, size, gfx_mix(pal[PAL_DIM], 0x404050, 60));
        static const int16_t d[8][2] = { { 0, -1000 }, { 707, -707 }, { 1000, 0 }, { 707, 707 },
                                         { 0, 1000 }, { -707, 707 }, { -1000, 0 }, { -707, -707 } };
        int cx = x + size / 2, cy = y + size / 2;
        for (int i = 0; i < 8; i++)
            gfx_line(s, cx, cy, cx + d[i][0] * P(14) / 1000, cy + d[i][1] * P(14) / 1000, P(6), white);
        gfx_circle(s, cx, cy, P(11), white);
        gfx_circle(s, cx, cy, P(5), gfx_mix(pal[PAL_DIM], 0x404050, 60));
        break;
    }
    case ICON_CLOCK: {
        tile(s, x, y, size, gfx_mix(theme->bg_bottom, 0x202028, 80));
        int cx = x + size / 2, cy = y + size / 2;
        gfx_circle(s, cx, cy, P(15), white);
        gfx_line(s, cx, cy, cx, cy - P(10), P(3), ink);
        gfx_line(s, cx, cy, cx + P(7), cy + P(4), P(3), ink);
        gfx_circle(s, cx, cy, P(2) + 1, pal[PAL_ERR]);
        break;
    }
    case ICON_SNAKE:
        tile(s, x, y, size, gfx_mix(theme->bg_bottom, pal[PAL_OK], 60));
        gfx_line(s, x + P(12), y + P(33), x + P(24), y + P(33), P(6), pal[PAL_OK]);
        gfx_line(s, x + P(24), y + P(33), x + P(24), y + P(18), P(6), pal[PAL_OK]);
        gfx_line(s, x + P(24), y + P(18), x + P(33), y + P(18), P(6), lighter(pal[PAL_OK], 60));
        gfx_circle(s, x + P(36), y + P(32), P(4), pal[PAL_ERR]);
        break;
    case ICON_ABOUT:
        tile(s, x, y, size, gfx_mix(pal[PAL_ACCENT], 0x101020, 100));
        gfx_circle(s, x + P(24), y + P(24), P(12), white);
        gfx_circle(s, x + P(29), y + P(19), P(10), gfx_mix(pal[PAL_ACCENT], 0x101020, 70));
        break;
    case ICON_APPS:
        tile(s, x, y, size, gfx_mix(theme->bg_bottom, pal[PAL_FG], 50));
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                gfx_circle(s, x + P(14 + c * 10), y + P(14 + r * 10), P(3) + 1, white);
        break;
    default:
        tile(s, x, y, size, pal[PAL_DIM]);
        bars(s, x, y, size, white);
    }
}

/* sin of minute mark i (6 degrees each), times 1000. cos(i) = sin(i + 15). */
static const int16_t sin60[60] = {
    0, 105, 208, 309, 407, 500, 588, 669, 743, 809, 866, 914, 951, 978, 995,
    1000, 995, 978, 951, 914, 866, 809, 743, 669, 588, 500, 407, 309, 208, 105,
    0, -105, -208, -309, -407, -500, -588, -669, -743, -809, -866, -914, -951, -978, -995,
    -1000, -995, -978, -951, -914, -866, -809, -743, -669, -588, -500, -407, -309, -208, -105,
};

/* An arc from minute mark `from` to `to` (0 = 12 o'clock, clockwise), as short lines. */
static void arc(struct surface *s, int cx, int cy, int r, int from, int to, int t, uint32_t c) {
    for (int i = from; i < to; i++) {
        int a = i % 60, b = (i + 1) % 60;
        gfx_line(s, cx + sin60[a] * r / 1000, cy - sin60[(a + 15) % 60] * r / 1000,
                 cx + sin60[b] * r / 1000, cy - sin60[(b + 15) % 60] * r / 1000, t, c);
    }
}

/* Small one-colour symbols for buttons and menus. */
void icon_glyph(struct surface *s, int g, int x, int y, int size, uint32_t c) {
    int t = size >= 20 ? 2 : (size >= 14 ? 2 : 1);   /* stroke thickness */
    int cx = x + size / 2, cy = y + size / 2;
    switch (g) {
    case GLYPH_FOLDER:
        gfx_round(s, x, y + P(6), P(20), P(10), P(4), c);
        gfx_round(s, x, y + P(12), size, P(32), P(5), c);
        break;
    case GLYPH_FILE:
        gfx_round(s, x + P(8), y + P(2), P(32), P(44), P(5), c);
        break;
    case GLYPH_PROGRAM:
        gfx_ring(s, x, y + P(4), size, P(40), P(7), c, 256);
        gfx_line(s, x + P(10), y + P(16), x + P(18), y + P(24), t, c);
        gfx_line(s, x + P(18), y + P(24), x + P(10), y + P(32), t, c);
        gfx_line(s, x + P(23), y + P(32), x + P(36), y + P(32), t, c);
        break;
    case GLYPH_UP:
        gfx_line(s, cx, y + P(8), cx, y + P(42), t + 1, c);
        gfx_line(s, cx, y + P(8), cx - P(14), y + P(22), t + 1, c);
        gfx_line(s, cx, y + P(8), cx + P(14), y + P(22), t + 1, c);
        break;
    case GLYPH_HOME:
        gfx_line(s, x + P(4), y + P(24), cx, y + P(6), t + 1, c);
        gfx_line(s, cx, y + P(6), x + P(44), y + P(24), t + 1, c);
        gfx_ring(s, x + P(10), y + P(22), P(28), P(22), P(4), c, 256);
        break;
    case GLYPH_PLUS:
    case GLYPH_NEW:
        gfx_line(s, cx, y + P(8), cx, y + P(40), t + 1, c);
        gfx_line(s, x + P(8), cy, x + P(40), cy, t + 1, c);
        break;
    case GLYPH_TRASH:
        gfx_line(s, x + P(6), y + P(12), x + P(42), y + P(12), t, c);
        gfx_line(s, x + P(18), y + P(6), x + P(30), y + P(6), t, c);
        gfx_ring(s, x + P(11), y + P(14), P(26), P(30), P(5), c, 256);
        break;
    case GLYPH_PEN:
        gfx_line(s, x + P(10), y + P(38), x + P(36), y + P(12), t + 2, c);
        gfx_line(s, x + P(6), y + P(42), x + P(10), y + P(38), t, c);
        break;
    case GLYPH_POWER:
        gfx_line(s, cx, y + P(4), cx, y + P(22), t, c);
        arc(s, cx, cy + P(3), P(17), 7, 53, t, c);           /* a ring with a gap at the top */
        break;
    case GLYPH_SEARCH:
        gfx_ring(s, x + P(4), y + P(4), P(30), P(30), P(15), c, 256);
        gfx_ring(s, x + P(5), y + P(5), P(28), P(28), P(14), c, 256);
        gfx_line(s, x + P(30), y + P(30), x + P(44), y + P(44), t + 1, c);
        break;
    case GLYPH_LOCK:
        gfx_ring(s, x + P(14), y + P(4), P(20), P(24), P(10), c, 256);
        gfx_ring(s, x + P(15), y + P(5), P(18), P(22), P(9), c, 256);
        gfx_round(s, x + P(8), y + P(20), P(32), P(26), P(5), c);
        break;
    case GLYPH_RESTART:
        arc(s, cx, cy, P(17), 10, 60, t, c);
        gfx_line(s, cx, cy - P(17), cx - P(8), cy - P(24), t, c);   /* the arrowhead */
        gfx_line(s, cx, cy - P(17), cx - P(8), cy - P(10), t, c);
        break;
    case GLYPH_EXIT:
        gfx_ring(s, x + P(4), y + P(4), P(26), P(40), P(6), c, 256);
        gfx_line(s, x + P(20), cy, x + P(44), cy, t, c);
        gfx_line(s, x + P(44), cy, x + P(36), cy - P(8), t, c);
        gfx_line(s, x + P(44), cy, x + P(36), cy + P(8), t, c);
        break;
    case GLYPH_SAVE:
        gfx_ring(s, x + P(4), y + P(4), P(40), P(40), P(6), c, 256);
        gfx_round(s, x + P(14), y + P(4), P(20), P(12), P(2), c);
        gfx_round(s, x + P(12), y + P(26), P(24), P(18), P(3), c);
        break;
    case GLYPH_OPEN:
        gfx_round(s, x, y + P(6), P(20), P(10), P(4), c);
        gfx_ring(s, x, y + P(12), size, P(32), P(5), c, 256);
        gfx_line(s, cx, y + P(38), cx, y + P(22), t, c);
        gfx_line(s, cx, y + P(22), cx - P(6), y + P(28), t, c);
        gfx_line(s, cx, y + P(22), cx + P(6), y + P(28), t, c);
        break;
    case GLYPH_ROOT:
        gfx_ring(s, x + P(2), y + P(10), P(44), P(28), P(6), c, 256);
        gfx_circle(s, x + P(36), y + P(24), P(3) + 1, c);
        gfx_line(s, x + P(10), y + P(24), x + P(26), y + P(24), t, c);
        break;
    case GLYPH_BIN:
    case GLYPH_CHIP:
        gfx_ring(s, x + P(10), y + P(10), P(28), P(28), P(5), c, 256);
        gfx_round(s, x + P(18), y + P(18), P(12), P(12), P(2), c);
        for (int i = 0; i < 3; i++) {
            int o = P(16 + i * 8);
            gfx_fill(s, x + o, y + P(3), t, P(7), c);  gfx_fill(s, x + o, y + P(38), t, P(7), c);
            gfx_fill(s, x + P(3), y + o, P(7), t, c);  gfx_fill(s, x + P(38), y + o, P(7), t, c);
        }
        break;
    case GLYPH_PALETTE:
        gfx_ring(s, x + P(2), y + P(4), P(44), P(40), P(20), c, 256);
        gfx_circle(s, x + P(14), y + P(18), P(4), c);
        gfx_circle(s, x + P(24), y + P(12), P(4), c);
        gfx_circle(s, x + P(34), y + P(18), P(4), c);
        gfx_circle(s, x + P(30), y + P(32), P(5), c);
        break;
    case GLYPH_INFO:
        gfx_ring(s, x + P(2), y + P(2), P(44), P(44), P(22), c, 256);
        gfx_ring(s, x + P(3), y + P(3), P(42), P(42), P(21), c, 256);
        gfx_circle(s, cx, y + P(14), P(3), c);
        gfx_round(s, cx - P(2) - 1, y + P(21), P(4) + 2, P(16), P(2), c);
        break;
    case GLYPH_BELL:
        gfx_round(s, x + P(10), y + P(8), P(28), P(30), P(13), c);
        gfx_round(s, x + P(4), y + P(30), P(40), P(8), P(4), c);
        gfx_circle(s, cx, y + P(42), P(5), c);
        break;
    }
}
