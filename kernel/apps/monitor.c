/* monitor: what the computer is doing right now. CPU and memory over the last
 * minute, and every task with the share of the CPU it's been using. Programs
 * (ring 3) can be stopped from here; kernel tasks can't. */
#include "gui/wm.h"
#include "sched/task.h"
#include "mm/pmm.h"
#include "drivers/mouse.h"
#include "arch/cpu.h"
#include "arch/io.h"

#define HIST   60
#define CARD_H 150
#define ROW    30

static int cpu_h[HIST], mem_h[HIST], nh;
static struct task_info tasks[32], prev[32];
static int ntasks, nprev, sel = -1, hover_row = -1;
static int pct[32];                   /* each task's CPU share over the last sample */
static uint64_t last_total;
static uint64_t last_sample;
static bool hover_end;
static uint8_t held;

static int mem_pct(void) { return (int)(pmm_used_pages() * 100 / (pmm_total_pages() ? pmm_total_pages() : 1)); }

static void sample(void) {
    cpu_sample();
    if (nh == HIST) { memmove(cpu_h, cpu_h + 1, sizeof cpu_h[0] * (HIST - 1)); memmove(mem_h, mem_h + 1, sizeof mem_h[0] * (HIST - 1)); nh--; }
    cpu_h[nh] = cpu_usage();
    mem_h[nh++] = mem_pct();

    memcpy(prev, tasks, sizeof tasks);
    nprev = ntasks;
    ntasks = task_list(tasks, 32);
    uint64_t total = sched_total_ticks(), dt = total - last_total;
    last_total = total;
    for (int i = 0; i < ntasks; i++) {
        pct[i] = 0;
        for (int j = 0; j < nprev; j++)
            if (prev[j].id == tasks[i].id && dt) pct[i] = (int)((tasks[i].ticks - prev[j].ticks) * 100 / dt);
    }
}

static void graph(struct surface *s, int x, int y, int w, int h, const int *v, int n, uint32_t c) {
    gfx_round(s, x, y, w, h, 8, gfx_mix(ui(UI_BG), 0x000000, 40));
    for (int g = 1; g < 4; g++) gfx_fill(s, x + 6, y + h * g / 4, w - 12, 1, gfx_mix(ui(UI_BG), ui(UI_TEXT), 14));
    if (n < 2) return;
    int px = -1, py = 0;
    for (int i = 0; i < n; i++) {
        int gx = x + 6 + (w - 12) * (HIST - n + i) / (HIST - 1);
        int gy = y + h - 4 - v[i] * (h - 8) / 100;
        /* fill under the line, faintly */
        for (int yy = gy; yy < y + h - 4; yy++) gfx_blend(s, gx, yy, 2, 1, c, 40);
        if (px >= 0) gfx_line(s, px, py, gx, gy, 2, c);
        px = gx; py = gy;
    }
}

static void card(struct surface *s, int x, int w, const char *title, const char *value, const char *sub,
                 const int *hist, uint32_t c) {
    gfx_round(s, x, 14, w, CARD_H, 14, ui(UI_RAISED));
    gfx_utext(s, &font_bold, x + 16, 26, title, ui(UI_DIM));
    gfx_utext(s, &font_big, x + 16, 44, value, ui(UI_TEXT));
    gfx_utext_fit(s, &font_small, x + 16, 90, sub, ui(UI_DIM), w - 32);
    if (hist) graph(s, x + 16, 110, w - 32, CARD_H - 104, hist, nh, c);
}

static struct rect end_btn(struct window *w) { return (struct rect){ w->w - 16 - 110, w->h - 44, 110, 32 }; }

