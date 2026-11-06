/* notes: a text editor in a window.
 *
 *   ctrl+s  save        ctrl+n  new         ctrl+o  open (type a path)
 *   arrows, home/end, page up/down, click to move, scroll wheel to scroll
 *
 * The text is one flat buffer; lines are found by looking for '\n'. That's slow
 * for a novel but instant for anything you'd type here, and simple to follow. */
#include "gui/wm.h"
#include "fs/ramfs.h"
#include "fs/diskfs.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "arch/cpu.h"
#include "shell/shell.h"

#define CAP      (64 * 1024)
#define BAR      44
#define STATUS   28
#define LINE_H   20
#define GUTTER   52
#define PAD      12

static char text[CAP];
static int len, caret, top;            /* top: first line on screen */
static int want_col = -1;              /* column to aim for when moving up/down */
static char path[128];
static bool dirty, blink;
static enum { EDIT, ASK_OPEN, ASK_SAVE } mode;
static char ask[128];
static char msg[64];
static uint64_t msg_until;
static int hover_btn = -1;
static uint8_t held;

static struct rect btn_rect(int i) { return (struct rect){ 10 + i * 40, 7, 34, 30 }; }
static const int btn_icon[3] = { GLYPH_NEW, GLYPH_OPEN, GLYPH_SAVE };

/* ---- finding our way around the text --------------------------------------------- */

static int line_start(int pos) { while (pos > 0 && text[pos - 1] != '\n') pos--; return pos; }
static int line_end(int pos)   { while (pos < len && text[pos] != '\n') pos++; return pos; }
static int line_of(int pos)    { int n = 0; for (int i = 0; i < pos; i++) n += text[i] == '\n'; return n; }
static int count_lines(void)   { return line_of(len) + 1; }

static int pos_of_line(int line) {
    int pos = 0;
    while (line > 0 && pos < len) { if (text[pos++] == '\n') line--; }
    return pos;
}

static int visible_lines(struct window *w) { return (w->h - BAR - STATUS - 8) / LINE_H; }

static void keep_caret_visible(struct window *w) {
    int l = line_of(caret), vis = visible_lines(w);
    if (l < top) top = l;
    if (l >= top + vis) top = l - vis + 1;
    if (top < 0) top = 0;
}

static void say(const char *m) { strlcpy(msg, m, sizeof msg); msg_until = pit_ticks() + pit_hz() * 3; }

static void set_title(struct window *w) {
    char t[48];
    const char *name = path[0] ? strrchr(path, '/') + 1 : "untitled";
    snprintf(t, sizeof t, "%s%s - notes", dirty ? "* " : "", name);
    wm_set_title(w, t);
}

/* ---- drawing -------------------------------------------------------------------------- */

