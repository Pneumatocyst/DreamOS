/* files: a file browser.
 *
 *   click            select            double-click / enter   open
 *   backspace        go up             arrows                 move the selection
 *   delete           delete (asks)     scroll wheel           scroll
 *
 * Opening a folder goes into it. Opening a program runs it in the terminal;
 * opening anything else opens it in notes. It also watches the filesystem's change
 * counter (sysinfo), so it updates when you change files somewhere else.
 *
 *   files [folder]   start somewhere other than home */
#include "gui.h"

#define BAR       52                    /* toolbar */
#define STATUS    30                    /* status line at the bottom */
#define ROW       32
#define SIDE      170                   /* the places on the left */
#define PREVIEW_W 240
#define NPLACES   4
#define MAXITEMS  512
#define C(slot) COLOR_UI(slot)

static int win, W = 860, H = 500;
#define LIST_X    SIDE
#define LIST_W    (W - SIDE - PREVIEW_W)
#define VISIBLE   ((H - BAR - STATUS - 8) / ROW)

enum { KIND_FOLDER, KIND_TEXT, KIND_PROGRAM };
enum { MODE_BROWSE, MODE_NAME, MODE_CONFIRM };
enum { NAME_FOLDER, NAME_FILE, NAME_RENAME };

struct item { char name[32]; int kind; struct file_stat st; };
static struct item items[MAXITEMS];
static int nitems;
static char dir[256] = "/root";          /* the folder we're looking at */
static int sel = -1, scroll, hover = -1, hover_btn = -1, hover_place = -1;
static uint64_t seen_gen;
static unsigned long last_click;
static int last_click_item = -1;
static uint8_t held;
static int mode, name_kind;
static char input[32];
static char status[96];
static int status_slot = UI_DIM;

static void act_up(void);
static void act_home(void);
static void act_folder(void);
static void act_file(void);
static void act_rename(void);
static void act_delete(void);

static struct { int x; const char *label; int glyph; void (*act)(void); bool needs_sel; } btns[] = {
    { 0, "up", GLYPH_UP, act_up, false },                 { 0, "home", GLYPH_HOME, act_home, false },
    { 0, "new folder", GLYPH_FOLDER, act_folder, false }, { 0, "new file", GLYPH_PLUS, act_file, false },
    { 0, "rename", GLYPH_PEN, act_rename, true },         { 0, "delete", GLYPH_TRASH, act_delete, true },
};
#define NBTNS (int)(sizeof btns / sizeof btns[0])

static const struct { const char *label, *path; int glyph; } places[NPLACES] = {
    { "home", "/root", GLYPH_HOME }, { "programs", "/bin", GLYPH_PROGRAM },
    { "settings", "/etc", GLYPH_CHIP }, { "everything", "/", GLYPH_ROOT },
};

