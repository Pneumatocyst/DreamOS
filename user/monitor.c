/* monitor: what the computer is doing right now. CPU and memory over the last
 * half minute, and every task with the share of the CPU it's been using. Programs
 * (ring 3) can be stopped from here; kernel tasks can't. */
#include "gui.h"

#define HIST   60
#define CARD_H 150
#define ROW    30
#define C(slot) COLOR_UI(slot)

static int win, W = 720, H = 480;
static int cpu_h[HIST], mem_h[HIST], nh;
static struct task_entry tl[32], prev[32];
static int ntasks, nprev, sel = -1, hover_row = -1;
static int pct[32];                   /* each task's CPU share over the last sample */
static uint64_t last_total;
static struct sysinfo si;
static bool hover_end;
static uint8_t held;

static int mem_pct(void) { return si.mem_total ? (int)(si.mem_used * 100 / si.mem_total) : 0; }

static void sample(void) {
    sysinfo(&si);
    if (nh == HIST) { memmove(cpu_h, cpu_h + 1, sizeof cpu_h[0] * (HIST - 1)); memmove(mem_h, mem_h + 1, sizeof mem_h[0] * (HIST - 1)); nh--; }
    cpu_h[nh] = si.cpu;
    mem_h[nh++] = mem_pct();

    memcpy(prev, tl, sizeof tl);
    nprev = ntasks;
    ntasks = tasks(tl, 32);
    uint64_t dt = si.total_ticks - last_total;
    last_total = si.total_ticks;
    for (int i = 0; i < ntasks; i++) {
        pct[i] = 0;
        for (int j = 0; j < nprev; j++)
            if (prev[j].id == tl[i].id && dt) pct[i] = (int)((tl[i].ticks - prev[j].ticks) * 100 / dt);
    }
    if (sel >= ntasks) sel = -1;
}

static void graph(int x, int y, int w, int h, const int *v, int n, uint32_t c) {
    draw_round(x, y, w, h, 8, mix(ui(UI_BG), 0x000000, 40));
    for (int g = 1; g < 4; g++) draw_fill(x + 6, y + h * g / 4, w - 12, 1, mix(ui(UI_BG), ui(UI_TEXT), 14));
    if (n < 2) return;
    int px = -1, py = 0;
    for (int i = 0; i < n; i++) {
        int gx = x + 6 + (w - 12) * (HIST - n + i) / (HIST - 1);
        int gy = y + h - 4 - v[i] * (h - 8) / 100;
        if (px >= 0) {
            draw_round_a(px, gy < py ? gy : py, gx - px + 1, y + h - 4 - (gy < py ? gy : py), 0, c, 40);   /* fill below */
            draw_line(px, py, gx, gy, 2, c);
        }
        px = gx; py = gy;
    }
}

static void card(int x, int w, const char *title, const char *value, const char *sub, const int *hist, uint32_t c) {
    draw_round(x, 14, w, CARD_H, 14, C(UI_RAISED));
    draw_text(FONT_BOLD, x + 16, 26, title, C(UI_DIM));
    draw_text(FONT_BIG, x + 16, 44, value, C(UI_TEXT));
    draw_text_fit(FONT_SMALL, x + 16, 90, sub, C(UI_DIM), w - 32);
    if (hist) graph(x + 16, 110, w - 32, CARD_H - 104, hist, nh, c);
}

static bool in_end_btn(int x, int y) { return x >= W - 126 && x < W - 16 && y >= H - 44 && y < H - 12; }

