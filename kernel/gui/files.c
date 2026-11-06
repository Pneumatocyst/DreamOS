/* files: a file browser.
 *
 *   click            select            double-click / enter   open
 *   backspace        go up             arrows                 move the selection
 *   delete           delete (asks)     scroll wheel           scroll
 *
 * Opening a folder goes into it. Opening a program runs it in the terminal;
 * opening anything else opens it in the editor there. It also watches the
 * filesystem's change counter, so it updates when you change files elsewhere. */
#include "gui/wm.h"
#include "fs/ramfs.h"
#include "fs/diskfs.h"
#include "drivers/theme.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "arch/cpu.h"
#include "shell/shell.h"

#define BAR       52                    /* toolbar */
#define STATUS    30                    /* status line at the bottom */
#define ROW       32
#define SIDE      170                   /* the places on the left */
#define PREVIEW_W 240
#define NPLACES   4

static int W = 860, H = 500;           /* follow the window's size */
#define LIST_X    SIDE
#define LIST_W    (W - SIDE - PREVIEW_W)
#define VISIBLE   ((H - BAR - STATUS - 8) / ROW)

enum { KIND_FOLDER, KIND_TEXT, KIND_PROGRAM };
enum { MODE_BROWSE, MODE_NAME, MODE_CONFIRM };
enum { NAME_FOLDER, NAME_FILE, NAME_RENAME };

static int dir;                         /* the folder we're looking at */
static int items[FS_MAX_NODES], nitems;
static int sel = -1, scroll, hover = -1, hover_btn = -1, hover_place = -1;
static uint64_t seen_gen;
static uint64_t last_click;
static int last_click_item = -1;
static uint8_t held;
static int mode, name_kind;
static char input[FS_NAME_MAX];
static char status[96];
static uint8_t status_color;

static void act_up(struct window *w);
static void act_home(struct window *w);
static void act_folder(struct window *w);
static void act_file(struct window *w);
static void act_rename(struct window *w);
static void act_delete(struct window *w);

static struct { struct rect r; const char *label; int glyph; void (*act)(struct window *); bool needs_sel; } btns[] = {
    { { 0 }, "up", GLYPH_UP, act_up, false },                   { { 0 }, "home", GLYPH_HOME, act_home, false },
    { { 0 }, "new folder", GLYPH_FOLDER, act_folder, false },   { { 0 }, "new file", GLYPH_PLUS, act_file, false },
    { { 0 }, "rename", GLYPH_PEN, act_rename, true },           { { 0 }, "delete", GLYPH_TRASH, act_delete, true },
};
#define NBTNS (int)(sizeof btns / sizeof btns[0])

static const struct { const char *label, *path; int glyph; } places[NPLACES] = {
    { "home", "/root", GLYPH_HOME }, { "programs", "/bin", GLYPH_PROGRAM },
    { "settings", "/etc", GLYPH_CHIP }, { "everything", "/", GLYPH_ROOT },
};

/* ---- looking at the filesystem ------------------------------------------------------ */

static int kind_of(int n) {
    const struct fs_node *f = fs_get(n);
    if (f->dir) return KIND_FOLDER;
    if (f->size >= 4 && !memcmp(fs_data(n), "\x7f" "ELF", 4)) return KIND_PROGRAM;
    return KIND_TEXT;
}

static void say(uint8_t color, const char *msg) {
    strlcpy(status, msg, sizeof status);
    status_color = color;
}

static void reload(void) {
    fs_lock();
    if (!fs_get(dir)->used || !fs_get(dir)->dir) dir = fs_home();   /* our folder was deleted */
    int sel_node = sel >= 0 && sel < nitems ? items[sel] : -1;
    nitems = 0;
    for (int c = fs_child(dir, -1); c >= 0; c = fs_child(dir, c)) {
        /* folders first, then by name */
        int i = nitems++;
        while (i > 0) {
            const struct fs_node *a = fs_get(items[i - 1]), *b = fs_get(c);
            if (a->dir != b->dir ? b->dir : strcmp(a->name, b->name) > 0) { items[i] = items[i - 1]; i--; }
            else break;
        }
        items[i] = c;
    }
    sel = -1;
    for (int i = 0; i < nitems; i++) if (items[i] == sel_node) sel = i;
    seen_gen = fs_generation();
    fs_unlock();
    if (scroll > nitems - VISIBLE) scroll = nitems - VISIBLE;
    if (scroll < 0) scroll = 0;
}