static void draw(struct window *w) {
    struct surface *s = w->surf;
    int W = s->w, H = s->h;
    uint32_t paper = gfx_mix(ui(UI_BG), 0x000000, 30);

    /* toolbar */
    gfx_fill(s, 0, 0, W, BAR, ui(UI_BG));
    for (int i = 0; i < 3; i++) ui_icon_button(s, btn_rect(i), btn_icon[i], hover_btn == i, false);
    struct rect field = { 136, 8, W - 136 - 12, 28 };
    if (mode == EDIT) {
        char p[140];
        snprintf(p, sizeof p, "%s", path[0] ? path : "untitled (ctrl+s to choose where to save)");
        gfx_utext_fit(s, &font_ui, field.x + 4, field.y + 5, p, path[0] ? ui(UI_TEXT) : ui(UI_DIM), field.w - 8);
    } else {
        int lw = gfx_utext(s, &font_bold, field.x, field.y + 5, mode == ASK_OPEN ? "open:" : "save as:", ui(UI_ACCENT));
        ui_field(s, (struct rect){ lw + 8, field.y, W - lw - 20, field.h }, ask, NULL, true, false);
    }
    gfx_fill(s, 0, BAR - 1, W, 1, ui(UI_LINE));

    /* the page: line numbers on the left, text on the right */
    int y0 = BAR, h = H - BAR - STATUS;
    gfx_fill(s, 0, y0, W, h, paper);
    gfx_fill(s, 0, y0, GUTTER, h, gfx_mix(paper, 0x000000, 30));
    int cur_line = line_of(caret), vis = visible_lines(w);
    int pos = pos_of_line(top);
    int cols = (W - GUTTER - PAD * 2) / FONT_W;
    for (int l = top; l < top + vis + 1 && pos <= len; l++) {
        int y = y0 + 6 + (l - top) * LINE_H;
        if (y + LINE_H > y0 + h) break;
        if (l == cur_line) gfx_fill(s, GUTTER, y, W - GUTTER, LINE_H, gfx_mix(paper, ui(UI_TEXT), 10));
        char num[8];
        snprintf(num, sizeof num, "%d", l + 1);
        gfx_utext(s, &font_small, GUTTER - 10 - gfx_utext_width(&font_small, num), y + 3, num,
                  l == cur_line ? ui(UI_TEXT) : ui(UI_DIM));
        int end = line_end(pos), c = 0;
        for (int i = pos; i < end && c < cols; i++, c++) {
            char ch = text[i] >= 32 && text[i] < 127 ? text[i] : '?';
            if (ch != ' ') gfx_glyph(s, GUTTER + PAD + c * FONT_W, y + 2, (unsigned char)ch, ui(UI_TEXT), -1);
        }
        if (l == cur_line && mode == EDIT && blink) {
            int cc = caret - pos;
            if (cc <= cols) gfx_fill(s, GUTTER + PAD + cc * FONT_W, y + 1, 2, LINE_H - 2, ui(UI_ACCENT));
        }
        pos = end + 1;
    }
    ui_scrollbar(s, W - 8, y0 + 4, h - 8, count_lines(), vis, top);

    /* status line */
    int sy = H - STATUS;
    gfx_fill(s, 0, sy, W, STATUS, ui(UI_BG));
    gfx_fill(s, 0, sy, W, 1, ui(UI_LINE));
    int words = 0;
    for (int i = 0; i < len; i++) if (text[i] > ' ' && (i == 0 || text[i - 1] <= ' ')) words++;
    char st[96];
    snprintf(st, sizeof st, "line %d, column %d     %d words     %d bytes", cur_line + 1, caret - line_start(caret) + 1, words, len);
    gfx_utext(s, &font_small, 12, sy + 7, st, ui(UI_DIM));
    const char *right = pit_ticks() < msg_until ? msg : (dirty ? "not saved" : path[0] ? "saved" : "new note");
    uint32_t rc = pit_ticks() < msg_until ? ui(UI_ACCENT) : (dirty ? ui(UI_WARN) : ui(UI_OK));
    gfx_utext(s, &font_small, W - 12 - gfx_utext_width(&font_small, right), sy + 7, right, rc);
    set_title(w);
}

/* ---- loading and saving ------------------------------------------------------------------ */

static bool load(const char *p) {
    fs_lock();
    int n = fs_lookup(fs_home(), p);
    bool ok = n >= 0 && !fs_get(n)->dir && fs_get(n)->size < CAP;
    if (ok) {
        len = fs_get(n)->size;
        memcpy(text, fs_data(n), len);
        fs_path(n, path, sizeof path);
    }
    fs_unlock();
    if (!ok) { say(n < 0 ? "no such file" : "can't open that (a folder, or too big)"); return false; }
    caret = top = 0;
    dirty = false;
    return true;
}

static void save_to(const char *p) {
    fs_lock();
    int n = fs_lookup(fs_home(), p);
    if (n == FS_ENOENT) n = fs_create(fs_home(), p, false);
    if (n >= 0 && fs_get(n)->dir) n = FS_EISDIR;
    if (n >= 0) n = fs_write(n, text, len, false) < 0 ? FS_ENOSPC : n;
    if (n >= 0) fs_path(n, path, sizeof path);
    fs_unlock();
    if (n < 0) { say(fs_strerror(n)); return; }
    dirty = false;
    say(diskfs_autosave() < 0 ? "saved (but the disk write failed!)" : "saved");
}

/* ---- editing --------------------------------------------------------------------------------- */

static void insert(const char *s, int n) {
    if (len + n >= CAP) { say("that's as long as a note can get"); return; }
    memmove(text + caret + n, text + caret, len - caret);
    memcpy(text + caret, s, n);
    len += n;
    caret += n;
    dirty = true;
}

static void erase(int at, int n) {
    if (at < 0 || at + n > len || n <= 0) return;
    memmove(text + at, text + at + n, len - at - n);
    len -= n;
    if (caret > at) caret = caret - n < at ? at : caret - n;
    dirty = true;
}

static void move_vertical(int lines) {
    int col = want_col >= 0 ? want_col : caret - line_start(caret);
    int target = line_of(caret) + lines;
    if (target < 0) target = 0;
    int pos = pos_of_line(target), end = line_end(pos);
    caret = pos + col < end ? pos + col : end;
    want_col = col;
}

static void new_note(void) { len = caret = top = 0; path[0] = 0; dirty = false; }

