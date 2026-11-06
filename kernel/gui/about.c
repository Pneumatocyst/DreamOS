/* about: a little card with the logo and live system info. */
#include "gui/wm.h"
#include "drivers/fb.h"
#include "drivers/theme.h"
#include "arch/cpu.h"
#include "mm/pmm.h"
#include "sched/task.h"

static uint64_t last_sec = -1;

static void row(struct surface *s, int y, const char *label, const char *value) {
    gfx_text(s, 40, y, label, theme->pal[PAL_ACCENT], -1);
    gfx_text(s, 40 + 10 * FONT_W, y, value, theme->pal[PAL_FG], -1);
}

static void draw(struct surface *s) {
    uint32_t bg = gfx_mix(theme->bar, theme->bg_top, 100);
    gfx_fill(s, 0, 0, s->w, s->h, bg);

    /* the logo: a little sun */
    static const int16_t dir[8][2] = {
        { 0, -1000 }, { 707, -707 }, { 1000, 0 }, { 707, 707 },
        { 0, 1000 }, { -707, 707 }, { -1000, 0 }, { -707, -707 },
    };
    int cx = 76, cy = 72;
    for (int i = 0; i < 8; i++)
        gfx_line(s, cx + dir[i][0] * 30 / 1000, cy + dir[i][1] * 30 / 1000,
                 cx + dir[i][0] * 44 / 1000, cy + dir[i][1] * 44 / 1000, 4, theme->pal[PAL_ACCENT]);
    gfx_circle(s, cx, cy, 21, theme->pal[PAL_ACCENT]);
    gfx_circle(s, cx, cy, 13, gfx_mix(theme->pal[PAL_ACCENT], 0xffffff, 120));

    gfx_text_scaled(s, 146, 40, "dream", 3, theme->pal[PAL_FG], 256);
    gfx_text(s, 148, 96, "version " DREAM_VERSION "  x86_64", theme->pal[PAL_DIM], -1);

    char v[48];
    uint64_t up = pit_ticks() / pit_hz();
    last_sec = up;
    int y = 140;
    snprintf(v, sizeof v, "%luh %02lum %02lus", up / 3600, (up / 60) % 60, up % 60);
    row(s, y, "uptime", v); y += 22;
    snprintf(v, sizeof v, "%lu MiB used of %lu MiB", (pmm_used_pages() * PAGE_SIZE) >> 20,
             (pmm_total_pages() * PAGE_SIZE) >> 20);
    row(s, y, "memory", v); y += 22;
    snprintf(v, sizeof v, "%u x %u, double buffered", fb.width, fb.height);
    row(s, y, "display", v); y += 22;
    row(s, y, "theme", theme->name); y += 22;
    struct task_info ti[16];
    snprintf(v, sizeof v, "%d running side by side", task_list(ti, 16));
    row(s, y, "tasks", v); y += 30;

    for (int p = 0; p < PAL_COUNT; p++) gfx_round(s, 40 + p * 34, y, 26, 14, 5, theme->pal[p]);
}

static void tick(struct window *w) {
    if (pit_ticks() / pit_hz() != last_sec) draw(w->surf);
}

static void redraw(struct window *w) { draw(w->surf); }

const struct app about_app = { "about", NULL, NULL, tick, redraw, NULL };

void about_open(void) {
    struct window *w = wm_find(&about_app);
    if (w) { wm_show(w); return; }
    w = wm_open(&about_app, "about dream", 420, 312, NULL);
    if (w) draw(w->surf);
}