static void select_node(int node) {
    for (int i = 0; i < nitems; i++) if (items[i] == node) sel = i;
    if (sel >= 0 && sel < scroll) scroll = sel;
    if (sel >= scroll + VISIBLE) scroll = sel - VISIBLE + 1;
}

static void size_text(char *out, size_t n, int node) {
    const struct fs_node *f = fs_get(node);
    if (f->dir) {
        int count = 0;
        for (int c = fs_child(node, -1); c >= 0; c = fs_child(node, c)) count++;
        snprintf(out, n, "%d item%s", count, count == 1 ? "" : "s");
    } else if (f->size < 1024) snprintf(out, n, "%u bytes", f->size);
    else snprintf(out, n, "%u.%u KB", f->size / 1024, (f->size % 1024) * 10 / 1024);
}

/* ---- drawing ---------------------------------------------------------------------------- */

static void icon(struct surface *s, int x, int y, int kind, int size) {
    if (kind == KIND_FOLDER) {
        uint32_t c = ui(UI_ACCENT);
        gfx_round(s, x, y + size * 4 / 24, size * 10 / 24, size * 5 / 24, size / 10, gfx_mix(c, 0x000000, 70));
        gfx_round_ex(s, x, y + size * 6 / 24, size, size * 16 / 24, size / 7, gfx_mix(c, 0xffffff, 40), c, 256);
    } else if (kind == KIND_PROGRAM) {
        gfx_round_ex(s, x + size / 12, y + size / 12, size * 20 / 24, size * 20 / 24, size / 5,
                     gfx_mix(ui(UI_OK), 0xffffff, 30), ui(UI_OK), 256);
        icon_glyph(s, GLYPH_PROGRAM, x + size / 4, y + size / 4, size / 2, ui(UI_ON_ACCENT));
    } else {
        int w = size * 17 / 24, x0 = x + (size - w) / 2;
        gfx_round(s, x0, y, w, size, size / 8, gfx_mix(ui(UI_TEXT), ui(UI_BG), 40));
        for (int j = 0; j < 3; j++)
            gfx_fill(s, x0 + w / 5, y + size * (7 + j * 4) / 24, w * 3 / 5 - (j == 2 ? w / 5 : 0), size / 24 + 1,
                     gfx_mix(ui(UI_DIM), ui(UI_BG), 40));
    }
}

static bool btn_disabled(int i) { return (btns[i].needs_sel && sel < 0) || (btns[i].act == act_up && dir == 0); }

