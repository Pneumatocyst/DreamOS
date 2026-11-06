/* clock: an analog clock face, read from the computer's clock chip (via sysinfo). */
#include "gui.h"

#define C(slot) COLOR_UI(slot)
#define CX 120
#define CY 116
#define R  96

/* sin(i * 6 degrees) * 1000, for i = 0..59: one entry per minute mark.
   cos is the same table, a quarter turn (15 steps) further on. */
static const int16_t sin60[60] = {
    0, 105, 208, 309, 407, 500, 588, 669, 743, 809, 866, 914, 951, 978, 995,
    1000, 995, 978, 951, 914, 866, 809, 743, 669, 588, 500, 407, 309, 208, 105,
    0, -105, -208, -309, -407, -500, -588, -669, -743, -809, -866, -914, -951, -978, -995,
    -1000, -995, -978, -951, -914, -866, -809, -743, -669, -588, -500, -407, -309, -208, -105,
};
static const char *const days[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const months[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static int last_sec = -1;

/* A point `len` pixels from the centre, pointing at minute mark i (0 = 12 o'clock). */
static void at(int i, int len, int *x, int *y) {
    i %= 60;
    *x = CX + sin60[i] * len / 1000;
    *y = CY - sin60[(i + 15) % 60] * len / 1000;
}

static void hand(int i, int back, int len, int thick, uint32_t c) {
    int x0, y0, x1, y1;
    at(i + 30, back, &x0, &y0);              /* a short tail behind the centre */
    at(i, len, &x1, &y1);
    draw_line(x0, y0, x1, y1, thick, c);
}

static void draw(void) {
    struct sysinfo t;
    sysinfo(&t);
    last_sec = t.sec;
    uint32_t bg = ui(UI_BG), acc = ui(UI_ACCENT), fg = ui(UI_TEXT), dim = ui(UI_DIM);
    draw_fill(0, 0, 240, 290, bg);
    draw_circle(CX, CY, R + 4, mix(bg, acc, 110));
    draw_circle(CX, CY, R, mix(current_theme()->bg_bottom, 0x000000, 40));
    for (int i = 0; i < 60; i++) {
        bool hour = i % 5 == 0;
        int x0, y0, x1, y1;
        at(i, R - (hour ? 14 : 7), &x0, &y0);
        at(i, R - 5, &x1, &y1);
        draw_line(x0, y0, x1, y1, hour ? 3 : 1, hour ? fg : dim);
    }
    hand((t.hour % 12) * 5 + t.min / 12, 10, 52, 6, fg);
    hand(t.min, 12, 78, 4, fg);
    hand(t.sec, 18, 86, 2, acc);
    draw_circle(CX, CY, 6, acc);
    draw_circle(CX, CY, 2, bg);

    char line[32];
    snprintf(line, sizeof line, "%02u:%02u:%02u", t.hour, t.min, t.sec);
    draw_text_center(FONT_TITLE, CX, CY + R + 14, line, fg);
    snprintf(line, sizeof line, "%s %u %s %u", days[t.weekday], t.day, months[(t.mon + 11) % 12], t.year);
    draw_text_center(FONT_UI, CX, CY + R + 42, line, dim);
}

static void tick(void) {
    struct sysinfo t;
    sysinfo(&t);
    if (t.sec != last_sec) draw();
}

int main(void) {
    int win = win_open("clock", 240, 290, ICON_CLOCK, 0, 0);
    if (win < 0) { puts("the clock needs the desktop: type startx first.\n"); return 1; }
    app_run(win, &(struct app_loop){ .draw = draw, .tick = tick, .tick_ms = 100 });
    return 0;
}