static void draw(struct window *w) {
    struct surface *s = w->surf;
    int W = s->w, H = s->h;
    gfx_fill(s, 0, 0, W, H, ui(UI_BG));
    int cw = (W - 16 * 4) / 3;
    char v[32], sub[64];

    snprintf(v, sizeof v, "%d%%", nh ? cpu_h[nh - 1] : 0);
    card(s, 16, cw, "cpu", v, "one core, 100 Hz scheduler", cpu_h, ui(UI_ACCENT));

    uint64_t used = pmm_used_pages() * PAGE_SIZE >> 20, total = pmm_total_pages() * PAGE_SIZE >> 20;
    snprintf(v, sizeof v, "%lu MiB", used);
    snprintf(sub, sizeof sub, "of %lu MiB in use (%d%%)", total, mem_pct());
    card(s, 32 + cw, cw, "memory", v, sub, mem_h, ui(UI_OK));

    uint64_t up = pit_ticks() / pit_hz();
    snprintf(v, sizeof v, "%luh %02lum", up / 3600, (up / 60) % 60);
    int nuser = 0;
    for (int i = 0; i < ntasks; i++) nuser += tasks[i].user;
    snprintf(sub, sizeof sub, "%d tasks, %d of them programs", ntasks, nuser);
    card(s, 48 + 2 * cw, cw, "uptime", v, sub, NULL, 0);
    /* a little task-share bar in the third card */
    int bx = 48 + 2 * cw + 16, by = 120, bw = cw - 32;
    gfx_round(s, bx, by, bw, 10, 5, gfx_mix(ui(UI_BG), 0x000000, 40));
    int off = 0;
    static const int slot[] = { UI_ACCENT, UI_OK, UI_WARN, UI_ERR, UI_DIM };
    for (int i = 0; i < ntasks; i++) {
        int seg = pct[i] * bw / 100;
        if (seg > 0) { gfx_round(s, bx + off, by, seg, 10, 4, ui(slot[i % 5])); off += seg; }
    }
    gfx_utext(s, &font_small, bx, by + 16, "where the time goes", ui(UI_DIM));

    /* the task table */
    int ty = CARD_H + 30;
    gfx_utext(s, &font_bold, 20, ty, "id", ui(UI_DIM));
    gfx_utext(s, &font_bold, 70, ty, "name", ui(UI_DIM));
    gfx_utext(s, &font_bold, 250, ty, "kind", ui(UI_DIM));
    gfx_utext(s, &font_bold, 350, ty, "state", ui(UI_DIM));
    gfx_utext(s, &font_bold, W - 200, ty, "cpu time", ui(UI_DIM));
    gfx_utext(s, &font_bold, W - 80, ty, "cpu", ui(UI_DIM));
    gfx_fill(s, 16, ty + 24, W - 32, 1, ui(UI_LINE));
    static const char *const states[] = { "ready", "running", "sleeping", "done" };
    int maxrows = (H - ty - 30 - 56) / ROW;
    for (int i = 0; i < ntasks && i < maxrows; i++) {
        int y = ty + 30 + i * ROW;
        if (i == sel) gfx_round(s, 12, y - 2, W - 24, ROW - 2, 8, ui(UI_SEL));
        else if (i == hover_row) gfx_round(s, 12, y - 2, W - 24, ROW - 2, 8, ui(UI_HOVER));
        char t[32];
        snprintf(t, sizeof t, "%d", tasks[i].id);
        gfx_utext(s, &font_ui, 20, y + 4, t, ui(UI_DIM));
        gfx_utext(s, &font_ui, 70, y + 4, tasks[i].name, ui(UI_TEXT));
        gfx_utext(s, &font_ui, 250, y + 4, tasks[i].user ? "program" : "kernel", tasks[i].user ? ui(UI_OK) : ui(UI_DIM));
        gfx_utext(s, &font_ui, 350, y + 4, states[tasks[i].state], ui(UI_DIM));
        uint64_t sec = tasks[i].ticks / pit_hz();
        snprintf(t, sizeof t, "%lu.%02lus", sec, tasks[i].ticks % pit_hz());
        gfx_utext(s, &font_ui, W - 200, y + 4, t, ui(UI_TEXT));
        snprintf(t, sizeof t, "%d%%", pct[i]);
        gfx_round(s, W - 80, y + 9, 40, 6, 3, gfx_mix(ui(UI_BG), 0x000000, 40));
        if (pct[i]) gfx_round(s, W - 80, y + 9, pct[i] * 40 / 100 + 3, 6, 3, ui(UI_ACCENT));
        gfx_utext(s, &font_small, W - 34, y + 5, t, ui(UI_TEXT));
    }

    bool can_end = sel >= 0 && sel < ntasks && tasks[sel].user;
    struct rect b = end_btn(w);
    const char *hint = sel < 0 ? "select a task to see what you can do with it"
                     : can_end ? "this is a user program: it can be stopped"
                               : "kernel tasks keep dream running, so they can't be stopped";
    gfx_utext(s, &font_small, 20, b.y + 9, hint, ui(UI_DIM));
    if (can_end) ui_button(s, b, "end task", hover_end, true);
}

static void mouse(struct window *w, int x, int y, uint8_t buttons, int wheel) {
    (void)wheel;
    bool click = (buttons & MOUSE_LEFT) && !(held & MOUSE_LEFT);
    held = buttons;
    int ty = CARD_H + 30 + 30, r = y >= ty ? (y - ty + 2) / ROW : -1;
    if (r >= ntasks) r = -1;
    bool he = rect_contains(end_btn(w), x, y);
    bool changed = r != hover_row || he != hover_end;
    hover_row = r;
    hover_end = he;
    if (click) {
        if (he && sel >= 0 && tasks[sel].user) { task_kill(tasks[sel].id); wm_notify("task stopped", tasks[sel].name); }
        else sel = r;
        changed = true;
    }
    if (changed) draw(w);
}

static void tick(struct window *w) {
    if (pit_ticks() - last_sample < pit_hz() / 2) return;
    last_sample = pit_ticks();
    sample();
    draw(w);
}

static void redraw(struct window *w) { draw(w); }

const struct app monitor_app = { .name = "monitor", .mouse = mouse, .tick = tick, .redraw = redraw,
                                 .resize = redraw, .icon = ICON_MONITOR };

void monitor_open(void) {
    struct window *w = wm_find(&monitor_app);
    if (w) { wm_show(w); return; }
    w = wm_open(&monitor_app, "monitor", 720, 480, NULL);
    if (!w) return;
    wm_resizable(w, 640, 380);
    last_total = sched_total_ticks();
    ntasks = task_list(tasks, 32);
    sample();
    draw(w);
}
