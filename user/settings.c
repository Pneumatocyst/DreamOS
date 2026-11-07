/* settings: themes and wallpapers, desktop behaviour, the machine's name and
 * password, and an "about" page. Choices are written to /etc/dream.conf (through
 * the config system call), so they're still there after a reboot.
 *
 *   settings          open on "appearance"
 *   settings about    open on "about" */
#include "gui.h"

#define W        760
#define H        500
#define SIDE     196
#define CX       (SIDE + 30)
#define NPAGES   4
#define MAXHOT   24
#define C(slot) COLOR_UI(slot)

enum { PAGE_LOOK, PAGE_DESKTOP, PAGE_SYSTEM, PAGE_ABOUT };
enum { F_NONE, F_HOST, F_PW };

static int win, page, hover_page = -1, focus_field;
static char host[32], pw[32];
static struct { int x, y, w, h; } hot[MAXHOT];  /* clickable things on the current page */
static int nhot, hover_hot = -1;
static uint8_t held;
static char status[64];
static struct sysinfo si;

static const char *const page_names[NPAGES] = { "appearance", "desktop", "system", "about" };
static const int page_glyphs[NPAGES] = { GLYPH_PALETTE, GLYPH_HOME, GLYPH_CHIP, GLYPH_INFO };

static int add_hot(int x, int y, int w, int h) {
    if (nhot < MAXHOT) { hot[nhot].x = x; hot[nhot].y = y; hot[nhot].w = w; hot[nhot].h = h; }
    return nhot++;
}

static void heading(int y, const char *t) { draw_text(FONT_TITLE, CX, y, t, C(UI_TEXT)); }

/* ---- appearance ------------------------------------------------------------------------------ */

static void theme_card(int x, int y, const struct theme_info *t, bool chosen, bool hov) {
    int w = 116, h = 80;
    if (chosen) draw_round(x - 4, y - 4, w + 8, h + 8, 16, C(UI_ACCENT));
    else if (hov) draw_round(x - 4, y - 4, w + 8, h + 8, 16, C(UI_HOVER));
    draw_gradient(x, y, w, h, 12, t->bg_top, t->bg_bottom);
    draw_round(x + 14, y + 14, 70, 44, 6, mix(t->bar, t->bg_top, 90));     /* a tiny window */
    draw_round(x + 14, y + 14, 70, 10, 4, t->bar);
    draw_round(x + 20, y + 30, 40, 4, 2, t->pal[0]);
    draw_round(x + 20, y + 38, 28, 4, 2, t->pal[2]);
    draw_round(x + 20, y + 46, 34, 4, 2, t->pal[3]);
    draw_circle(x + w - 16, y + h - 16, 7, t->pal[2]);
    draw_text_center(chosen ? FONT_BOLD : FONT_UI, x + w / 2, y + h + 10, t->name, chosen ? C(UI_TEXT) : C(UI_DIM));
}

static void page_look(void) {
    heading(26, "theme");
    draw_text(FONT_UI, CX, 54, "colours for everything: the desktop, the terminal and every app.", C(UI_DIM));
    struct theme_info t;
    int n = theme_get(0, &t);
    for (int i = 0; i < n && i < 4; i++) {
        theme_get(i, &t);
        int x = CX + i * 132, y = 84;
        int id = add_hot(x - 4, y - 4, 124, 112);
        theme_card(x, y, &t, !strcmp(t.name, si.theme), hover_hot == id);
    }
    heading(238, "wallpaper");
    char cur[64], name[16];
    if (!config_get("wallpaper", cur)) wallpaper_name(0, cur);
    int nw = wallpaper_name(0, name);
    for (int i = 0; i < nw && i < 6; i++) {
        wallpaper_name(i, name);
        int x = CX + i * 100, y = 276;
        int id = add_hot(x - 4, y - 4, 96, 86);
        bool chosen = !strcmp(cur, name);
        uint32_t edge = chosen ? ui(UI_ACCENT) : hover_hot == id ? ui(UI_HOVER) : ui(UI_BG);
        if (chosen || hover_hot == id) draw_round(x - 4, y - 4, 96, 64, 12, edge);
        draw_wallpaper(x, y, 88, 56, name);
        /* round off the preview's corners by painting the frame colour back over them */
        draw_ring(x - 1, y - 1, 90, 58, 9, edge);
        draw_ring(x - 2, y - 2, 92, 60, 10, edge);
        draw_text_center(chosen ? FONT_BOLD : FONT_UI, x + 44, y + 64, name, chosen ? C(UI_TEXT) : C(UI_DIM));
    }
    draw_text(FONT_SMALL, CX, H - 40, "tip: right-click the desktop to flip through wallpapers.", C(UI_DIM));
}

