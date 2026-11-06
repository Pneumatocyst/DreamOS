/* settings: themes and wallpapers, desktop behaviour, the machine's name and
 * password, and an "about" page. Choices are written to /etc/dream.conf
 * (see sys/config.c), so they're still there after a reboot. */
#include "gui/wm.h"
#include "drivers/theme.h"
#include "drivers/fb.h"
#include "drivers/mouse.h"
#include "drivers/keyboard.h"
#include "fs/ramfs.h"
#include "fs/diskfs.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "sched/task.h"
#include "arch/cpu.h"
#include "arch/io.h"
#include "shell/shell.h"

#define W        760
#define H        500
#define SIDE     196
#define CX       (SIDE + 30)
#define NPAGES   4

enum { PAGE_LOOK, PAGE_DESKTOP, PAGE_SYSTEM, PAGE_ABOUT };
enum { F_NONE, F_HOST, F_PW };

static int page, hover_page = -1, focus_field;
static char host[32], pw[32];
static struct surface *thumbs[8];
static struct rect hot[24];            /* clickable things on the current page */
static int nhot, hover_hot = -1;
static uint8_t held;
static char status[64];

static const char *const page_names[NPAGES] = { "appearance", "desktop", "system", "about" };
static const int page_glyphs[NPAGES] = { GLYPH_PALETTE, GLYPH_HOME, GLYPH_CHIP, GLYPH_INFO };

static int add_hot(struct rect r) { if (nhot < 24) hot[nhot] = r; return nhot++; }

static void heading(struct surface *s, int y, const char *t) { gfx_utext(s, &font_title, CX, y, t, ui(UI_TEXT)); }

/* ---- appearance ------------------------------------------------------------------------------ */

static void theme_card(struct surface *s, int x, int y, const struct theme *t, bool chosen, bool hov) {
    int w = 116, h = 80;
    if (chosen) gfx_round(s, x - 4, y - 4, w + 8, h + 8, 16, ui(UI_ACCENT));
    else if (hov) gfx_round(s, x - 4, y - 4, w + 8, h + 8, 16, ui(UI_HOVER));
    gfx_round_ex(s, x, y, w, h, 12, t->bg_top, t->bg_bottom, 256);
    gfx_round(s, x + 14, y + 14, 70, 44, 6, gfx_mix(t->bar, t->bg_top, 90));     /* a tiny window */
    gfx_round(s, x + 14, y + 14, 70, 10, 4, t->bar);
    gfx_round(s, x + 20, y + 30, 40, 4, 2, t->pal[PAL_FG]);
    gfx_round(s, x + 20, y + 38, 28, 4, 2, t->pal[PAL_ACCENT]);
    gfx_round(s, x + 20, y + 46, 34, 4, 2, t->pal[PAL_OK]);
    gfx_circle(s, x + w - 16, y + h - 16, 7, t->pal[PAL_ACCENT]);
    gfx_utext_center(s, chosen ? &font_bold : &font_ui, x + w / 2, y + h + 10, t->name, chosen ? ui(UI_TEXT) : ui(UI_DIM));
}

static void make_thumbs(void) {
    for (int i = 0; i < wallpaper_count; i++) {
        if (!thumbs[i]) thumbs[i] = surface_new(88, 56);
        if (thumbs[i]) wallpaper_draw(thumbs[i], wallpaper_names[i]);
    }
}

static void page_look(struct surface *s) {
    heading(s, 26, "theme");
    gfx_utext(s, &font_ui, CX, 54, "colours for everything: the desktop, the terminal and every app.", ui(UI_DIM));
    for (int i = 0; i < theme_count; i++) {
        int x = CX + i * 132, y = 84;
        int id = add_hot((struct rect){ x - 4, y - 4, 124, 112 });
        theme_card(s, x, y, &themes[i], &themes[i] == theme, hover_hot == id);
    }
    heading(s, 238, "wallpaper");
    const char *cur = config_get("wallpaper", wallpaper_names[0]);
    for (int i = 0; i < wallpaper_count; i++) {
        int x = CX + i * 100, y = 276;
        int id = add_hot((struct rect){ x - 4, y - 4, 96, 86 });
        bool chosen = !strcmp(cur, wallpaper_names[i]);
        uint32_t edge = chosen ? ui(UI_ACCENT) : hover_hot == id ? ui(UI_HOVER) : ui(UI_BG);
        if (chosen || hover_hot == id) gfx_round(s, x - 4, y - 4, 96, 64, 12, edge);
        if (thumbs[i]) {
            gfx_blit(s, x, y, thumbs[i], 0, 0, 88, 56);
            /* round off the thumbnail's corners by painting the frame colour back over them */
            gfx_ring(s, x - 1, y - 1, 90, 58, 9, edge, 256);
            gfx_ring(s, x - 2, y - 2, 92, 60, 10, edge, 256);
        }
        gfx_utext_center(s, chosen ? &font_bold : &font_ui, x + 44, y + 64, wallpaper_names[i], chosen ? ui(UI_TEXT) : ui(UI_DIM));
    }
    gfx_utext(s, &font_small, CX, H - 40, "tip: right-click the desktop to flip through wallpapers.", ui(UI_DIM));
}

