/* clock: an analog clock face, read from the RTC chip. */
#include "gui/wm.h"
#include "drivers/rtc.h"
#include "drivers/fb.h"
#include "drivers/theme.h"

/* sin(i * 6 degrees) * 1000, for i = 0..59: one entry per minute mark.
   cos is the same table, a quarter turn (15 steps) further on. */
static const int16_t sin60[60] = {
    0, 105, 208, 309, 407, 500, 588, 669, 743, 809, 866, 914, 951, 978, 995,
    1000, 995, 978, 951, 914, 866, 809, 743, 669, 588, 500, 407, 309, 208, 105,
    0, -105, -208, -309, -407, -500, -588, -669, -743, -809, -866, -914, -951, -978, -995,
    -1000, -995, -978, -951, -914, -866, -809, -743, -669, -588, -500, -407, -309, -208, -105,
};

#define CX 120
#define CY 116
#define R  96

static int last_sec = -1;

/* A point `len` pixels from the centre, pointing at minute mark i (0 = 12 o'clock). */
static void at(int i, int len, int *x, int *y) {
    i %= 60;
    *x = CX + sin60[i] * len / 1000;
    *y = CY - sin60[(i + 15) % 60] * len / 1000;
}

static void hand(struct surface *s, int i, int back, int len, int thick, uint32_t c) {
    int x0, y0, x1, y1;
    at(i + 30, back, &x0, &y0);              /* a short tail behind the centre */
    at(i, len, &x1, &y1);
    gfx_line(s, x0, y0, x1, y1, thick, c);
}

static void draw(struct surface *s) {
    struct rtc_time t;
    rtc_now(&t);
    last_sec = t.sec;
    uint32_t bg = gfx_mix(theme->bar, theme->bg_top, 100);
    gfx_fill(s, 0, 0, s->w, s->h, bg);

    gfx_circle(s, CX, CY, R + 4, gfx_mix(bg, theme->pal[PAL_ACCENT], 110));
    gfx_circle(s, CX, CY, R, gfx_mix(theme->bg_bottom, 0x000000, 40));
    for (int i = 0; i < 60; i++) {
        bool hour = i % 5 == 0;
        int x0, y0, x1, y1;
        at(i, R - (hour ? 14 : 7), &x0, &y0);
        at(i, R - 5, &x1, &y1);
        gfx_line(s, x0, y0, x1, y1, hour ? 3 : 1, hour ? theme->pal[PAL_FG] : theme->pal[PAL_DIM]);
    }
    hand(s, (t.hour % 12) * 5 + t.min / 12, 10, 52, 6, theme->pal[PAL_FG]);
    hand(s, t.min, 12, 78, 4, theme->pal[PAL_FG]);
    hand(s, t.sec, 18, 86, 2, theme->pal[PAL_ACCENT]);
    gfx_circle(s, CX, CY, 6, theme->pal[PAL_ACCENT]);
    gfx_circle(s, CX, CY, 2, bg);

    char line[32];
    snprintf(line, sizeof line, "%02u:%02u:%02u", t.hour, t.min, t.sec);
    gfx_text(s, CX - gfx_text_width(line) / 2, CY + R + 16, line, theme->pal[PAL_FG], -1);
    snprintf(line, sizeof line, "%s %u %s %u", rtc_day_names[rtc_weekday(&t)], t.day,
             rtc_month_names[(t.mon + 11) % 12], t.year);
    gfx_text(s, CX - gfx_text_width(line) / 2, CY + R + 36, line, theme->pal[PAL_DIM], -1);
}

static void tick(struct window *w) {
    struct rtc_time t;
    rtc_now(&t);
    if (t.sec != last_sec) draw(w->surf);
}

static void redraw(struct window *w) { draw(w->surf); }

const struct app clock_app = { "clock", NULL, NULL, tick, redraw, NULL };

void clock_open(void) {
    struct window *w = wm_find(&clock_app);
    if (w) { wm_show(w); return; }
    w = wm_open(&clock_app, "clock", 240, 276, NULL);
    if (!w) return;
    wm_move(w, fb.width - 240 - 48, WM_PANEL_H + 36);
    draw(w->surf);
}
