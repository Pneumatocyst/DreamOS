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

#define W         700
#define H         450
#define BAR       44                    /* toolbar */
#define STATUS    30                    /* status line at the bottom */
#define ROW       28
#define PREVIEW_W 250
#define LIST_W    (W - PREVIEW_W)
#define VISIBLE   ((H - BAR - STATUS) / ROW)

enum { KIND_FOLDER, KIND_TEXT, KIND_PROGRAM };
enum { MODE_BROWSE, MODE_NAME, MODE_CONFIRM };
enum { NAME_FOLDER, NAME_FILE, NAME_RENAME };

static int dir;                         /* the folder we're looking at */
static int items[FS_MAX_NODES], nitems;
static int sel = -1, scroll, hover = -1, hover_btn = -1;
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

static struct { struct rect r; const char *label; void (*act)(struct window *); bool needs_sel; } btns[] = {
    { { 0 }, "up", act_up, false },          { { 0 }, "home", act_home, false },
    { { 0 }, "new folder", act_folder, false }, { { 0 }, "new file", act_file, false },
    { { 0 }, "rename", act_rename, true },   { { 0 }, "delete", act_delete, true },
};
#define NBTNS (int)(sizeof btns / sizeof btns[0])

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

static void icon(struct surface *s, int x, int y, int kind, int k) {
    uint32_t bar = theme->bar;
    if (kind == KIND_FOLDER) {
        uint32_t c = theme->pal[PAL_ACCENT];
        gfx_round(s, x, y + 1 * k, 9 * k, 6 * k, 2 * k, gfx_mix(c, 0x000000, 70));
        gfx_round(s, x, y + 4 * k, 20 * k, 15 * k, 3 * k, c);
    } else if (kind == KIND_PROGRAM) {
        gfx_round(s, x, y + 1 * k, 20 * k, 17 * k, 3 * k, theme->pal[PAL_OK]);
        gfx_line(s, x + 5 * k, y + 6 * k, x + 9 * k, y + 9 * k, 2 * k, bar);
        gfx_line(s, x + 9 * k, y + 9 * k, x + 5 * k, y + 12 * k, 2 * k, bar);
        gfx_fill(s, x + 11 * k, y + 12 * k, 5 * k, 2 * k, bar);
    } else {
        gfx_round(s, x + 3 * k, y, 15 * k, 19 * k, 2 * k, gfx_mix(theme->pal[PAL_FG], bar, 50));
        for (int j = 0; j < 3; j++) gfx_fill(s, x + 6 * k, y + (6 + j * 4) * k, 9 * k, k, gfx_mix(theme->pal[PAL_DIM], bar, 40));
    }
}

static void button(struct surface *s, int i, int x, int y) {
    int w = gfx_text_width(btns[i].label) + 20;
    btns[i].r = (struct rect){ x, y, w, 26 };
    bool disabled = (btns[i].needs_sel && sel < 0) || (btns[i].act == act_up && dir == 0);
    uint32_t bg = gfx_mix(theme->bar, theme->pal[PAL_FG], i == hover_btn && !disabled ? 45 : 22);
    gfx_round(s, x, y, w, 26, 7, bg);
    gfx_text(s, x + 10, y + 5, btns[i].label, disabled ? theme->pal[PAL_DIM] : theme->pal[PAL_FG], -1);
}

