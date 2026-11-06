/* snake: steer with the arrow keys, eat the dots, don't run into yourself.
 * The walls wrap around. Space pauses; enter starts again. Your best score is
 * kept in /etc/dream.conf, so it survives a reboot. */
#include "gui/wm.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "arch/cpu.h"
#include "arch/io.h"

#define COLS  24
#define ROWS  17
#define CELL  20
#define TOP   48
#define W     (COLS * CELL)
#define H     (TOP + ROWS * CELL)
#define MAXLEN (COLS * ROWS)

static struct { int8_t x, y; } body[MAXLEN];
static int len, dx, dy, ndx, ndy, score, best, fx, fy, steps;
static bool over, paused, started;
static uint32_t seed = 12345;

static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }

static bool on_snake(int x, int y) {
    for (int i = 0; i < len; i++) if (body[i].x == x && body[i].y == y) return true;
    return false;
}

static void place_food(void) {
    do { fx = rnd() % COLS; fy = rnd() % ROWS; } while (on_snake(fx, fy));
}

static void reset(void) {
    seed ^= (uint32_t)pit_ticks();
    len = 4;
    for (int i = 0; i < len; i++) body[i].x = COLS / 2 - i, body[i].y = ROWS / 2;
    dx = ndx = 1; dy = ndy = 0;
    score = steps = 0;
    over = paused = false;
    place_food();
    const char *b = config_get("snake_best", "0");
    best = 0;
    while (*b >= '0' && *b <= '9') best = best * 10 + (*b++ - '0');
}

static void draw(struct surface *s) {
    uint32_t board = gfx_mix(ui(UI_BG), 0x000000, 50);
    gfx_fill(s, 0, 0, W, H, ui(UI_BG));
    char t[40];
    snprintf(t, sizeof t, "%d", score);
    gfx_utext(s, &font_title, 16, 12, t, ui(UI_TEXT));
    gfx_utext(s, &font_ui, 22 + gfx_utext_width(&font_title, t), 16, "points", ui(UI_DIM));
    snprintf(t, sizeof t, "best %d", best);
    gfx_utext(s, &font_bold, W - 16 - gfx_utext_width(&font_bold, t), 16, t, ui(UI_ACCENT));

    gfx_fill(s, 0, TOP, W, H - TOP, board);
    for (int y = 0; y < ROWS; y++)               /* a faint checkerboard */
        for (int x = (y & 1); x < COLS; x += 2)
            gfx_fill(s, x * CELL, TOP + y * CELL, CELL, CELL, gfx_mix(board, ui(UI_TEXT), 6));

    /* food: a glowing dot */
    int cx = fx * CELL + CELL / 2, cy = TOP + fy * CELL + CELL / 2;
    gfx_circle(s, cx, cy, CELL / 2 + 2, gfx_mix(board, ui(UI_ERR), 70));
    gfx_circle(s, cx, cy, CELL / 2 - 4, ui(UI_ERR));

    /* the snake, fading from head to tail */
    for (int i = len - 1; i >= 0; i--) {
        uint32_t c = gfx_mix(ui(UI_OK), gfx_mix(ui(UI_OK), board, 150), i * 256 / (len + 2));
        gfx_round(s, body[i].x * CELL + 2, TOP + body[i].y * CELL + 2, CELL - 4, CELL - 4, 6, c);
        /* bridge to the next segment so it reads as one body (unless it wrapped) */
        if (i + 1 < len) {
            int nx = body[i + 1].x, ny = body[i + 1].y;
            if (nx == body[i].x && (ny - body[i].y == 1 || ny - body[i].y == -1))
                gfx_fill(s, body[i].x * CELL + 2, TOP + (ny > body[i].y ? ny : body[i].y) * CELL - 4, CELL - 4, 8, c);
            if (ny == body[i].y && (nx - body[i].x == 1 || nx - body[i].x == -1))
                gfx_fill(s, (nx > body[i].x ? nx : body[i].x) * CELL - 4, TOP + body[i].y * CELL + 2, 8, CELL - 4, c);
        }
    }
    int hx = body[0].x * CELL + CELL / 2, hy = TOP + body[0].y * CELL + CELL / 2;   /* eyes */
    gfx_circle(s, hx + dx * 3 - dy * 4, hy + dy * 3 + dx * 4, 2, board);
    gfx_circle(s, hx + dx * 3 + dy * 4, hy + dy * 3 - dx * 4, 2, board);

    const char *msg = !started ? "press an arrow key to start" : over ? "crashed! press enter to play again"
                    : paused ? "paused. space to carry on" : NULL;
    if (msg) {
        int mw = gfx_utext_width(&font_bold, msg) + 40;
        gfx_round_a(s, (W - mw) / 2, TOP + (H - TOP) / 2 - 22, mw, 44, 22, 0x000000, 170);
        gfx_utext_center(s, &font_bold, W / 2, TOP + (H - TOP) / 2 - 9, msg, 0xffffff);
    }
}

static void step(void) {
    dx = ndx; dy = ndy;
    int nx = (body[0].x + dx + COLS) % COLS, ny = (body[0].y + dy + ROWS) % ROWS;
    bool eat = nx == fx && ny == fy;
    if (!eat) len--;                               /* the tail moves on (unless we grow) */
    if (on_snake(nx, ny)) {
        if (!eat) len++;
        over = true;
        if (score > best) {
            char b[16];
            snprintf(b, sizeof b, "%d", score);
            config_set("snake_best", b);
            best = score;
            wm_notify("new best score in snake!", b);
        }
        return;
    }
    memmove(&body[1], &body[0], sizeof body[0] * len);
    body[0].x = nx; body[0].y = ny;
    len++;
    if (eat) { score += 10; if (len < MAXLEN) place_food(); }
}

static void tick(struct window *w) {
    if (!started || over || paused) return;
    /* faster as you grow: every tick (10/s) once you're long, every other tick at first */
    if (++steps % (len < 12 ? 2 : 1)) return;
    step();
    draw(w->surf);
}

static void key(struct window *w, int k) {
    int kx = k == KEY_LEFT ? -1 : k == KEY_RIGHT ? 1 : 0, ky = k == KEY_UP ? -1 : k == KEY_DOWN ? 1 : 0;
    if (kx || ky) {
        started = true;
        if (kx != -dx || ky != -dy) { ndx = kx; ndy = ky; }   /* no reversing into yourself */
    }
    if (k == ' ' && !over) paused = !paused;
    if (k == '\n' && over) { reset(); started = true; }
    draw(w->surf);
}

static void redraw(struct window *w) { draw(w->surf); }
static bool close(struct window *w) { (void)w; started = false; return true; }

const struct app snake_app = { .name = "snake", .key = key, .tick = tick, .redraw = redraw, .close = close, .icon = ICON_SNAKE };

void snake_open(void) {
    struct window *w = wm_find(&snake_app);
    if (w) { wm_show(w); return; }
    w = wm_open(&snake_app, "snake", W, H, NULL);
    if (!w) return;
    reset();
    started = false;
    draw(w->surf);
}
