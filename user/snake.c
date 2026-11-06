/* snake: steer with the arrow keys, eat the dots, don't run into yourself.
 * The walls wrap around. Space pauses; enter starts again. Your best score is
 * kept in /etc/dream.conf (config_set), so it survives a reboot. */
#include "gui.h"

#define COLS  24
#define ROWS  17
#define CELL  20
#define TOP   48
#define W     (COLS * CELL)
#define H     (TOP + ROWS * CELL)
#define MAXLEN (COLS * ROWS)
#define C(slot) COLOR_UI(slot)

static int win;
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
    seed ^= (uint32_t)uptime_ms();
    len = 4;
    for (int i = 0; i < len; i++) body[i].x = COLS / 2 - i, body[i].y = ROWS / 2;
    dx = ndx = 1; dy = ndy = 0;
    score = steps = 0;
    over = paused = false;
    place_food();
    char b[64];
    best = config_get("snake_best", b) ? atoi(b) : 0;
}

static void draw(void) {
    uint32_t board = mix(ui(UI_BG), 0x000000, 50);
    draw_fill(0, 0, W, H, C(UI_BG));
    char t[40];
    snprintf(t, sizeof t, "%d", score);
    draw_text(FONT_TITLE, 16, 12, t, C(UI_TEXT));
    draw_text(FONT_UI, 22 + text_width(FONT_TITLE, t), 16, "points", C(UI_DIM));
    snprintf(t, sizeof t, "best %d", best);
    draw_text(FONT_BOLD, W - 16 - text_width(FONT_BOLD, t), 16, t, C(UI_ACCENT));

    draw_fill(0, TOP, W, H - TOP, board);
    uint32_t check = mix(board, ui(UI_TEXT), 6);
    for (int y = 0; y < ROWS; y++)               /* a faint checkerboard */
        for (int x = (y & 1); x < COLS; x += 2)
            draw_fill(x * CELL, TOP + y * CELL, CELL, CELL, check);

    /* food: a glowing dot */
    int cx = fx * CELL + CELL / 2, cy = TOP + fy * CELL + CELL / 2;
    draw_circle(cx, cy, CELL / 2 + 2, mix(board, ui(UI_ERR), 70));
    draw_circle(cx, cy, CELL / 2 - 4, C(UI_ERR));

    /* the snake, fading from head to tail */
    uint32_t ok = ui(UI_OK);
    for (int i = len - 1; i >= 0; i--) {
        uint32_t c = mix(ok, mix(ok, board, 150), i * 256 / (len + 2));
        draw_round(body[i].x * CELL + 2, TOP + body[i].y * CELL + 2, CELL - 4, CELL - 4, 6, c);
        /* bridge to the next segment so it reads as one body (unless it wrapped) */
        if (i + 1 < len) {
            int nx = body[i + 1].x, ny = body[i + 1].y;
            if (nx == body[i].x && (ny - body[i].y == 1 || ny - body[i].y == -1))
                draw_fill(body[i].x * CELL + 2, TOP + (ny > body[i].y ? ny : body[i].y) * CELL - 4, CELL - 4, 8, c);
            if (ny == body[i].y && (nx - body[i].x == 1 || nx - body[i].x == -1))
                draw_fill((nx > body[i].x ? nx : body[i].x) * CELL - 4, TOP + body[i].y * CELL + 2, 8, CELL - 4, c);
        }
    }
    int hx = body[0].x * CELL + CELL / 2, hy = TOP + body[0].y * CELL + CELL / 2;   /* eyes */
    draw_circle(hx + dx * 3 - dy * 4, hy + dy * 3 + dx * 4, 2, board);
    draw_circle(hx + dx * 3 + dy * 4, hy + dy * 3 - dx * 4, 2, board);

    const char *msg = !started ? "press an arrow key to start" : over ? "crashed! press enter to play again"
                    : paused ? "paused. space to carry on" : NULL;
    if (msg) {
        int mw = text_width(FONT_BOLD, msg) + 40;
        draw_round_a((W - mw) / 2, TOP + (H - TOP) / 2 - 22, mw, 44, 22, 0x000000, 170);
        draw_text_center(FONT_BOLD, W / 2, TOP + (H - TOP) / 2 - 9, msg, 0xffffff);
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
            notify(win, "new best score in snake!", b);
        }
        return;
    }
    memmove(&body[1], &body[0], sizeof body[0] * len);
    body[0].x = nx; body[0].y = ny;
    len++;
    if (eat) { score += 10; if (len < MAXLEN) place_food(); }
}

static void tick(void) {
    if (!started || over || paused) return;
    /* faster as you grow: every tick (10/s) once you're long, every other tick at first */
    if (++steps % (len < 12 ? 2 : 1)) return;
    step();
    draw();
}

static void key(int k) {
    int kx = k == KEY_LEFT ? -1 : k == KEY_RIGHT ? 1 : 0, ky = k == KEY_UP ? -1 : k == KEY_DOWN ? 1 : 0;
    if (kx || ky) {
        started = true;
        if (kx != -dx || ky != -dy) { ndx = kx; ndy = ky; }   /* no reversing into yourself */
    }
    if (k == ' ' && !over) paused = !paused;
    if (k == '\n' && over) { reset(); started = true; }
    draw();
}

int main(void) {
    win = win_open("snake", W, H, ICON_SNAKE, 0, 0);
    if (win < 0) { puts("snake needs the desktop: type startx first.\n"); return 1; }
    reset();
    app_run(win, &(struct app_loop){ .draw = draw, .key = key, .tick = tick, .tick_ms = 100 });
    return 0;
}