/* ---- desktop --------------------------------------------------------------------------------- */

static void page_desktop(struct surface *s) {
    heading(s, 26, "starting up");
    bool on = strcmp(config_get("autostart", "yes"), "no");
    gfx_round(s, CX, 64, W - CX - 30, 60, 12, ui(UI_RAISED));
    gfx_utext(s, &font_bold, CX + 16, 74, "open the desktop when dream starts", ui(UI_TEXT));
    gfx_utext(s, &font_small, CX + 16, 96, "off: you get the text-mode shell, and can type startx later", ui(UI_DIM));
    int id = add_hot((struct rect){ W - 30 - 60, 82, 44, 24 });
    (void)id;
    ui_toggle(s, W - 30 - 58, 83, on);

    heading(s, 150, "keyboard shortcuts");
    static const char *const keys[][2] = {
        { "super", "open the launcher (type to search)" },
        { "alt + tab", "switch between windows" },
        { "alt + f4", "close the window" },
        { "f11", "maximize or restore the window" },
        { "ctrl + alt + t", "open the terminal" },
        { "right-click", "the desktop menu" },
        { "double-click", "a title bar to maximize" },
        { "drag to an edge", "snap a window to half the screen" },
    };
    for (int i = 0; i < 8; i++) {
        int y = 190 + i * 32;
        int kw = gfx_utext_width(&font_bold, keys[i][0]) + 20;
        gfx_round(s, CX, y, kw, 24, 7, ui(UI_RAISED));
        gfx_utext(s, &font_bold, CX + 10, y + 3, keys[i][0], ui(UI_TEXT));
        gfx_utext(s, &font_ui, CX + 170, y + 3, keys[i][1], ui(UI_DIM));
    }
}

/* ---- system ---------------------------------------------------------------------------------- */

static void page_system(struct surface *s) {
    heading(s, 26, "this computer's name");
    gfx_utext(s, &font_ui, CX, 54, "shown in the prompt and on the lock screen. press enter to save.", ui(UI_DIM));
    int id = add_hot((struct rect){ CX, 80, 280, 36 });
    (void)id;
    ui_field(s, (struct rect){ CX, 80, 280, 36 }, focus_field == F_HOST ? host : sh_hostname(), NULL, focus_field == F_HOST, false);

    heading(s, 140, "password");
    gfx_utext(s, &font_ui, CX, 168, passwd_set() ? "the lock screen asks for a password." : "no password: the lock screen opens with a click.", ui(UI_DIM));
    add_hot((struct rect){ CX, 194, 280, 36 });
    ui_field(s, (struct rect){ CX, 194, 280, 36 }, pw, "new password", focus_field == F_PW, true);
    int b1 = add_hot((struct rect){ CX + 292, 194, 90, 36 });
    ui_button(s, hot[b1], "set", hover_hot == b1, true);
    if (passwd_set()) {
        int b2 = add_hot((struct rect){ CX + 392, 194, 100, 36 });
        ui_button(s, hot[b2], "remove", hover_hot == b2, false);
    } else add_hot((struct rect){ 0, 0, 0, 0 });

    heading(s, 260, "storage");
    int dirs, files;
    size_t bytes;
    fs_usage(&dirs, &files, &bytes);
    char line[96];
    struct disk *d = diskfs_mounted();
    if (d) snprintf(line, sizeof line, "saved on %s (%s), %lu MiB. saved %lu times so far.", d->name, d->model, d->sectors / 2048, diskfs_saves());
    else snprintf(line, sizeof line, "no disk: files live in memory and vanish when you turn off.");
    gfx_utext(s, &font_ui, CX, 292, line, ui(UI_DIM));
    snprintf(line, sizeof line, "%d files and %d folders, %lu KB", files, dirs, (uint64_t)bytes / 1024);
    gfx_utext(s, &font_ui, CX, 314, line, ui(UI_TEXT));
    int used = files + dirs, bw = W - CX - 30;
    gfx_round(s, CX, 342, bw, 10, 5, ui(UI_RAISED));
    gfx_round(s, CX, 342, 10 + used * (bw - 10) / FS_MAX_NODES, 10, 5, ui(UI_ACCENT));
    snprintf(line, sizeof line, "%d of %d file slots used", used, FS_MAX_NODES);
    gfx_utext(s, &font_small, CX, 360, line, ui(UI_DIM));
    if (status[0]) gfx_utext(s, &font_bold, CX, H - 40, status, ui(UI_OK));
}