/* ---- desktop --------------------------------------------------------------------------------- */

static bool autostart(void) { char v[64]; return !config_get("autostart", v) || strcmp(v, "no"); }

static void page_desktop(void) {
    heading(26, "starting up");
    draw_round(CX, 64, W - CX - 30, 60, 12, C(UI_RAISED));
    draw_text(FONT_BOLD, CX + 16, 74, "open the desktop when dream starts", C(UI_TEXT));
    draw_text(FONT_SMALL, CX + 16, 96, "off: you get the text-mode shell, and can type startx later", C(UI_DIM));
    add_hot(W - 30 - 60, 82, 44, 24);
    draw_toggle(W - 30 - 58, 83, autostart());

    heading(150, "keyboard shortcuts");
    static const char *const keys[][2] = {
        { "super or f1", "open the launcher (type to search)" },
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
        int kw = text_width(FONT_BOLD, keys[i][0]) + 20;
        draw_round(CX, y, kw, 24, 7, C(UI_RAISED));
        draw_text(FONT_BOLD, CX + 10, y + 3, keys[i][0], C(UI_TEXT));
        draw_text(FONT_UI, CX + 170, y + 3, keys[i][1], C(UI_DIM));
    }
}

/* ---- system ---------------------------------------------------------------------------------- */

static void page_system(void) {
    heading(26, "this computer's name");
    draw_text(FONT_UI, CX, 54, "shown in the prompt and on the lock screen. press enter to save.", C(UI_DIM));
    add_hot(CX, 80, 280, 36);
    draw_field(CX, 80, 280, 36, focus_field == F_HOST ? host : si.hostname, focus_field == F_HOST, false);

    bool has_pw = passwd_isset();
    heading(140, "password");
    draw_text(FONT_UI, CX, 168, has_pw ? "the lock screen asks for a password." : "no password: the lock screen opens with a click.", C(UI_DIM));
    add_hot(CX, 194, 280, 36);
    if (!pw[0] && focus_field != F_PW) {
        draw_field(CX, 194, 280, 36, "", false, false);
        draw_text(FONT_UI, CX + 10, 203, "new password", C(UI_DIM));
    } else draw_field(CX, 194, 280, 36, pw, focus_field == F_PW, true);
    int b1 = add_hot(CX + 292, 194, 90, 36);
    draw_button(CX + 292, 194, 90, 36, "set", hover_hot == b1, true);
    if (has_pw) {
        int b2 = add_hot(CX + 392, 194, 100, 36);
        draw_button(CX + 392, 194, 100, 36, "remove", hover_hot == b2, false);
    } else add_hot(0, 0, 0, 0);

    heading(260, "storage");
    char line[128];
    if (si.disk[0]) snprintf(line, sizeof line, "saved on %s (%s), %lu MiB. saved %lu times so far.", si.disk, si.disk_model, si.disk_mb, si.disk_saves);
    else snprintf(line, sizeof line, "no disk: files live in memory and vanish when you turn off.");
    draw_text(FONT_UI, CX, 292, line, C(UI_DIM));
    snprintf(line, sizeof line, "%d files and %d folders, %lu KB", si.files, si.dirs, si.fs_bytes / 1024);
    draw_text(FONT_UI, CX, 314, line, C(UI_TEXT));
    int used = si.files + si.dirs, bw = W - CX - 30, max = si.max_nodes ? si.max_nodes : 1;
    draw_round(CX, 342, bw, 10, 5, C(UI_RAISED));
    draw_round(CX, 342, 10 + used * (bw - 10) / max, 10, 5, C(UI_ACCENT));
    snprintf(line, sizeof line, "%d of %d file slots used", used, max);
    draw_text(FONT_SMALL, CX, 360, line, C(UI_DIM));
    if (status[0]) draw_text(FONT_BOLD, CX, H - 40, status, C(UI_OK));
}