static void draw_preview(struct surface *s) {
    uint32_t bg = ui(UI_SIDEBAR);
    int x0 = W - PREVIEW_W, y0 = BAR, h = H - BAR - STATUS;
    gfx_fill(s, x0, y0, PREVIEW_W, h, bg);
    gfx_fill(s, x0, y0, 1, h, ui(UI_LINE));

    int node = sel >= 0 ? items[sel] : dir;
    const struct fs_node *f = fs_get(node);
    int kind = kind_of(node);
    icon(s, x0 + (PREVIEW_W - 64) / 2, y0 + 24, kind, 64);

    char line[64];
    const char *name = node == 0 ? "/" : f->name;
    gfx_utext_center(s, &font_title, x0 + PREVIEW_W / 2, y0 + 100, name, ui(UI_TEXT));
    char size[24];
    size_text(size, sizeof size, node);
    snprintf(line, sizeof line, "%s, %s", kind == KIND_FOLDER ? "folder" : kind == KIND_PROGRAM ? "program" : "text", size);
    gfx_utext_center(s, &font_ui, x0 + PREVIEW_W / 2, y0 + 130, line, ui(UI_DIM));
    const struct rtc_time *t = &f->mtime;
    snprintf(line, sizeof line, "changed %s %u, %02u:%02u", rtc_month_names[(t->mon + 11) % 12], t->day, t->hour, t->min);
    gfx_utext_center(s, &font_small, x0 + PREVIEW_W / 2, y0 + 152, line, ui(UI_DIM));

    int y = y0 + 186, cols = (PREVIEW_W - 32) / FONT_W, maxy = y0 + h - 22;
    gfx_round(s, x0 + 12, y - 12, PREVIEW_W - 24, maxy - y + 22, 10, gfx_mix(bg, 0x000000, 40));
    if (kind == KIND_PROGRAM) {
        static const char *about[] = { "a user-mode program.", "open it to run it", "in the terminal." };
        for (int i = 0; i < 3; i++, y += 20) gfx_utext(s, &font_ui, x0 + 24, y, about[i], ui(UI_DIM));
    } else if (kind == KIND_FOLDER) {
        int shown = 0;
        for (int c = fs_child(node, -1); c >= 0 && y < maxy - 10; c = fs_child(node, c), y += 20, shown++) {
            snprintf(line, sizeof line, "%s%s", fs_get(c)->name, fs_get(c)->dir ? "/" : "");
            gfx_utext_fit(s, &font_ui, x0 + 24, y, line, fs_get(c)->dir ? ui(UI_ACCENT) : ui(UI_TEXT), PREVIEW_W - 48);
        }
        if (!shown) gfx_utext(s, &font_ui, x0 + 24, y, "(empty)", ui(UI_DIM));
    } else {
        /* the first lines of the file, in the terminal's font */
        fs_lock();
        const char *d = fs_data(node);
        uint32_t len = fs_get(node)->size, i = 0;
        while (i < len && y < maxy - 10) {
            int c = 0;
            while (i < len && d[i] != '\n') {
                if (c < cols && c < 63) { char ch = d[i]; line[c++] = ch >= 32 && ch < 127 ? ch : '.'; }
                i++;
            }
            line[c] = 0;
            gfx_text(s, x0 + 22, y, line, ui(UI_TEXT), -1);
            y += 18;
            i++;
        }
        fs_unlock();
        if (!len) gfx_utext(s, &font_ui, x0 + 24, y, "(empty file)", ui(UI_DIM));
    }
}