/* ---- about ----------------------------------------------------------------------------------- */

static void cpu_brand(char *out) {
    uint32_t r[4];
    asm volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(0x80000000), "c"(0));
    strlcpy(out, "unknown", 49);
    if (r[0] < 0x80000004) return;
    for (uint32_t i = 0; i < 3; i++) {
        asm volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(0x80000002 + i), "c"(0));
        memcpy(out + i * 16, r, 16);
    }
    out[48] = 0;
    while (*out == ' ') memmove(out, out + 1, strlen(out));
}

static void page_about(struct surface *s) {
    int lx = CX + 50, ly = 80;
    gfx_circle(s, lx, ly, 46, gfx_mix(ui(UI_ACCENT), ui(UI_BG), 200));
    gfx_circle(s, lx, ly, 34, ui(UI_ACCENT));
    gfx_circle(s, lx + 14, ly - 12, 30, gfx_mix(ui(UI_ACCENT), ui(UI_BG), 200));
    gfx_utext(s, &font_big, CX + 124, 40, "dream", ui(UI_TEXT));
    gfx_utext(s, &font_ui, CX + 126, 88, "version " DREAM_VERSION ", x86_64. a small OS for learning.", ui(UI_DIM));

    char brand[49], v[96];
    cpu_brand(brand);
    uint64_t up = pit_ticks() / pit_hz();
    struct task_info ti[32];
    int dirs, files;
    size_t bytes;
    fs_usage(&dirs, &files, &bytes);
    const char *rows[6][2] = { { "processor", brand }, { "memory", NULL }, { "display", NULL },
                               { "uptime", NULL }, { "tasks", NULL }, { "theme", theme->name } };
    char mem[48], disp[48], upt[48], tasks[48];
    snprintf(mem, sizeof mem, "%lu MiB used of %lu MiB", (pmm_used_pages() * PAGE_SIZE) >> 20, (pmm_total_pages() * PAGE_SIZE) >> 20);
    snprintf(disp, sizeof disp, "%u x %u, 32-bit colour, double buffered", fb.width, fb.height);
    snprintf(upt, sizeof upt, "%luh %02lum %02lus", up / 3600, (up / 60) % 60, up % 60);
    snprintf(tasks, sizeof tasks, "%d running side by side, %d files on disk", task_list(ti, 32), files);
    rows[1][1] = mem; rows[2][1] = disp; rows[3][1] = upt; rows[4][1] = tasks;
    gfx_round(s, CX, 150, W - CX - 30, 6 * 34 + 16, 14, ui(UI_RAISED));
    for (int i = 0; i < 6; i++) {
        int y = 162 + i * 34;
        if (i) gfx_fill(s, CX + 16, y - 6, W - CX - 62, 1, ui(UI_LINE));
        gfx_utext(s, &font_bold, CX + 18, y + 3, rows[i][0], ui(UI_DIM));
        gfx_utext_fit(s, &font_ui, CX + 130, y + 3, rows[i][1], ui(UI_TEXT), W - CX - 190);
    }
    snprintf(v, sizeof v, "booted by limine. written in C. the text is set in Inter.");
    gfx_utext(s, &font_small, CX, H - 40, v, ui(UI_DIM));
    for (int p = 0; p < PAL_COUNT; p++) gfx_circle(s, W - 40 - (PAL_COUNT - 1 - p) * 22, H - 33, 7, theme->pal[p]);
}

/* ---- the window ------------------------------------------------------------------------------- */

static void draw(struct window *w) {
    struct surface *s = w->surf;
    gfx_fill(s, 0, 0, W, H, ui(UI_BG));
    gfx_fill(s, 0, 0, SIDE, H, ui(UI_SIDEBAR));
    gfx_fill(s, SIDE, 0, 1, H, ui(UI_LINE));
    for (int i = 0; i < NPAGES; i++) {
        int y = 16 + i * 40;
        if (i == page) gfx_round(s, 10, y, SIDE - 20, 34, 9, ui(UI_SEL));
        else if (i == hover_page) gfx_round(s, 10, y, SIDE - 20, 34, 9, ui(UI_HOVER));
        icon_glyph(s, page_glyphs[i], 24, y + 9, 16, i == page ? ui(UI_TEXT) : ui(UI_DIM));
        gfx_utext(s, i == page ? &font_bold : &font_ui, 52, y + 8, page_names[i], i == page ? ui(UI_TEXT) : ui(UI_DIM));
    }
    gfx_utext(s, &font_small, 20, H - 30, "dream " DREAM_VERSION, ui(UI_DIM));
    nhot = 0;
    if (page == PAGE_LOOK) page_look(s);
    else if (page == PAGE_DESKTOP) page_desktop(s);
    else if (page == PAGE_SYSTEM) page_system(s);
    else page_about(s);
}