static void key(struct window *w, int k) {
    if (mode != EDIT) {
        size_t n = strlen(ask);
        if (k == 27) mode = EDIT;
        else if (k == '\n') {
            int m = mode;
            mode = EDIT;
            if (m == ASK_OPEN) load(ask); else save_to(ask);
        }
        else if (k == '\b') { if (n) ask[n - 1] = 0; }
        else if (k >= 32 && k < 127 && n < sizeof ask - 1) { ask[n] = (char)k; ask[n + 1] = 0; }
        draw(w);
        return;
    }
    bool vertical = false;
    switch (k) {
    case 19:   /* ctrl+s */
        if (path[0]) save_to(path);
        else { mode = ASK_SAVE; strlcpy(ask, "/root/note.txt", sizeof ask); }
        break;
    case 14:   new_note(); break;                                          /* ctrl+n */
    case 15:   mode = ASK_OPEN; strlcpy(ask, "/root/", sizeof ask); break; /* ctrl+o */
    case KEY_LEFT:  if (caret > 0) caret--; break;
    case KEY_RIGHT: if (caret < len) caret++; break;
    case KEY_UP:    move_vertical(-1); vertical = true; break;
    case KEY_DOWN:  move_vertical(1); vertical = true; break;
    case KEY_PGUP:  move_vertical(-visible_lines(w)); vertical = true; break;
    case KEY_PGDN:  move_vertical(visible_lines(w)); vertical = true; break;
    case KEY_HOME:  caret = line_start(caret); break;
    case KEY_END:   caret = line_end(caret); break;
    case KEY_DEL:   erase(caret, 1); break;
    case '\b':      if (caret > 0) erase(caret - 1, 1); break;
    case '\t':      insert("    ", 4); break;
    case '\n': {                                  /* new line, keeping the indentation */
        int s = line_start(caret), ind = 0;
        while (s + ind < caret && text[s + ind] == ' ' && ind < 32) ind++;
        char buf[33] = "\n";
        memset(buf + 1, ' ', ind);
        insert(buf, ind + 1);
        break;
    }
    default:
        if (k >= 32 && k < 127) { char c = (char)k; insert(&c, 1); }
    }
    if (!vertical) want_col = -1;
    blink = true;
    keep_caret_visible(w);
    draw(w);
}

static void mouse(struct window *w, int x, int y, uint8_t buttons, int wheel) {
    bool click = (buttons & MOUSE_LEFT) && !(held & MOUSE_LEFT);
    held = buttons;
    int hb = -1;
    for (int i = 0; i < 3; i++) if (rect_contains(btn_rect(i), x, y)) hb = i;
    bool changed = hb != hover_btn;
    hover_btn = hb;
    if (wheel) {
        top += wheel * 3;
        int max = count_lines() - visible_lines(w);
        if (top > max) top = max;
        if (top < 0) top = 0;
        changed = true;
    }
    if (click) {
        changed = true;
        if (hb == 0) new_note();
        else if (hb == 1) { mode = ASK_OPEN; strlcpy(ask, path[0] ? path : "/root/", sizeof ask); }
        else if (hb == 2) key(w, 19);
        else if (y > BAR && y < w->h - STATUS) {
            mode = EDIT;
            int line = top + (y - BAR - 6) / LINE_H, col = (x - GUTTER - PAD + FONT_W / 2) / FONT_W;
            if (line >= count_lines()) line = count_lines() - 1;
            if (col < 0) col = 0;
            int pos = pos_of_line(line), end = line_end(pos);
            caret = pos + col < end ? pos + col : end;
            want_col = -1;
            blink = true;
        }
    }
    if (changed) draw(w);
}

static void tick(struct window *w) {
    static int n;
    if (++n % 5) return;                      /* blink twice a second */
    blink = !blink;
    draw(w);
}

static void redraw(struct window *w) { draw(w); }

const struct app notes_app = { .name = "notes", .mouse = mouse, .key = key, .tick = tick,
                               .redraw = redraw, .resize = redraw, .icon = ICON_NOTES };

void notes_open(void) {
    struct window *w = wm_find(&notes_app);
    if (w) { wm_show(w); return; }
    w = wm_open(&notes_app, "notes", 700, 470, NULL);
    if (!w) return;
    wm_resizable(w, 420, 240);
    mode = EDIT;
    if (!len && !path[0]) {
        static const char hello[] = "welcome to notes.\n\nctrl+s saves, ctrl+o opens, ctrl+n starts over.\n";
        memcpy(text, hello, sizeof hello - 1);
        len = sizeof hello - 1;
        caret = len;
    }
    draw(w);
}

void notes_open_file(const char *p) {
    if (dirty && wm_find(&notes_app)) save_to(path[0] ? path : "/root/unsaved.txt");   /* don't lose work */
    load(p);
    mode = EDIT;
    notes_open();
    struct window *w = wm_find(&notes_app);
    if (w) draw(w);
}