static const char *const months[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* ---- paths ----------------------------------------------------------------------------------- */

static void join(char *out, size_t n, const char *folder, const char *name) {
    snprintf(out, n, "%s%s%s", folder, strcmp(folder, "/") ? "/" : "", name);
}

static void parent(char *path) {            /* /root/a/b -> /root/a, /root -> / */
    char *slash = strrchr(path, '/');
    if (!slash || slash == path) strlcpy(path, "/", 2);
    else *slash = 0;
}

static void item_path(int i, char *out, size_t n) { join(out, n, dir, items[i].name); }

/* ---- looking at the filesystem ------------------------------------------------------------------ */

static int kind_of(const char *path, bool is_dir) {
    if (is_dir) return KIND_FOLDER;
    char head[4];
    return readfile(path, head, 4) == 4 && !memcmp(head, "\x7f" "ELF", 4) ? KIND_PROGRAM : KIND_TEXT;
}

static void say(int slot, const char *msg) { strlcpy(status, msg, sizeof status); status_slot = slot; }

static void reload(void) {
    static char list[MAXITEMS * 34];
    char sel_name[32] = "";
    if (sel >= 0 && sel < nitems) strlcpy(sel_name, items[sel].name, sizeof sel_name);
    struct file_stat st;
    if (stat(dir, &st) < 0 || !st.is_dir) strlcpy(dir, "/root", sizeof dir);   /* our folder was deleted */
    long n = listdir(dir, list, sizeof list);
    nitems = 0;
    for (long i = 0; i < n && nitems < MAXITEMS; ) {
        long end = i;
        while (end < n && list[end] != '\n') end++;
        struct item it;
        memset(&it, 0, sizeof it);
        long len = end - i;
        bool is_dir = len > 0 && list[end - 1] == '/';
        if (is_dir) len--;
        if (len > 31) len = 31;
        memcpy(it.name, list + i, len);
        char path[300];
        join(path, sizeof path, dir, it.name);
        stat(path, &it.st);
        it.kind = kind_of(path, is_dir);
        /* folders first, then by name */
        int k = nitems++;
        while (k > 0 && ((items[k - 1].kind != KIND_FOLDER && it.kind == KIND_FOLDER) ||
                         ((items[k - 1].kind == KIND_FOLDER) == (it.kind == KIND_FOLDER) && strcmp(items[k - 1].name, it.name) > 0))) {
            items[k] = items[k - 1];
            k--;
        }
        items[k] = it;
        i = end + 1;
    }
    sel = -1;
    for (int i = 0; i < nitems; i++) if (sel_name[0] && !strcmp(items[i].name, sel_name)) sel = i;
    struct sysinfo si;
    sysinfo(&si);
    seen_gen = si.fs_generation;
    if (scroll > nitems - VISIBLE) scroll = nitems - VISIBLE;
    if (scroll < 0) scroll = 0;
}

static void select_name(const char *name) {
    for (int i = 0; i < nitems; i++) if (!strcmp(items[i].name, name)) sel = i;
    if (sel >= 0 && sel < scroll) scroll = sel;
    if (sel >= scroll + VISIBLE) scroll = sel - VISIBLE + 1;
}

static void size_text(char *out, size_t n, const struct item *it) {
    if (it->st.is_dir) snprintf(out, n, "%d item%s", it->st.children, it->st.children == 1 ? "" : "s");
    else if (it->st.size < 1024) snprintf(out, n, "%ld bytes", (long)it->st.size);
    else snprintf(out, n, "%ld.%ld KB", (long)(it->st.size / 1024), (long)(it->st.size % 1024 * 10 / 1024));
}

/* ---- drawing ------------------------------------------------------------------------------------- */

static void icon(int x, int y, int kind, int size) {
    if (kind == KIND_FOLDER) {
        uint32_t c = ui(UI_ACCENT);
        draw_round(x, y + size * 4 / 24, size * 10 / 24, size * 5 / 24, size / 10, mix(c, 0x000000, 70));
        draw_gradient(x, y + size * 6 / 24, size, size * 16 / 24, size / 7, mix(c, 0xffffff, 40), c);
    } else if (kind == KIND_PROGRAM) {
        draw_gradient(x + size / 12, y + size / 12, size * 20 / 24, size * 20 / 24, size / 5, mix(ui(UI_OK), 0xffffff, 30), ui(UI_OK));
        draw_glyph(GLYPH_PROGRAM, x + size / 4, y + size / 4, size / 2, C(UI_ON_ACCENT));
    } else {
        int w = size * 17 / 24, x0 = x + (size - w) / 2;
        draw_round(x0, y, w, size, size / 8, mix(ui(UI_TEXT), ui(UI_BG), 40));
        for (int j = 0; j < 3; j++)
            draw_fill(x0 + w / 5, y + size * (7 + j * 4) / 24, w * 3 / 5 - (j == 2 ? w / 5 : 0), size / 24 + 1, mix(ui(UI_DIM), ui(UI_BG), 40));
    }
}

static bool btn_disabled(int i) { return (btns[i].needs_sel && sel < 0) || (btns[i].act == act_up && !strcmp(dir, "/")); }

static void draw_preview(void) {
    int x0 = W - PREVIEW_W, y0 = BAR, h = H - BAR - STATUS;
    draw_fill(x0, y0, PREVIEW_W, h, C(UI_SIDEBAR));
    draw_fill(x0, y0, 1, h, C(UI_LINE));

    /* what we're describing: the selection, or the folder itself */
    struct item here;
    char path[300];
    if (sel >= 0) { here = items[sel]; item_path(sel, path, sizeof path); }
    else {
        memset(&here, 0, sizeof here);
        strlcpy(path, dir, sizeof path);
        strlcpy(here.name, !strcmp(dir, "/") ? "/" : strrchr(dir, '/') + 1, sizeof here.name);
        stat(dir, &here.st);
        here.kind = KIND_FOLDER;
    }
    icon(x0 + (PREVIEW_W - 64) / 2, y0 + 24, here.kind, 64);
    draw_text_center(FONT_TITLE, x0 + PREVIEW_W / 2, y0 + 100, here.name, C(UI_TEXT));
    char size[24], line[80];
    size_text(size, sizeof size, &here);
    snprintf(line, sizeof line, "%s, %s", here.kind == KIND_FOLDER ? "folder" : here.kind == KIND_PROGRAM ? "program" : "text", size);
    draw_text_center(FONT_UI, x0 + PREVIEW_W / 2, y0 + 130, line, C(UI_DIM));
    snprintf(line, sizeof line, "changed %s %u, %02u:%02u", months[(here.st.mon + 11) % 12], here.st.day, here.st.hour, here.st.min);
    draw_text_center(FONT_SMALL, x0 + PREVIEW_W / 2, y0 + 152, line, C(UI_DIM));

    int y = y0 + 186, cols = (PREVIEW_W - 32) / 8, maxy = y0 + h - 22;
    if (cols > 63) cols = 63;
    draw_round(x0 + 12, y - 12, PREVIEW_W - 24, maxy - y + 22, 10, mix(ui(UI_SIDEBAR), 0x000000, 40));
    if (here.kind == KIND_PROGRAM) {
        static const char *about[] = { "a user-mode program.", "open it to run it", "in the terminal." };
        for (int i = 0; i < 3; i++, y += 20) draw_text(FONT_UI, x0 + 24, y, about[i], C(UI_DIM));
    } else if (here.kind == KIND_FOLDER) {
        static char list[4096];
        long n = listdir(path, list, sizeof list), shown = 0;
        for (long i = 0; i < n && y < maxy - 10; y += 20, shown++) {
            long end = i;
            while (end < n && list[end] != '\n') end++;
            char name[40];
            long len = end - i < 39 ? end - i : 39;
            memcpy(name, list + i, len);
            name[len] = 0;
            draw_text_fit(FONT_UI, x0 + 24, y, name, len && name[len - 1] == '/' ? C(UI_ACCENT) : C(UI_TEXT), PREVIEW_W - 48);
            i = end + 1;
        }
        if (!shown) draw_text(FONT_UI, x0 + 24, y, "(empty)", C(UI_DIM));
    } else {
        /* the first lines of the file, in the terminal's font */
        static char data[2048];
        long len = readfile(path, data, sizeof data), i = 0;
        while (i < len && y < maxy - 10) {
            int c = 0;
            while (i < len && data[i] != '\n') {
                if (c < cols) { char ch = data[i]; line[c++] = ch >= 32 && ch < 127 ? ch : '.'; }
                i++;
            }
            line[c] = 0;
            draw_text(FONT_MONO, x0 + 22, y, line, C(UI_TEXT));
            y += 18;
            i++;
        }
        if (len <= 0) draw_text(FONT_UI, x0 + 24, y, "(empty file)", C(UI_DIM));
    }
}

static void draw(void) {
    /* places */
    draw_fill(0, 0, SIDE, H, C(UI_SIDEBAR));
    draw_fill(SIDE, 0, 1, H, C(UI_LINE));
    draw_text(FONT_SMALL, 18, 18, "places", C(UI_DIM));
    for (int i = 0; i < NPLACES; i++) {
        int y = 40 + i * 36;
        bool here = !strcmp(places[i].path, dir);
        if (here) draw_round(8, y, SIDE - 16, 32, 8, C(UI_SEL));
        else if (i == hover_place) draw_round(8, y, SIDE - 16, 32, 8, C(UI_HOVER));
        draw_glyph(places[i].glyph, 20, y + 8, 16, here ? C(UI_TEXT) : C(UI_DIM));
        draw_text(here ? FONT_BOLD : FONT_UI, 46, y + 7, places[i].label, here ? C(UI_TEXT) : C(UI_DIM));
    }
    struct sysinfo si;
    sysinfo(&si);
    char line[128];
    snprintf(line, sizeof line, "%d files, %lu KB", si.files, si.fs_bytes / 1024);
    draw_text(FONT_SMALL, 18, H - STATUS - 26, line, C(UI_DIM));

    /* toolbar: icon buttons, then where we are */
    draw_fill(SIDE + 1, 0, W - SIDE - 1, BAR, C(UI_BG));
    draw_fill(SIDE + 1, BAR - 1, W - SIDE - 1, 1, C(UI_LINE));
    int x = SIDE + 12;
    for (int i = 0; i < NBTNS; i++) {
        if (i == 2) x = W - 12 - 4 * 38;
        btns[i].x = x;
        if (btn_disabled(i)) draw_glyph(btns[i].glyph, x + 8, 18, 18, mix(ui(UI_DIM), ui(UI_BG), 120));
        else {
            if (i == hover_btn) draw_round(x, 10, 34, 32, 8, C(UI_HOVER));
            draw_glyph(btns[i].glyph, x + 8, 17, 18, i == hover_btn ? C(UI_TEXT) : C(UI_DIM));
        }
        x += 38;
    }
    /* the path as breadcrumbs: dim folders, the last one bright */
    int px = SIDE + 12 + 2 * 38 + 10, room = W - 12 - 4 * 38 - px - 10;
    char head[256];
    strlcpy(head, dir, sizeof head);
    const char *tail = "";
    if (strcmp(dir, "/")) { char *s = strrchr(head, '/'); tail = dir + (s - head) + 1; s[1] = 0; }
    int tw = text_width(FONT_BOLD, tail);
    draw_text_fit(FONT_UI, px, 17, head, C(UI_DIM), room - tw > 30 ? room - tw : 30);
    int hx = px + text_width(FONT_UI, head);
    if (hx > px + room - tw) hx = px + (room - tw > 30 ? room - tw : 30);
    draw_text_fit(FONT_BOLD, hx, 17, tail, C(UI_TEXT), px + room - hx);

    /* the list */
    draw_fill(LIST_X + 1, BAR, LIST_W - 1, H - BAR - STATUS, C(UI_BG));
    for (int i = scroll; i < nitems && i < scroll + VISIBLE; i++) {
        int y = BAR + 6 + (i - scroll) * ROW;
        if (i == sel) draw_round(LIST_X + 8, y, LIST_W - 16, ROW - 2, 8, C(UI_SEL));
        else if (i == hover) draw_round(LIST_X + 8, y, LIST_W - 16, ROW - 2, 8, C(UI_HOVER));
        icon(LIST_X + 18, y + 6, items[i].kind, 18);
        char size[24];
        size_text(size, sizeof size, &items[i]);
        int sw = text_width(FONT_SMALL, size);
        draw_text(FONT_SMALL, LIST_X + LIST_W - 22 - sw, y + 9, size, C(UI_DIM));
        draw_text_fit(FONT_UI, LIST_X + 46, y + 7, items[i].name, C(UI_TEXT), LIST_W - 46 - sw - 34);
    }
    if (!nitems) draw_text_center(FONT_UI, LIST_X + LIST_W / 2, BAR + 60, "this folder is empty", C(UI_DIM));
    draw_scrollbar(LIST_X + LIST_W - 9, BAR + 6, H - BAR - STATUS - 12, nitems, VISIBLE, scroll);
    draw_preview();

    /* status line, or a question */
    int sy = H - STATUS, tx0 = SIDE + 14, ty = sy + 6;
    draw_fill(SIDE + 1, sy, W - SIDE - 1, STATUS, C(UI_BG));
    draw_fill(SIDE + 1, sy, W - SIDE - 1, 1, C(UI_LINE));
    if (mode == MODE_NAME) {
        static const char *ask[] = { "new folder name:", "new file name:", "rename to:" };
        int tx = tx0 + text_width(FONT_BOLD, ask[name_kind]) + 8;
        draw_text(FONT_BOLD, tx0, ty, ask[name_kind], C(UI_ACCENT));
        draw_text(FONT_UI, tx, ty, input, C(UI_TEXT));
        tx += text_width(FONT_UI, input);
        draw_fill(tx + 1, sy + 7, 2, STATUS - 14, C(UI_ACCENT));
        const char *hint = "enter to confirm, esc cancels";
        draw_text(FONT_SMALL, W - 14 - text_width(FONT_SMALL, hint), ty + 1, hint, C(UI_DIM));
    } else if (mode == MODE_CONFIRM) {
        snprintf(line, sizeof line, "delete %s%s? press y to delete, any other key to keep it.",
                 items[sel].name, items[sel].kind == KIND_FOLDER ? " and everything in it" : "");
        draw_text(FONT_BOLD, tx0, ty, line, C(UI_WARN));
    } else if (status[0]) {
        draw_text(FONT_UI, tx0, ty, status, C(status_slot));
    } else {
        snprintf(line, sizeof line, "%d item%s", nitems, nitems == 1 ? "" : "s");
        draw_text(FONT_UI, tx0, ty, line, C(UI_DIM));
        const char *hint = "double-click to open, backspace goes up";
        draw_text(FONT_SMALL, W - 14 - text_width(FONT_SMALL, hint), ty + 1, hint, C(UI_DIM));
    }
}

/* ---- doing things ------------------------------------------------------------------------------ */

static void go_to(const char *path) {
    strlcpy(dir, path, sizeof dir);
    sel = -1;
    scroll = 0;
    status[0] = 0;
    reload();
}

static void act_up(void) {
    if (!strcmp(dir, "/")) return;
    char was[32], up[256];
    strlcpy(was, strrchr(dir, '/') + 1, sizeof was);
    strlcpy(up, dir, sizeof up);
    parent(up);
    go_to(up);
    select_name(was);
}

static void act_home(void)   { go_to("/root"); }
static void act_folder(void) { mode = MODE_NAME; name_kind = NAME_FOLDER; input[0] = 0; }
static void act_file(void)   { mode = MODE_NAME; name_kind = NAME_FILE; input[0] = 0; }
static void act_rename(void) {
    if (sel < 0) return;
    mode = MODE_NAME;
    name_kind = NAME_RENAME;
    strlcpy(input, items[sel].name, sizeof input);
}
static void act_delete(void) { if (sel >= 0) mode = MODE_CONFIRM; }

static void commit_name(void) {
    mode = MODE_BROWSE;
    if (!input[0] || strchr(input, '/') || !strcmp(input, ".") || !strcmp(input, "..")) {
        say(UI_ERR, "that's not a usable name");
        return;
    }
    char path[300];
    int r;
    if (name_kind == NAME_RENAME) { item_path(sel, path, sizeof path); r = rename(path, input); }
    else {
        join(path, sizeof path, dir, input);
        r = name_kind == NAME_FOLDER ? mkdir(path) : create(path);
    }
    if (r < 0) { say(UI_ERR, fs_error(r)); return; }
    reload();
    select_name(input);
    say(UI_OK, name_kind == NAME_RENAME ? "renamed" : name_kind == NAME_FOLDER ? "folder created" : "file created");
}

static void do_delete(void) {
    mode = MODE_BROWSE;
    char path[300];
    item_path(sel, path, sizeof path);
    int r = remove(path);
    if (r == E_BUSY) { say(UI_ERR, "can't delete that: the terminal is inside it"); return; }
    if (r < 0) { say(UI_ERR, fs_error(r)); return; }
    sel = -1;
    reload();
    say(UI_OK, "deleted");
}

/* Open: go into folders; programs run in the terminal; anything else opens in notes. */
static void open_item(int i) {
    char path[300], cmd[310];
    item_path(i, path, sizeof path);
    if (items[i].kind == KIND_FOLDER) { go_to(path); return; }
    if (items[i].kind == KIND_TEXT) {
        if (spawn("notes", path) < 0) say(UI_ERR, "couldn't start notes");
        return;
    }
    snprintf(cmd, sizeof cmd, "%s\n", path);
    if (terminal_run(cmd) < 0) say(UI_WARN, "the terminal is busy. finish what's running there first.");
    else say(UI_DIM, "running it in the terminal");
}

/* ---- input ------------------------------------------------------------------------------------- */

static int row_at(int x, int y) {
    if (x < LIST_X || x >= LIST_X + LIST_W || y < BAR + 6 || y >= H - STATUS) return -1;
    int i = scroll + (y - BAR - 6) / ROW;
    return i < nitems ? i : -1;
}

static void mouse(int x, int y, uint8_t buttons, int wheel) {
    bool click = (buttons & 1) && !(held & 1);
    held = buttons;

    int hb = -1;
    for (int i = 0; i < NBTNS; i++) if (x >= btns[i].x && x < btns[i].x + 34 && y >= 10 && y < 42) hb = i;
    if (hb >= 0 && btn_disabled(hb)) hb = -1;
    int hr = row_at(x, y);
    int hp = x < SIDE && y >= 40 && (y - 40) / 36 < NPLACES && (y - 40) % 36 < 32 ? (y - 40) / 36 : -1;
    bool changed = hb != hover_btn || hr != hover || hp != hover_place;
    hover_btn = hb;
    hover = hr;
    hover_place = hp;

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
        if (hp >= 0) go_to(places[hp].path);
        else if (hb >= 0) btns[hb].act();
        else if (hr >= 0) {
            unsigned long now = uptime_ms();
            if (hr == last_click_item && now - last_click < 400) {
                open_item(hr);                                 /* double-click */
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
    if (changed) draw();
}

static void key(int k) {
    if (mode == MODE_NAME) {
        size_t n = strlen(input);
        if (k == '\n') commit_name();
        else if (k == 27) mode = MODE_BROWSE;
        else if (k == '\b') { if (n) input[n - 1] = 0; }
        else if (k >= 32 && k < 127 && n < sizeof input - 1) { input[n] = k; input[n + 1] = 0; }
    } else if (mode == MODE_CONFIRM) {
        if (k == 'y' || k == 'Y') do_delete();
        else { mode = MODE_BROWSE; say(UI_DIM, "kept it"); }
    } else {
        status[0] = 0;
        if (k == KEY_DOWN && sel < nitems - 1) sel++;
        else if (k == KEY_UP && sel > 0) sel--;
        else if (k == KEY_UP && sel < 0 && nitems) sel = 0;
        else if (k == '\n' && sel >= 0) open_item(sel);
        else if (k == '\b') act_up();
        else if (k == KEY_DEL && sel >= 0) mode = MODE_CONFIRM;
        else if (k == 27) exit(0);
        if (sel >= 0) {
            if (sel < scroll) scroll = sel;
            if (sel >= scroll + VISIBLE) scroll = sel - VISIBLE + 1;
        }
    }
    draw();
}

static void tick(void) {
    struct sysinfo si;
    sysinfo(&si);
    if (si.fs_generation != seen_gen) { reload(); draw(); }   /* something changed (maybe in the terminal) */
}

static void resize(int w, int h) { W = w; H = h; reload(); draw(); }

int main(int argc, char **argv) {
    win = win_open("files", W, H, ICON_FILES, 600, 320);
    if (win < 0) { puts("files needs the desktop: type startx first.\n"); return 1; }
    struct file_stat st;
    if (argc > 1 && argv[1][0] == '/' && stat(argv[1], &st) == 0 && st.is_dir) strlcpy(dir, argv[1], sizeof dir);
    reload();
    app_run(win, &(struct app_loop){ .draw = draw, .key = key, .mouse = mouse, .tick = tick, .resize = resize, .tick_ms = 500 });
    return 0;
}