static void save_host(void) {
    if (!host[0]) return;
    fs_lock();
    int n = fs_lookup(0, "/etc/hostname");
    if (n < 0) n = fs_create(0, "/etc/hostname", false);
    char line[40];
    int len = snprintf(line, sizeof line, "%s\n", host);
    if (n >= 0) fs_write(n, line, len, false);
    fs_unlock();
    diskfs_autosave();
    strlcpy(status, "name saved.", sizeof status);
}

static void click_hot(int i) {
    status[0] = 0;
    if (page == PAGE_LOOK) {
        if (i < theme_count) {
            theme_set(themes[i].name);
            config_set("theme", themes[i].name);
        } else if (i - theme_count < wallpaper_count) {
            config_set("wallpaper", wallpaper_names[i - theme_count]);
            wm_wallpaper_changed();
        }
    } else if (page == PAGE_DESKTOP) {
        bool on = strcmp(config_get("autostart", "yes"), "no");
        config_set("autostart", on ? "no" : "yes");
    } else if (page == PAGE_SYSTEM) {
        if (i == 0) { focus_field = F_HOST; strlcpy(host, sh_hostname(), sizeof host); }
        else if (i == 1) focus_field = F_PW;
        else if (i == 2 && pw[0]) { passwd_change(pw); pw[0] = 0; focus_field = F_NONE; strlcpy(status, "password set. try the lock screen.", sizeof status); }
        else if (i == 3) { passwd_change(""); strlcpy(status, "password removed.", sizeof status); }
    }
}

static void mouse(struct window *w, int x, int y, uint8_t buttons, int wheel) {
    (void)wheel;
    bool click = (buttons & MOUSE_LEFT) && !(held & MOUSE_LEFT);
    held = buttons;
    int hp = x < SIDE && y >= 16 && (y - 16) / 40 < NPAGES && (y - 16) % 40 < 34 ? (y - 16) / 40 : -1;
    int hh = -1;
    for (int i = 0; i < nhot && i < 24; i++) if (rect_contains(hot[i], x, y)) hh = i;
    bool changed = hp != hover_page || hh != hover_hot;
    hover_page = hp;
    hover_hot = hh;
    if (click) {
        changed = true;
        if (hp >= 0) { page = hp; focus_field = F_NONE; status[0] = 0; }
        else if (hh >= 0) click_hot(hh);
        else focus_field = F_NONE;
    }
    if (changed) draw(w);
}

static void key(struct window *w, int k) {
    char *buf = focus_field == F_HOST ? host : focus_field == F_PW ? pw : NULL;
    if (!buf) {
        if (k == KEY_DOWN && page < NPAGES - 1) page++;
        else if (k == KEY_UP && page > 0) page--;
        draw(w);
        return;
    }
    size_t n = strlen(buf), cap = focus_field == F_HOST ? 24 : sizeof pw - 1;
    if (k == '\n') {
        if (focus_field == F_HOST) save_host();
        else if (pw[0]) { passwd_change(pw); pw[0] = 0; strlcpy(status, "password set.", sizeof status); }
        focus_field = F_NONE;
    } else if (k == 27) focus_field = F_NONE;
    else if (k == '\b') { if (n) buf[n - 1] = 0; }
    else if (k > 32 && k < 127 && n < cap && (focus_field == F_PW || k == '-' || (k | 32) >= 'a')) { buf[n] = (char)k; buf[n + 1] = 0; }
    draw(w);
}

static void tick(struct window *w) {
    static uint64_t last;
    if (page == PAGE_ABOUT && pit_ticks() / pit_hz() != last) { last = pit_ticks() / pit_hz(); draw(w); }
}

static void redraw(struct window *w) { make_thumbs(); draw(w); }

const struct app settings_app = { .name = "settings", .mouse = mouse, .key = key, .tick = tick,
                                  .redraw = redraw, .icon = ICON_SETTINGS };

void settings_open_page(int p) {
    struct window *w = wm_find(&settings_app);
    page = p;
    if (w) { wm_show(w); draw(w); return; }
    w = wm_open(&settings_app, "settings", W, H, NULL);
    if (!w) return;
    focus_field = F_NONE;
    make_thumbs();
    draw(w);
}

void settings_open(void) { settings_open_page(PAGE_LOOK); }
void about_open(void)    { settings_open_page(PAGE_ABOUT); }