static void draw(struct surface *s) {
    uint32_t list_bg = ui(UI_BG);

    /* places */
    gfx_fill(s, 0, 0, SIDE, H, ui(UI_SIDEBAR));
    gfx_fill(s, SIDE, 0, 1, H, ui(UI_LINE));
    gfx_utext(s, &font_small, 18, 18, "places", ui(UI_DIM));
    for (int i = 0; i < NPLACES; i++) {
        int y = 40 + i * 36;
        bool here = fs_lookup(0, places[i].path) == dir;
        if (here) gfx_round(s, 8, y, SIDE - 16, 32, 8, ui(UI_SEL));
        else if (i == hover_place) gfx_round(s, 8, y, SIDE - 16, 32, 8, ui(UI_HOVER));
        icon_glyph(s, places[i].glyph, 20, y + 8, 16, here ? ui(UI_TEXT) : ui(UI_DIM));
        gfx_utext(s, here ? &font_bold : &font_ui, 46, y + 7, places[i].label, here ? ui(UI_TEXT) : ui(UI_DIM));
    }
    int dirs, files;
    size_t bytes;
    fs_usage(&dirs, &files, &bytes);
    char line[128];
    snprintf(line, sizeof line, "%d files, %lu KB", files, (uint64_t)bytes / 1024);
    gfx_utext(s, &font_small, 18, H - STATUS - 26, line, ui(UI_DIM));

    /* toolbar: icon buttons, then where we are */
    gfx_fill(s, SIDE + 1, 0, W - SIDE - 1, BAR, list_bg);
    gfx_fill(s, SIDE + 1, BAR - 1, W - SIDE - 1, 1, ui(UI_LINE));
    int x = SIDE + 12;
    for (int i = 0; i < NBTNS; i++) {
        if (i == 2) x = W - 12 - 4 * 38;
        btns[i].r = (struct rect){ x, 10, 34, 32 };
        bool dis = btn_disabled(i);
        if (dis) icon_glyph(s, btns[i].glyph, x + 8, 18, 18, gfx_mix(ui(UI_DIM), ui(UI_BG), 120));
        else ui_icon_button(s, btns[i].r, btns[i].glyph, i == hover_btn, false);
        x += 38;
    }
    char path[256];
    fs_path(dir, path, sizeof path);
    /* the path as breadcrumbs: dim folders, the last one bright */
    int px = SIDE + 12 + 2 * 38 + 10, room = W - 12 - 4 * 38 - px - 10;
    const char *last = strrchr(path, '/');
    char head[256];
    size_t hl = last && last != path ? (size_t)(last - path + 1) : 1;
    strlcpy(head, path, hl + 1 < sizeof head ? hl + 1 : sizeof head);
    const char *tail = dir == 0 ? "" : last + 1;
    int tw = gfx_utext_width(&font_bold, tail);
    int hx = gfx_utext_fit(s, &font_ui, px, 17, head, ui(UI_DIM), room - tw > 30 ? room - tw : 30);
    gfx_utext_fit(s, &font_bold, hx, 17, tail, ui(UI_TEXT), px + room - hx);

    /* the list */
    gfx_fill(s, LIST_X + 1, BAR, LIST_W - 1, H - BAR - STATUS, list_bg);
    for (int i = scroll; i < nitems && i < scroll + VISIBLE; i++) {
        int y = BAR + 6 + (i - scroll) * ROW, node = items[i];
        if (i == sel) gfx_round(s, LIST_X + 8, y, LIST_W - 16, ROW - 2, 8, ui(UI_SEL));
        else if (i == hover) gfx_round(s, LIST_X + 8, y, LIST_W - 16, ROW - 2, 8, ui(UI_HOVER));
        icon(s, LIST_X + 18, y + 6, kind_of(node), 18);
        gfx_utext_fit(s, &font_ui, LIST_X + 46, y + 7, fs_get(node)->name, ui(UI_TEXT), LIST_W - 160);
        char size[24];
        size_text(size, sizeof size, node);
        gfx_utext(s, &font_small, LIST_X + LIST_W - 22 - gfx_utext_width(&font_small, size), y + 9, size, ui(UI_DIM));
    }
    if (!nitems) gfx_utext_center(s, &font_ui, LIST_X + LIST_W / 2, BAR + 60, "this folder is empty", ui(UI_DIM));
    ui_scrollbar(s, LIST_X + LIST_W - 9, BAR + 6, H - BAR - STATUS - 12, nitems, VISIBLE, scroll);
    draw_preview(s);

    /* status line, or a question */
    int sy = H - STATUS;
    gfx_fill(s, SIDE + 1, sy, W - SIDE - 1, STATUS, list_bg);
    gfx_fill(s, SIDE + 1, sy, W - SIDE - 1, 1, ui(UI_LINE));
    int tx0 = SIDE + 14, ty = sy + (STATUS - font_ui.height) / 2;
    if (mode == MODE_NAME) {
        static const char *ask[] = { "new folder name:", "new file name:", "rename to:" };
        int tx = gfx_utext(s, &font_bold, tx0, ty, ask[name_kind], ui(UI_ACCENT)) + 8;
        tx = gfx_utext(s, &font_ui, tx, ty, input, ui(UI_TEXT));
        gfx_fill(s, tx + 1, sy + 7, 2, STATUS - 14, ui(UI_ACCENT));
        const char *hint = "enter to confirm, esc cancels";
        gfx_utext(s, &font_small, W - 14 - gfx_utext_width(&font_small, hint), ty + 1, hint, ui(UI_DIM));
    } else if (mode == MODE_CONFIRM) {
        snprintf(line, sizeof line, "delete %s%s? press y to delete, any other key to keep it.",
                 fs_get(items[sel])->name, fs_get(items[sel])->dir ? " and everything in it" : "");
        gfx_utext(s, &font_bold, tx0, ty, line, ui(UI_WARN));
    } else if (status[0]) {
        gfx_utext(s, &font_ui, tx0, ty, status, theme->pal[status_color]);
    } else {
        snprintf(line, sizeof line, "%d item%s", nitems, nitems == 1 ? "" : "s");
        gfx_utext(s, &font_ui, tx0, ty, line, ui(UI_DIM));
        const char *hint = "double-click to open, backspace goes up";
        gfx_utext(s, &font_small, W - 14 - gfx_utext_width(&font_small, hint), ty + 1, hint, ui(UI_DIM));
    }
}

