/* calculator: the four operations, with decimals.
 *
 * Numbers are "fixed point": whole numbers that count millionths, so 2.5 is stored
 * as 2500000. Programs could use floating point now, but fixed point is exact for
 * money-style sums (0.1 + 0.2 really is 0.3 here), and it's a nice trick to know.
 * Adding is just adding; multiplying and dividing need a little care so nothing
 * overflows. */
#include "gui.h"

#define SCALE 1000000LL            /* six decimal places */
#define LIMIT 999999999999LL       /* the biggest whole part we show */
#define W     300
#define H     430
#define DISP  120
#define GAP   8
#define C(slot) COLOR_UI(slot)

static int win;
static int64_t acc, cur;           /* the running total and the number being typed */
static char op;                    /* pending + - * / (0 = none) */
static bool typing, has_dot, error;
static int decimals;               /* digits typed after the dot */
static char history[64];
static int hover = -1, pressed_btn = -1;
static uint8_t held;

static const char *const labels[20] = {
    "C", "+/-", "%", "/",
    "7", "8", "9", "x",
    "4", "5", "6", "-",
    "1", "2", "3", "+",
    "0", ".", "<", "=",
};

static void cell(int i, int *x, int *y, int *w, int *h) {
    *w = (W - 5 * GAP) / 4;
    *h = (H - DISP - 6 * GAP) / 5;
    *x = GAP + (i % 4) * (*w + GAP);
    *y = DISP + GAP + (i / 4) * (*h + GAP);
}

static int cell_at(int px, int py) {
    for (int i = 0; i < 20; i++) {
        int x, y, w, h;
        cell(i, &x, &y, &w, &h);
        if (px >= x && px < x + w && py >= y && py < y + h) return i;
    }
    return -1;
}

/* ---- arithmetic in millionths -------------------------------------------------------------- */

static bool too_big(int64_t v) { return v > LIMIT * SCALE || v < -LIMIT * SCALE; }

static int64_t mul(int64_t a, int64_t b) {
    bool neg = (a < 0) != (b < 0);
    uint64_t x = a < 0 ? -a : a, y = b < 0 ? -b : b;
    uint64_t xi = x / SCALE, xf = x % SCALE;
    /* (xi + xf/S) * y  =  xi*y + xf*y/S, split again so xf*y can't overflow */
    if (xi && y > (uint64_t)INT64_MAX / xi) { error = true; return 0; }
    uint64_t r = xi * y + xf * (y / SCALE) + xf * (y % SCALE) / SCALE;
    return neg ? -(int64_t)r : (int64_t)r;
}

static int64_t divide(int64_t a, int64_t b) {
    if (b == 0) { error = true; return 0; }
    bool neg = (a < 0) != (b < 0);
    uint64_t x = a < 0 ? -a : a, y = b < 0 ? -b : b;
    uint64_t q = x / y, rem = x % y;
    if (q > (uint64_t)INT64_MAX / SCALE) { error = true; return 0; }
    uint64_t r = q * SCALE;
    for (int64_t place = SCALE / 10; place; place /= 10) {   /* long division, one digit at a time */
        if (rem > (uint64_t)INT64_MAX / 10) { error = true; return 0; }
        rem *= 10;
        r += rem / y * place;
        rem %= y;
    }
    return neg ? -(int64_t)r : (int64_t)r;
}

static int64_t apply(int64_t a, char o, int64_t b) {
    switch (o) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return mul(a, b);
    case '/': return divide(a, b);
    }
    return b;
}

static void format(int64_t v, char *out, size_t n) {
    if (error) { strlcpy(out, "error", n); return; }
    bool neg = v < 0;
    uint64_t x = neg ? -v : v;
    char frac[8];
    snprintf(frac, sizeof frac, "%06lu", x % SCALE);
    int f = 6;
    while (f > 0 && frac[f - 1] == '0') f--;            /* drop trailing zeros */
    frac[f] = 0;
    char whole[24], grouped[32];                         /* 1,234,567 */
    snprintf(whole, sizeof whole, "%lu", x / SCALE);
    int wl = (int)strlen(whole), g = 0;
    for (int i = 0; i < wl; i++) {
        if (i && (wl - i) % 3 == 0) grouped[g++] = ',';
        grouped[g++] = whole[i];
    }
    grouped[g] = 0;
    snprintf(out, n, "%s%s%s%s", neg ? "-" : "", grouped, f ? "." : "", frac);
}

/* ---- buttons --------------------------------------------------------------------------------- */

static void clear(void) { acc = cur = 0; op = 0; typing = has_dot = error = false; decimals = 0; history[0] = 0; }

static void digit(int d) {
    if (error) clear();
    if (!typing) { cur = 0; typing = true; has_dot = false; decimals = 0; }
    if (has_dot) {
        if (decimals == 6) return;
        int64_t place = SCALE;
        for (int i = 0; i <= decimals; i++) place /= 10;
        cur += (cur < 0 ? -1 : 1) * d * place;
        decimals++;
    } else {
        if (cur / SCALE > LIMIT / 10) return;
        cur = cur * 10 + (cur < 0 ? -1 : 1) * d * SCALE;
    }
}