/* ---- about ----------------------------------------------------------------------------------- */

/* cpuid works from ring 3 too: the CPU will tell anyone its name */
static void cpu_brand(char out[49]) {
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

static void page_about(void) {
    uint32_t acc = ui(UI_ACCENT), bg = ui(UI_BG);
    int lx = CX + 50, ly = 80;
    draw_circle(lx, ly, 46, mix(acc, bg, 200));
    draw_circle(lx, ly, 34, acc);
    draw_circle(lx + 14, ly - 12, 30, mix(acc, bg, 200));
    draw_text(FONT_BIG, CX + 124, 40, "dream", C(UI_TEXT));
    char ver[64];
    snprintf(ver, sizeof ver, "version %s, x86_64. a small OS for learning.", si.version);
    draw_text(FONT_UI, CX + 126, 88, ver, C(UI_DIM));

    char brand[49], mem[48], disp[48], upt[48], tsk[64];
    cpu_brand(brand);
    uint64_t up = si.uptime_ms / 1000;
    snprintf(mem, sizeof mem, "%lu MiB used of %lu MiB", si.mem_used >> 20, si.mem_total >> 20);
    snprintf(disp, sizeof disp, "%d x %d, 32-bit colour, double buffered", si.screen_w, si.screen_h);
    snprintf(upt, sizeof upt, "%luh %02lum %02lus", up / 3600, (up / 60) % 60, up % 60);
    snprintf(tsk, sizeof tsk, "%d running side by side, %d files on disk", si.ntasks, si.files);
    const char *rows[6][2] = { { "processor", brand }, { "memory", mem }, { "display", disp },
                               { "uptime", upt }, { "tasks", tsk }, { "theme", si.theme } };
    draw_round(CX, 150, W - CX - 30, 6 * 34 + 16, 14, C(UI_RAISED));
    for (int i = 0; i < 6; i++) {
        int y = 162 + i * 34;
        if (i) draw_fill(CX + 16, y - 6, W - CX - 62, 1, C(UI_LINE));
        draw_text(FONT_BOLD, CX + 18, y + 3, rows[i][0], C(UI_DIM));
        draw_text_fit(FONT_UI, CX + 130, y + 3, rows[i][1], C(UI_TEXT), W - CX - 190);
    }
    draw_text(FONT_SMALL, CX, H - 40, "booted by limine. written in C. the text is set in Inter.", C(UI_DIM));
    for (int p = 0; p < 6; p++) draw_circle(W - 40 - (5 - p) * 22, H - 33, 7, current_theme()->pal[p]);
}

/* ---- the window ------------------------------------------------------------------------------- */

static void draw(void) {
    sysinfo(&si);
    draw_fill(0, 0, W, H, C(UI_BG));
    draw_fill(0, 0, SIDE, H, C(UI_SIDEBAR));
    draw_fill(SIDE, 0, 1, H, C(UI_LINE));
    for (int i = 0; i < NPAGES; i++) {
        int y = 16 + i * 40;
        if (i == page) draw_round(10, y, SIDE - 20, 34, 9, C(UI_SEL));
        else if (i == hover_page) draw_round(10, y, SIDE - 20, 34, 9, C(UI_HOVER));
        draw_glyph(page_glyphs[i], 24, y + 9, 16, i == page ? C(UI_TEXT) : C(UI_DIM));
        draw_text(i == page ? FONT_BOLD : FONT_UI, 52, y + 8, page_names[i], i == page ? C(UI_TEXT) : C(UI_DIM));
    }
    char ver[24];
    snprintf(ver, sizeof ver, "dream %s", si.version);
    draw_text(FONT_SMALL, 20, H - 30, ver, C(UI_DIM));
    nhot = 0;
    if (page == PAGE_LOOK) page_look();
    else if (page == PAGE_DESKTOP) page_desktop();
    else if (page == PAGE_SYSTEM) page_system();
    else page_about();
}

static void save_host(void) {
    if (!host[0]) return;
    char line[40];
    int len = snprintf(line, sizeof line, "%s\n", host);
    if (writefile("/etc/hostname", line, len) >= 0) strlcpy(status, "name saved.", sizeof status);
}

static void set_password(void) {
    if (!pw[0]) return;
    passwd_set(pw);
    pw[0] = 0;
    focus_field = F_NONE;
    strlcpy(status, "password set. try the lock screen.", sizeof status);
}

static void click_hot(int i) {
    status[0] = 0;
    if (page == PAGE_LOOK) {
        struct theme_info t;
        int nt = theme_get(0, &t);
        char name[16];
        if (i < nt) { theme_get(i, &t); theme_set(t.name); }
        else if (wallpaper_name(i - nt, name) > 0) wallpaper_set(name);
    } else if (page == PAGE_DESKTOP) {
        config_set("autostart", autostart() ? "no" : "yes");
    } else if (page == PAGE_SYSTEM) {
        if (i == 0) { focus_field = F_HOST; strlcpy(host, si.hostname, sizeof host); }
        else if (i == 1) focus_field = F_PW;
        else if (i == 2) set_password();
        else if (i == 3 && passwd_isset()) { passwd_set(""); strlcpy(status, "password removed.", sizeof status); }
    }
}

static bool in(int i, int x, int y) { return x >= hot[i].x && x < hot[i].x + hot[i].w && y >= hot[i].y && y < hot[i].y + hot[i].h; }

static void mouse(int x, int y, uint8_t buttons, int wheel) {
    (void)wheel;
    bool click = (buttons & 1) && !(held & 1);
    held = buttons;
    int hp = x < SIDE && y >= 16 && (y - 16) / 40 < NPAGES && (y - 16) % 40 < 34 ? (y - 16) / 40 : -1;
    int hh = -1;
    for (int i = 0; i < nhot && i < MAXHOT; i++) if (in(i, x, y)) hh = i;
    bool changed = hp != hover_page || hh != hover_hot;
    hover_page = hp;
    hover_hot = hh;
    if (click) {
        changed = true;
        if (hp >= 0) { page = hp; focus_field = F_NONE; status[0] = 0; }
        else if (hh >= 0) click_hot(hh);
        else focus_field = F_NONE;
    }
    if (changed) draw();
}

static void key(int k) {
    char *buf = focus_field == F_HOST ? host : focus_field == F_PW ? pw : NULL;
    if (!buf) {
        if (k == KEY_DOWN && page < NPAGES - 1) page++;
        else if (k == KEY_UP && page > 0) page--;
        draw();
        return;
    }
    size_t n = strlen(buf), cap = focus_field == F_HOST ? 24 : sizeof pw - 1;
    if (k == '\n') {
        if (focus_field == F_HOST) save_host();
        else set_password();
        focus_field = F_NONE;
    } else if (k == 27) focus_field = F_NONE;
    else if (k == '\b') { if (n) buf[n - 1] = 0; }
    else if (k > 32 && k < 127 && n < cap && (focus_field == F_PW || k == '-' || (k | 32) >= 'a')) { buf[n] = (char)k; buf[n + 1] = 0; }
    draw();
}

static void tick(void) { if (page == PAGE_ABOUT) draw(); }   /* the uptime ticks along */

int main(int argc, char **argv) {
    win = win_open("settings", W, H, ICON_SETTINGS, 0, 0);
    if (win < 0) { puts("settings needs the desktop: type startx first.\n"); return 1; }
    if (argc > 1 && !strcmp(argv[1], "about")) page = PAGE_ABOUT;
    app_run(win, &(struct app_loop){ .draw = draw, .key = key, .mouse = mouse, .tick = tick, .tick_ms = 1000 });
    return 0;
}