static void draw_preview(struct surface *s) {
    uint32_t bg = gfx_mix(gfx_mix(theme->bg_top, theme->bg_bottom, 128), theme->bar, 150);
    int x0 = LIST_W, y0 = BAR, h = H - BAR - STATUS;
    gfx_fill(s, x0, y0, PREVIEW_W, h, bg);
    gfx_fill(s, x0, y0, 1, h, gfx_mix(bg, theme->pal[PAL_DIM], 60));

    int node = sel >= 0 ? items[sel] : dir;
    const struct fs_node *f = fs_get(node);
    int kind = kind_of(node);
    icon(s, x0 + (PREVIEW_W - 60) / 2, y0 + 22, kind, 3);

    char line[64];
    const char *name = node == 0 ? "/" : f->name;
    gfx_text(s, x0 + (PREVIEW_W - gfx_text_width(name)) / 2, y0 + 92, name, theme->pal[PAL_FG], -1);
    char size[24];
    size_text(size, sizeof size, node);
    snprintf(line, sizeof line, "%s, %s", kind == KIND_FOLDER ? "folder" : kind == KIND_PROGRAM ? "program" : "file", size);
    gfx_text(s, x0 + (PREVIEW_W - gfx_text_width(line)) / 2, y0 + 112, line, theme->pal[PAL_DIM], -1);
    const struct rtc_time *t = &f->mtime;
    snprintf(line, sizeof line, "%s %u, %02u:%02u", rtc_month_names[(t->mon + 11) % 12], t->day, t->hour, t->min);
    gfx_text(s, x0 + (PREVIEW_W - gfx_text_width(line)) / 2, y0 + 130, line, theme->pal[PAL_DIM], -1);

    int y = y0 + 160, cols = (PREVIEW_W - 28) / FONT_W, maxy = y0 + h - 20;
    gfx_fill(s, x0 + 14, y - 8, PREVIEW_W - 28, 1, gfx_mix(bg, theme->pal[PAL_DIM], 60));
    if (kind == KIND_PROGRAM) {
        static const char *about[] = { "a user-mode program.", "double-click to run it", "in the terminal." };
        for (int i = 0; i < 3; i++, y += 18) gfx_text(s, x0 + 14, y, about[i], theme->pal[PAL_DIM], -1);
    } else if (kind == KIND_FOLDER) {
        int shown = 0;
        for (int c = fs_child(node, -1); c >= 0 && y < maxy; c = fs_child(node, c), y += 18, shown++) {
            snprintf(line, sizeof line, "%s%s", fs_get(c)->name, fs_get(c)->dir ? "/" : "");
            line[cols] = 0;
            gfx_text(s, x0 + 14, y, line, fs_get(c)->dir ? theme->pal[PAL_ACCENT] : theme->pal[PAL_FG], -1);
        }
        if (!shown) gfx_text(s, x0 + 14, y, "(empty)", theme->pal[PAL_DIM], -1);
    } else {
        /* the first lines of the file */
        fs_lock();
        const char *d = fs_data(node);
        uint32_t len = fs_get(node)->size, i = 0;
        while (i < len && y < maxy) {
            int c = 0;
            while (i < len && d[i] != '\n') {
                if (c < cols) { char ch = d[i]; line[c++] = ch >= 32 && ch < 127 ? ch : '.'; }
                i++;
            }
            line[c] = 0;
            gfx_text(s, x0 + 14, y, line, theme->pal[PAL_FG], -1);
            y += 18;
            i++;
        }
        fs_unlock();
        if (!len) gfx_text(s, x0 + 14, y, "(empty file)", theme->pal[PAL_DIM], -1);
    }
}