static void operator(char o) {
    if (error) return;
    char a[32];
    if (op && typing) acc = apply(acc, op, cur);
    else if (!op) acc = cur;
    if (too_big(acc)) error = true;
    format(acc, a, sizeof a);
    op = o;
    typing = false;
    cur = acc;
    snprintf(history, sizeof history, "%s %c", a, o == '*' ? 'x' : o);
}

static void equals(void) {
    if (!op || error) return;
    char a[32], b[32];
    format(acc, a, sizeof a);
    format(cur, b, sizeof b);
    snprintf(history, sizeof history, "%s %c %s =", a, op == '*' ? 'x' : op, b);
    acc = apply(acc, op, cur);
    if (too_big(acc)) error = true;
    cur = acc;
    op = 0;
    typing = false;
}

static void backspace(void) {
    if (!typing || error) return;
    if (has_dot && decimals) {
        int64_t place = SCALE;
        for (int i = 0; i < decimals; i++) place /= 10;
        cur -= cur % (place * 10);
        decimals--;
    } else if (has_dot) has_dot = false;
    else cur = cur / (10 * SCALE) * SCALE;
}

static void press(int i) {
    const char *l = labels[i];
    if (l[0] >= '0' && l[0] <= '9') digit(l[0] - '0');
    else if (!strcmp(l, "C")) clear();
    else if (!strcmp(l, "+/-")) cur = -cur;
    else if (!strcmp(l, "%")) cur = cur / 100;
    else if (!strcmp(l, ".")) { if (!typing) { digit(0); cur = 0; } has_dot = true; }
    else if (!strcmp(l, "<")) backspace();
    else if (!strcmp(l, "=")) equals();
    else operator(l[0] == 'x' ? '*' : l[0]);
}

/* ---- drawing and input ---------------------------------------------------------------------- */

static void draw(void) {
    draw_fill(0, 0, W, H, C(UI_BG));
    draw_text_fit(FONT_UI, W - 16 - text_width(FONT_UI, history), 18, history, C(UI_DIM), W - 32);
    char shown[40];
    format(cur, shown, sizeof shown);
    if (typing && has_dot && !decimals) strlcpy(shown + strlen(shown), ".", 2);
    int font = text_width(FONT_BIG, shown) < W - 32 ? FONT_BIG : FONT_TITLE;
    int fh = font == FONT_BIG ? 42 : 25;
    draw_text(font, W - 16 - text_width(font, shown), DISP - fh - 14, shown, error ? C(UI_ERR) : C(UI_TEXT));
    for (int i = 0; i < 20; i++) {
        int x, y, w, h;
        cell(i, &x, &y, &w, &h);
        bool isop = i % 4 == 3, istop = i < 3;
        uint32_t bg = isop ? ui(UI_ACCENT) : istop ? mix(ui(UI_RAISED), ui(UI_TEXT), 20) : ui(UI_RAISED);
        if (i == pressed_btn) bg = mix(bg, 0x000000, 60);
        else if (i == hover) bg = mix(bg, 0xffffff, 30);
        if (isop && op == (labels[i][0] == 'x' ? '*' : labels[i][0]) && !typing) bg = mix(bg, 0xffffff, 110);
        draw_round(x, y, w, h, 14, bg);
        draw_text_center(FONT_TITLE, x + w / 2, y + (h - 25) / 2, labels[i], isop ? C(UI_ON_ACCENT) : C(UI_TEXT));
    }
}

static void mouse(int x, int y, uint8_t buttons, int wheel) {
    (void)wheel;
    int h = cell_at(x, y);
    bool down = (buttons & 1) && !(held & 1), up = !(buttons & 1) && (held & 1);
    held = buttons;
    bool changed = h != hover;
    hover = h;
    if (down) { pressed_btn = h; changed = true; }
    if (up) { if (pressed_btn >= 0 && pressed_btn == h) press(h); pressed_btn = -1; changed = true; }
    if (changed) draw();
}

static void key(int k) {
    static const char map[] = "C?%/789*456-123+0.<=";
    if (k == '\n') k = '=';
    if (k == 27 || k == 'c' || k == 'C') k = 'C';
    if (k == '\b') k = '<';
    if (k == 'x' || k == 'X') k = '*';
    for (int i = 0; i < 20; i++) if (map[i] == k) { press(i); break; }
    draw();
}

int main(void) {
    win = win_open("calculator", W, H, ICON_CALC, 0, 0);
    if (win < 0) { puts("the calculator needs the desktop: type startx first.\n"); return 1; }
    clear();
    app_run(win, &(struct app_loop){ .draw = draw, .key = key, .mouse = mouse });
    return 0;
}