/* ---- doing things -------------------------------------------------------------------- */

static void saved(void) {
    if (diskfs_autosave() < 0) say(PAL_ERR, "couldn't save to disk!");
}

static void go_to(int node) {
    dir = node;
    sel = -1;
    scroll = 0;
    status[0] = 0;
    reload();
}

static void act_up(struct window *w) {
    (void)w;
    if (dir == 0) return;
    int was = dir;
    go_to(fs_get(dir)->parent);
    select_node(was);
}

static void act_home(struct window *w)   { (void)w; go_to(fs_home()); }
static void act_folder(struct window *w) { (void)w; mode = MODE_NAME; name_kind = NAME_FOLDER; input[0] = 0; }
static void act_file(struct window *w)   { (void)w; mode = MODE_NAME; name_kind = NAME_FILE; input[0] = 0; }

static void act_rename(struct window *w) {
    (void)w;
    if (sel < 0) return;
    mode = MODE_NAME;
    name_kind = NAME_RENAME;
    strlcpy(input, fs_get(items[sel])->name, sizeof input);
}

static void act_delete(struct window *w) { (void)w; if (sel >= 0) mode = MODE_CONFIRM; }

static void commit_name(void) {
    mode = MODE_BROWSE;
    if (!input[0] || strchr(input, '/') || !strcmp(input, ".") || !strcmp(input, "..")) {
        say(PAL_ERR, "that's not a usable name");
        return;
    }
    int r;
    if (name_kind == NAME_RENAME) { int e = fs_move(items[sel], dir, input); r = e < 0 ? e : items[sel]; }
    else r = fs_create_at(dir, input, name_kind == NAME_FOLDER);
    if (r < 0) { say(PAL_ERR, fs_strerror(r)); return; }
    saved();
    reload();
    select_node(r);
    say(PAL_OK, name_kind == NAME_RENAME ? "renamed" : name_kind == NAME_FOLDER ? "folder created" : "file created");
}

static int remove_tree(int n) {
    int c;
    while (fs_get(n)->dir && (c = fs_child(n, -1)) >= 0) {
        int r = remove_tree(c);
        if (r < 0) return r;
    }
    return fs_remove(n);
}

static void do_delete(void) {
    mode = MODE_BROWSE;
    int node = items[sel];
    if (fs_is_ancestor(node, sh_cwd())) { say(PAL_ERR, "can't delete that: the terminal is inside it"); return; }
    fs_lock();
    int r = remove_tree(node);
    fs_unlock();
    if (r < 0) { say(PAL_ERR, fs_strerror(r)); return; }
    saved();
    sel = -1;
    reload();
    say(PAL_OK, "deleted");
}

/* Open: go into folders; programs and files go to the terminal (run / edit). */
static void open_item(int i) {
    int node = items[i];
    if (fs_get(node)->dir) { go_to(node); return; }
    char path[256], cmd[300];
    fs_path(node, path, sizeof path);
    if (kind_of(node) != KIND_PROGRAM) { notes_open_file(path); return; }
    if (!sh_at_prompt()) { say(PAL_WARN, "the terminal is busy. finish what's running there first."); return; }
    snprintf(cmd, sizeof cmd, "%s\n", path);
    term_inject(cmd);
    term_open();                           /* bring the terminal forward */
    say(PAL_DIM, "running it in the terminal");
}

/* ---- input ----------------------------------------------------------------------------- */

static int row_at(int x, int y) {
    if (x < LIST_X || x >= LIST_X + LIST_W || y < BAR + 6 || y >= H - STATUS) return -1;
    int i = scroll + (y - BAR - 6) / ROW;
    return i < nitems ? i : -1;
}