static void draw(void) {
    draw_fill(0, 0, W, H, C(UI_BG));
    int cw = (W - 16 * 4) / 3;
    char v[32], sub[64];

    snprintf(v, sizeof v, "%d%%", nh ? cpu_h[nh - 1] : 0);
    card(16, cw, "cpu", v, "one core, 100 Hz scheduler", cpu_h, ui(UI_ACCENT));

    snprintf(v, sizeof v, "%lu MiB", si.mem_used >> 20);
    snprintf(sub, sizeof sub, "of %lu MiB in use (%d%%)", si.mem_total >> 20, mem_pct());
    card(32 + cw, cw, "memory", v, sub, mem_h, ui(UI_OK));

    uint64_t up = si.uptime_ms / 1000;
    snprintf(v, sizeof v, "%luh %02lum", up / 3600, (up / 60) % 60);
    int nuser = 0;
    for (int i = 0; i < ntasks; i++) nuser += tl[i].user;
    snprintf(sub, sizeof sub, "%d tasks, %d of them programs", ntasks, nuser);
    card(48 + 2 * cw, cw, "uptime", v, sub, NULL, 0);
    int bx = 48 + 2 * cw + 16, by = 120, bw = cw - 32, off = 0;   /* who's using the CPU */
    draw_round(bx, by, bw, 10, 5, mix(ui(UI_BG), 0x000000, 40));
    static const int slot[] = { UI_ACCENT, UI_OK, UI_WARN, UI_ERR, UI_DIM };
    for (int i = 0; i < ntasks; i++) {
        int seg = pct[i] * bw / 100;
        if (seg > 0) { draw_round(bx + off, by, seg, 10, 4, C(slot[i % 5])); off += seg; }
    }
    draw_text(FONT_SMALL, bx, by + 16, "where the time goes", C(UI_DIM));

    /* the task table */
    int ty = CARD_H + 30;
    draw_text(FONT_BOLD, 20, ty, "id", C(UI_DIM));
    draw_text(FONT_BOLD, 70, ty, "name", C(UI_DIM));
    draw_text(FONT_BOLD, 250, ty, "kind", C(UI_DIM));
    draw_text(FONT_BOLD, 350, ty, "state", C(UI_DIM));
    draw_text(FONT_BOLD, W - 200, ty, "cpu time", C(UI_DIM));
    draw_text(FONT_BOLD, W - 80, ty, "cpu", C(UI_DIM));
    draw_fill(16, ty + 24, W - 32, 1, C(UI_LINE));
    static const char *const states[] = { "ready", "running", "sleeping", "done" };
    int maxrows = (H - ty - 30 - 56) / ROW;
    for (int i = 0; i < ntasks && i < maxrows; i++) {
        int y = ty + 30 + i * ROW;
        if (i == sel) draw_round(12, y - 2, W - 24, ROW - 2, 8, C(UI_SEL));
        else if (i == hover_row) draw_round(12, y - 2, W - 24, ROW - 2, 8, C(UI_HOVER));
        char t[32];
        snprintf(t, sizeof t, "%d", tl[i].id);
        draw_text(FONT_UI, 20, y + 4, t, C(UI_DIM));
        draw_text(FONT_UI, 70, y + 4, tl[i].name, C(UI_TEXT));
        draw_text(FONT_UI, 250, y + 4, tl[i].user ? "program" : "kernel", tl[i].user ? C(UI_OK) : C(UI_DIM));
        draw_text(FONT_UI, 350, y + 4, states[tl[i].state & 3], C(UI_DIM));
        uint64_t hz = si.hz ? si.hz : 100, sec = tl[i].ticks / hz;
        snprintf(t, sizeof t, "%lu.%02lus", sec, (tl[i].ticks % hz) * 100 / hz);
        draw_text(FONT_UI, W - 200, y + 4, t, C(UI_TEXT));
        snprintf(t, sizeof t, "%d%%", pct[i]);
        draw_round(W - 80, y + 9, 40, 6, 3, mix(ui(UI_BG), 0x000000, 40));
        if (pct[i]) draw_round(W - 80, y + 9, pct[i] * 40 / 100 + 3, 6, 3, C(UI_ACCENT));
        draw_text(FONT_SMALL, W - 34, y + 5, t, C(UI_TEXT));
    }

    bool can_end = sel >= 0 && sel < ntasks && tl[sel].user;
    const char *hint = sel < 0 ? "select a task to see what you can do with it"
                     : can_end ? "this is a user program: it can be stopped"
                               : "kernel tasks keep dream running, so they can't be stopped";
    draw_text(FONT_SMALL, 20, H - 35, hint, C(UI_DIM));
    if (can_end) draw_button(W - 126, H - 44, 110, 32, "end task", hover_end, true);
}

static void mouse(int x, int y, uint8_t buttons, int wheel) {
    (void)wheel;
    bool click = (buttons & 1) && !(held & 1);
    held = buttons;
    int ty = CARD_H + 30 + 30, r = y >= ty ? (y - ty + 2) / ROW : -1;
    if (r >= ntasks) r = -1;
    bool he = in_end_btn(x, y);
    bool changed = r != hover_row || he != hover_end;
    hover_row = r;
    hover_end = he;
    if (click) {
        if (he && sel >= 0 && tl[sel].user) {
            if (kill(tl[sel].id) == 0) notify(win, "task stopped", tl[sel].name);
            sel = -1;                         /* the rows are about to shift: select nothing */
        } else sel = r;
        changed = true;
    }
    if (changed) draw();
}

static void tick(void) { sample(); draw(); }
static void resize(int w, int h) { W = w; H = h; draw(); }

int main(void) {
    win = win_open("monitor", W, H, ICON_MONITOR, 640, 380);
    if (win < 0) { puts("the monitor needs the desktop: type startx first.\n"); return 1; }
    sysinfo(&si);
    last_total = si.total_ticks;
    ntasks = tasks(tl, 32);
    sample();
    app_run(win, &(struct app_loop){ .draw = draw, .mouse = mouse, .tick = tick, .resize = resize, .tick_ms = 500 });
    return 0;
}