static void draw(struct surface *s) {
    uint32_t list_bg = gfx_mix(theme->bg_top, theme->bg_bottom, 128);

    /* toolbar */
    gfx_fill(s, 0, 0, W, BAR, theme->bar);
    gfx_fill(s, 0, BAR - 1, W, 1, gfx_mix(theme->bar, theme->pal[PAL_DIM], 70));
    int x = 10;
    for (int i = 0; i < 2; i++) { button(s, i, x, 9); x = btns[i].r.x + btns[i].r.w + 6; }
    int rx = W - 10;
    for (int i = NBTNS - 1; i >= 2; i--) { rx -= gfx_text_width(btns[i].label) + 20; button(s, i, rx, 9); rx -= 6; }
    char path[256];
    fs_path(dir, path, sizeof path);
    int room = (rx - x - 16) / FONT_W;
    const char *shown = path;
    if ((int)strlen(path) > room && room > 3) shown = path + strlen(path) - room + 2;
    if (shown != path) x = gfx_text(s, x + 8, 14, "..", theme->pal[PAL_DIM], -1) - 8;
    gfx_text(s, x + 8, 14, shown, theme->pal[PAL_FG], -1);

    /* the list */
    gfx_fill(s, 0, BAR, LIST_W, H - BAR - STATUS, list_bg);
    for (int i = scroll; i < nitems && i < scroll + VISIBLE; i++) {
        int y = BAR + 4 + (i - scroll) * ROW, node = items[i];
        if (i == sel) gfx_round(s, 6, y, LIST_W - 12, ROW - 2, 7, gfx_mix(list_bg, theme->pal[PAL_ACCENT], 70));
        else if (i == hover) gfx_round(s, 6, y, LIST_W - 12, ROW - 2, 7, gfx_mix(list_bg, theme->pal[PAL_FG], 20));
        icon(s, 16, y + 3, kind_of(node), 1);
        char name[40];
        strlcpy(name, fs_get(node)->name, sizeof name);
        gfx_text(s, 46, y + 5, name, fs_get(node)->dir ? theme->pal[PAL_ACCENT] : theme->pal[PAL_FG], -1);
        char size[24];
        size_text(size, sizeof size, node);
        gfx_text(s, LIST_W - 18 - gfx_text_width(size), y + 5, size, theme->pal[PAL_DIM], -1);
    }
    if (!nitems) gfx_text(s, (LIST_W - 20 * FONT_W) / 2, BAR + 60, "this folder is empty", theme->pal[PAL_DIM], -1);
    if (nitems > VISIBLE) {                                /* scrollbar */
        int track = H - BAR - STATUS - 8, len = track * VISIBLE / nitems;
        int pos = (track - len) * scroll / (nitems - VISIBLE);
        gfx_round(s, LIST_W - 6, BAR + 4 + pos, 3, len, 1, gfx_mix(list_bg, theme->pal[PAL_DIM], 160));
    }
    draw_preview(s);

    /* status line, or a question */
    int sy = H - STATUS;
    gfx_fill(s, 0, sy, W, STATUS, theme->bar);
    gfx_fill(s, 0, sy, W, 1, gfx_mix(theme->bar, theme->pal[PAL_DIM], 70));
    char line[128];
    if (mode == MODE_NAME) {
        static const char *ask[] = { "new folder name: ", "new file name: ", "rename to: " };
        int tx = gfx_text(s, 12, sy + 7, ask[name_kind], theme->pal[PAL_ACCENT], -1);
        tx = gfx_text(s, tx, sy + 7, input, theme->pal[PAL_FG], -1);
        gfx_fill(s, tx + 1, sy + 8, 2, FONT_H - 2, theme->pal[PAL_ACCENT]);
        gfx_text(s, W - 12 - 28 * FONT_W, sy + 7, "enter to confirm, esc cancels", theme->pal[PAL_DIM], -1);
    } else if (mode == MODE_CONFIRM) {
        snprintf(line, sizeof line, "delete %s%s? press y to delete, any other key to keep it.",
                 fs_get(items[sel])->name, fs_get(items[sel])->dir ? " and everything in it" : "");
        gfx_text(s, 12, sy + 7, line, theme->pal[PAL_WARN], -1);
    } else if (status[0]) {
        gfx_text(s, 12, sy + 7, status, theme->pal[status_color], -1);
    } else {
        snprintf(line, sizeof line, "%d item%s", nitems, nitems == 1 ? "" : "s");
        gfx_text(s, 12, sy + 7, line, theme->pal[PAL_DIM], -1);
        const char *hint = "double-click to open, backspace goes up";
        gfx_text(s, W - 12 - gfx_text_width(hint), sy + 7, hint, theme->pal[PAL_DIM], -1);
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
    if (!sh_at_prompt()) { say(PAL_WARN, "the terminal is busy. finish what's running there first."); return; }
    char path[256], cmd[300];
    fs_path(node, path, sizeof path);
    if (kind_of(node) == KIND_PROGRAM) snprintf(cmd, sizeof cmd, "%s\n", path);
    else snprintf(cmd, sizeof cmd, "edit \"%s\"\n", path);
    term_inject(cmd);
    term_open();                           /* bring the terminal forward */
    say(PAL_DIM, kind_of(node) == KIND_PROGRAM ? "running it in the terminal" : "opened in the editor, in the terminal");
}

/* ---- input ----------------------------------------------------------------------------- */

static int row_at(int x, int y) {
    if (x >= LIST_W || y < BAR + 4 || y >= H - STATUS) return -1;
    int i = scroll + (y - BAR - 4) / ROW;
    return i < nitems ? i : -1;
}

static void mouse(struct window *w, int x, int y, uint8_t buttons, int wheel) {
    bool click = (buttons & MOUSE_LEFT) && !(held & MOUSE_LEFT);
    held = buttons;

    int hb = -1;
    for (int i = 0; i < NBTNS; i++) if (rect_contains(btns[i].r, x, y)) hb = i;
    int hr = row_at(x, y);
    bool changed = hb != hover_btn || hr != hover;
    hover_btn = hb;
    hover = hr;

    if (wheel && x < LIST_W) {
        scroll += wheel * 2;
        if (scroll > nitems - VISIBLE) scroll = nitems - VISIBLE;
        if (scroll < 0) scroll = 0;
        changed = true;
    }
    if (click) {
        changed = true;
        if (mode != MODE_BROWSE) mode = MODE_BROWSE;          /* clicking elsewhere cancels a question */
        status[0] = 0;
        if (hb >= 0) {
            bool disabled = btns[hb].needs_sel && sel < 0;
            if (!disabled) btns[hb].act(w);
        } else if (hr >= 0) {
            uint64_t now = pit_ticks();
            if (hr == last_click_item && now - last_click < pit_hz() * 2 / 5) {
                open_item(hr);                             /* double-click */
                last_click_item = -1;
            } else {
                sel = hr;
                last_click = now;
                last_click_item = hr;
            }
        } else if (x < LIST_W && y > BAR && y < H - STATUS) {
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

static void redraw(struct window *w) { draw(w->surf); }

const struct app files_app = { "files", mouse, key, tick, redraw, NULL };

void files_open(void) {
    struct window *w = wm_find(&files_app);
    if (w) { wm_show(w); return; }
    w = wm_open(&files_app, "files", W, H, NULL);
    if (!w) return;
    wm_move(w, 330, WM_PANEL_H + 110);
    mode = MODE_BROWSE;
    status[0] = 0;
    held = 0;
    go_to(fs_home());
    draw(w->surf);
}