static void mouse(struct window *w, int x, int y, uint8_t buttons, int wheel) {
    bool click = (buttons & MOUSE_LEFT) && !(held & MOUSE_LEFT);
    held = buttons;

    int hb = -1;
    for (int i = 0; i < NBTNS; i++) if (rect_contains(btns[i].r, x, y)) hb = i;
    if (hb >= 0 && btn_disabled(hb)) hb = -1;
    int hr = row_at(x, y);
    int hp = x < SIDE && y >= 40 && (y - 40) / 36 < NPLACES && (y - 40) % 36 < 32 ? (y - 40) / 36 : -1;
    bool changed = hb != hover_btn || hr != hover || hp != hover_place;
    hover_place = hp;
    hover_btn = hb;
    hover = hr;

    if (wheel && x >= LIST_X && x < LIST_X + LIST_W) {
        scroll += wheel * 2;
        if (scroll > nitems - VISIBLE) scroll = nitems - VISIBLE;
        if (scroll < 0) scroll = 0;
        changed = true;
    }
    if (click) {
        changed = true;
        if (mode != MODE_BROWSE) mode = MODE_BROWSE;          /* clicking elsewhere cancels a question */
        status[0] = 0;
        if (hp >= 0) { int n = fs_lookup(0, places[hp].path); if (n >= 0) go_to(n); }
        else if (hb >= 0) btns[hb].act(w); else if (hr >= 0) {
            uint64_t now = pit_ticks();
            if (hr == last_click_item && now - last_click < pit_hz() * 2 / 5) {
                open_item(hr);                             /* double-click */
                last_click_item = -1;
            } else {
                sel = hr;
                last_click = now;
                last_click_item = hr;
            }
        } else if (x >= LIST_X && x < LIST_X + LIST_W && y > BAR && y < H - STATUS) {
            sel = -1;
        }
    }
    if (changed) draw(w->surf);
}

static void key(struct window *w, int k) {
    if (mode == MODE_NAME) {
        size_t n = strlen(input);
        if (k == '\n') commit_name();
        else if (k == 27) mode = MODE_BROWSE;
        else if (k == '\b') { if (n) input[n - 1] = 0; }
        else if (k >= 32 && k < 127 && n < sizeof input - 1) { input[n] = k; input[n + 1] = 0; }
    } else if (mode == MODE_CONFIRM) {
        if (k == 'y' || k == 'Y') do_delete();
        else { mode = MODE_BROWSE; say(PAL_DIM, "kept it"); }
    } else {
        status[0] = 0;
        if (k == KEY_DOWN && sel < nitems - 1) sel++;
        else if (k == KEY_UP && sel > 0) sel--;
        else if (k == KEY_UP && sel < 0 && nitems) sel = 0;
        else if (k == '\n' && sel >= 0) open_item(sel);
        else if (k == '\b') act_up(w);
        else if (k == KEY_DEL && sel >= 0) mode = MODE_CONFIRM;
        else if (k == 27) { wm_close(w); return; }
        if (sel >= 0) {
            if (sel < scroll) scroll = sel;
            if (sel >= scroll + VISIBLE) scroll = sel - VISIBLE + 1;
        }
    }
    draw(w->surf);
}

static void tick(struct window *w) {
    if (fs_generation() != seen_gen) {     /* something changed (maybe in the terminal) */
        reload();
        draw(w->surf);
    }
}

static void redraw(struct window *w) { W = w->w; H = w->h; reload(); draw(w->surf); }

const struct app files_app = { .name = "files", .mouse = mouse, .key = key, .tick = tick, .redraw = redraw, .resize = redraw, .icon = ICON_FILES };

void files_open(void) {
    struct window *w = wm_find(&files_app);
    if (w) { wm_show(w); return; }
    W = 860; H = 500;
    w = wm_open(&files_app, "files", W, H, NULL);
    if (!w) return;
    wm_resizable(w, 700, 320);
    mode = MODE_BROWSE;
    status[0] = 0;
    held = 0;
    go_to(fs_home());
    draw(w->surf);
}
