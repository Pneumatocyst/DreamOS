/* notes: a text editor, as an ordinary user program.
 *
 *   notes [file]      open a file (or start a new note)
 *
 *   ctrl+s  save        ctrl+n  new         ctrl+o  open (type a path)
 *   ctrl+a  select all  ctrl+c  copy        ctrl+x  cut        ctrl+v  paste
 *   arrows, home/end, page up/down, click to move, drag to select, wheel to scroll
 *
 * It runs in ring 3 like any program in /bin: it can only reach the outside world
 * through system calls. Files come and go with readfile/writefile; the window,
 * the drawing and the keyboard come through the window system (lib/gui.h).
 *
 * The text is one flat buffer; lines are found by looking for '\n'. That's slow
 * for a novel but instant for anything you'd type here, and simple to follow. */
#include "gui.h"

#define CAP     (64 * 1024)
#define BAR     44
#define STATUS  28
#define LINE_H  20
#define GUTTER  52
#define PAD     12
#define CHAR_W  8               /* the terminal font: every letter is 8 pixels wide */

#define C(slot) COLOR_UI(slot)

static int win, W = 700, H = 470;
static char text[CAP];
static int len, caret, top;            /* top: first line on screen */
static int want_col = -1;              /* column to aim for when moving up/down */
static int anchor = -1;                /* the other end of the selection (-1: nothing selected) */
static char path[128];
static bool dirty, blink = true;
static enum { EDIT, ASK_OPEN, ASK_SAVE } mode;
static char ask[128];
static char msg[64];
static unsigned long msg_until;
static int hover_btn = -1;
static uint8_t held;

static const int btn_icon[3] = { GLYPH_NEW, GLYPH_OPEN, GLYPH_SAVE };
static bool in_btn(int i, int x, int y) { return x >= 10 + i * 40 && x < 44 + i * 40 && y >= 7 && y < 37; }

static void copy(char *d, const char *s, size_t n) { size_t i = 0; for (; s[i] && i < n - 1; i++) d[i] = s[i]; d[i] = 0; }

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

static int visible_lines(void) { return (H - BAR - STATUS - 8) / LINE_H; }

static void keep_caret_visible(void) {
    int l = line_of(caret), vis = visible_lines();
    if (l < top) top = l;
    if (l >= top + vis) top = l - vis + 1;
    if (top < 0) top = 0;
}

/* the selection runs from the anchor to the caret, whichever way round */
static bool has_sel(void) { return anchor >= 0 && anchor != caret; }
static int sel_start(void) { return anchor < caret ? anchor : caret; }
static int sel_end(void)   { return anchor < caret ? caret : anchor; }

static void say(const char *m) { copy(msg, m, sizeof msg); msg_until = uptime_ms() + 3000; }

static const char *base_name(void) {
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
    return path[0] ? b : "untitled";
}

/* ---- drawing -------------------------------------------------------------------------- */

static void draw(void) {
    uint32_t paper = 0x000000;                       /* drawn as UI_BG darkened, below */

    /* toolbar */
    draw_fill(0, 0, W, BAR, C(UI_BG));
    for (int i = 0; i < 3; i++) {
        if (hover_btn == i) draw_round(10 + i * 40, 7, 34, 30, 8, C(UI_HOVER));
        draw_glyph(btn_icon[i], 10 + i * 40 + 8, 7 + 6, 18, hover_btn == i ? C(UI_TEXT) : C(UI_DIM));
    }
    int fx = 136;
    if (mode == EDIT) {
        draw_text_fit(FONT_UI, fx + 4, 13, path[0] ? path : "untitled (ctrl+s to choose where to save)",
                      path[0] ? C(UI_TEXT) : C(UI_DIM), W - fx - 20);
    } else {
        const char *label = mode == ASK_OPEN ? "open:" : "save as:";
        draw_text(FONT_BOLD, fx, 13, label, C(UI_ACCENT));
        int lw = fx + text_width(FONT_BOLD, label) + 8;
        draw_field(lw, 8, W - lw - 12, 28, ask, true, false);
    }
    draw_fill(0, BAR - 1, W, 1, C(UI_LINE));

    /* the page: line numbers on the left, text on the right */
    int y0 = BAR, h = H - BAR - STATUS;
    draw_fill(0, y0, W, h, C(UI_BG));
    draw_round_a(0, y0, W, h, 0, paper, 30);                 /* a shade darker than the window */
    draw_round_a(0, y0, GUTTER, h, 0, paper, 30);
    int cur_line = line_of(caret), vis = visible_lines();
    int pos = pos_of_line(top);
    int cols = (W - GUTTER - PAD * 2) / CHAR_W;
    if (cols > 250) cols = 250;
    for (int l = top; l < top + vis + 1 && pos <= len; l++) {
        int y = y0 + 6 + (l - top) * LINE_H;
        if (y + LINE_H > y0 + h) break;
        if (l == cur_line && !has_sel()) draw_round_a(GUTTER, y, W - GUTTER, LINE_H, 0, C(UI_TEXT), 10);
        if (has_sel()) {                         /* the selected part of this line */
            int end = line_end(pos), a = sel_start() > pos ? sel_start() : pos, b = sel_end() < end ? sel_end() : end;
            bool takes_newline = sel_end() > end && sel_start() <= end;    /* runs on to the next line */
            if (b > a || takes_newline) {
                int x0 = GUTTER + PAD + (a - pos) * CHAR_W, x1 = GUTTER + PAD + (b - pos) * CHAR_W + (takes_newline ? CHAR_W / 2 : 0);
                draw_round_a(x0, y + 1, x1 - x0, LINE_H - 2, 3, C(UI_ACCENT), 90);
            }
        }
        char num[12];
        int n = l + 1, k = 0;
        char rev[12];
        do { rev[k++] = '0' + n % 10; n /= 10; } while (n);
        for (int i = 0; i < k; i++) num[i] = rev[k - 1 - i];
        num[k] = 0;
        draw_text(FONT_SMALL, GUTTER - 10 - text_width(FONT_SMALL, num), y + 3, num, l == cur_line ? C(UI_TEXT) : C(UI_DIM));
        int end = line_end(pos), c = 0;
        char line[256];
        for (int i = pos; i < end && c < cols; i++) line[c++] = text[i] >= 32 && text[i] < 127 ? text[i] : '?';
        line[c] = 0;
        if (c) draw_text(FONT_MONO, GUTTER + PAD, y + 2, line, C(UI_TEXT));
        if (l == cur_line && mode == EDIT && blink && caret - pos <= cols)
            draw_fill(GUTTER + PAD + (caret - pos) * CHAR_W, y + 1, 2, LINE_H - 2, C(UI_ACCENT));
        pos = end + 1;
    }
    draw_scrollbar(W - 8, y0 + 4, h - 8, count_lines(), vis, top);

    /* status line */
    int sy = H - STATUS;
    draw_fill(0, sy, W, STATUS, C(UI_BG));
    draw_fill(0, sy, W, 1, C(UI_LINE));
    int words = 0;
    for (int i = 0; i < len; i++) if (text[i] > ' ' && (i == 0 || text[i - 1] <= ' ')) words++;
    char st[96];
    snprintf(st, sizeof st, "line %d, column %d     %d words     %d bytes",
             cur_line + 1, caret - line_start(caret) + 1, words, len);
    draw_text(FONT_SMALL, 12, sy + 7, st, C(UI_DIM));
    bool fresh = uptime_ms() < msg_until;
    const char *right = fresh ? msg : dirty ? "not saved" : path[0] ? "saved" : "new note";
    uint32_t rc = fresh ? C(UI_ACCENT) : dirty ? C(UI_WARN) : C(UI_OK);
    draw_text(FONT_SMALL, W - 12 - text_width(FONT_SMALL, right), sy + 7, right, rc);

    static char last_title[64];
    char t[64];
    snprintf(t, sizeof t, "%s%s - notes", dirty ? "* " : "", base_name());
    if (strcmp(t, last_title)) { win_title(win, t); copy(last_title, t, sizeof last_title); }
    win_flush(win);
}

/* ---- loading and saving ------------------------------------------------------------------ */

static bool load(const char *p) {
    long size = fileinfo(p);
    if (size == -1) { say("no such file"); return false; }
    if (size == FILE_IS_DIR || size >= CAP) { say("can't open that (a folder, or too big)"); return false; }
    long n = readfile(p, text, CAP - 1);
    if (n < 0) { say("couldn't read it"); return false; }
    len = (int)n;
    copy(path, p, sizeof path);
    caret = top = 0;
    dirty = false;
    return true;
}

static void save_to(const char *p) {
    if (writefile(p, text, len) < 0) { say("couldn't save there"); return; }
    copy(path, p, sizeof path);
    dirty = false;
    say("saved");
}

/* ---- editing --------------------------------------------------------------------------------- */

static void insert(const char *s, int n) {
    if (len + n >= CAP) { say("that's as long as a note can get"); return; }
    for (int i = len - 1; i >= caret; i--) text[i + n] = text[i];
    memcpy(text + caret, s, n);
    len += n;
    caret += n;
    dirty = true;
}

static void erase(int at, int n) {
    if (at < 0 || at + n > len || n <= 0) return;
    for (int i = at; i + n < len; i++) text[i] = text[i + n];
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

static void new_note(void) { len = caret = top = 0; anchor = -1; path[0] = 0; dirty = false; }

static void delete_selection(void) {
    if (!has_sel()) { anchor = -1; return; }
    int a = sel_start(), b = sel_end();
    caret = b;
    erase(a, b - a);
    caret = a;
    anchor = -1;
}

static void copy_selection(void) {
    if (has_sel()) { clip_set(text + sel_start(), sel_end() - sel_start()); say("copied"); }
}

static void paste(void) {
    static char clip[CAP];
    long n = clip_get(clip, sizeof clip);
    delete_selection();
    int j = 0;
    for (long i = 0; i < n; i++) if (clip[i] == '\n' || clip[i] == '\t' || (clip[i] >= 32 && clip[i] < 127)) clip[j++] = clip[i];
    if (j) insert(clip, j);
}

static void key(int k) {
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
        return;
    }
    bool vertical = false;
    /* typing over a selection replaces it; delete/backspace just remove it */
    if (has_sel() && ((k >= 32 && k < 127) || k == '\n' || k == '\t')) delete_selection();
    else if (has_sel() && (k == '\b' || k == KEY_DEL)) { delete_selection(); keep_caret_visible(); return; }
    switch (k) {
    case 1:    anchor = 0; caret = len; return;                       /* ctrl+a */
    case 3:    copy_selection(); return;                              /* ctrl+c */
    case 24:   copy_selection(); delete_selection(); break;           /* ctrl+x */
    case 22:   paste(); break;                                        /* ctrl+v */
    case 19:   /* ctrl+s */
        if (path[0]) save_to(path);
        else { mode = ASK_SAVE; copy(ask, "/root/note.txt", sizeof ask); }
        break;
    case 14:   new_note(); break;                                     /* ctrl+n */
    case 15:   mode = ASK_OPEN; copy(ask, "/root/", sizeof ask); break; /* ctrl+o */
    case KEY_LEFT:  if (caret > 0) caret--; break;
    case KEY_RIGHT: if (caret < len) caret++; break;
    case KEY_UP:    move_vertical(-1); vertical = true; break;
    case KEY_DOWN:  move_vertical(1); vertical = true; break;
    case KEY_PGUP:  move_vertical(-visible_lines()); vertical = true; break;
    case KEY_PGDN:  move_vertical(visible_lines()); vertical = true; break;
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
    if (k >= KEY_UP && k <= KEY_PGDN) anchor = -1;  /* moving the caret drops the selection */
    blink = true;
    keep_caret_visible();
}

static int pos_at(int x, int y) {               /* the text position under a point */
    int line = top + (y - BAR - 6) / LINE_H, col = (x - GUTTER - PAD + CHAR_W / 2) / CHAR_W;
    if (line >= count_lines()) line = count_lines() - 1;
    if (line < 0) line = 0;
    if (col < 0) col = 0;
    int pos = pos_of_line(line), end = line_end(pos);
    return pos + col < end ? pos + col : end;
}

static bool selecting;

static void mouse(int x, int y, uint8_t buttons, int wheel) {
    bool click = (buttons & 1) && !(held & 1);
    held = buttons;
    if (selecting) {                                /* dragging: the caret follows the pointer */
        if (buttons & 1) { caret = pos_at(x, y); keep_caret_visible(); }
        else { selecting = false; if (anchor == caret) anchor = -1; }
        return;
    }
    hover_btn = -1;
    for (int i = 0; i < 3; i++) if (in_btn(i, x, y)) hover_btn = i;
    if (wheel) {
        top += wheel * 3;
        int max = count_lines() - visible_lines();
        if (top > max) top = max;
        if (top < 0) top = 0;
    }
    if (!click) return;
    if (hover_btn == 0) new_note();
    else if (hover_btn == 1) { mode = ASK_OPEN; copy(ask, path[0] ? path : "/root/", sizeof ask); }
    else if (hover_btn == 2) key(19);
    else if (y > BAR && y < H - STATUS) {
        mode = EDIT;
        caret = anchor = pos_at(x, y);              /* start a selection; a plain click leaves it empty */
        selecting = true;
        want_col = -1;
        blink = true;
    }
}

int main(int argc, char **argv) {
    win = win_open("notes", W, H, ICON_NOTES, 420, 240);
    if (win < 0) { puts("notes needs the desktop: type startx first.\n"); return 1; }
    if (argc > 1) {
        if (!load(argv[1]) && fileinfo(argv[1]) == -1) copy(path, argv[1], sizeof path);   /* a new file */
    } else {
        static const char hello[] = "welcome to notes.\n\nctrl+s saves, ctrl+o opens, ctrl+n starts over.\n";
        len = sizeof hello - 1;
        memcpy(text, hello, len);
        caret = len;
    }
    draw();
    for (;;) {
        struct event e;
        if (!win_event(win, &e, 500)) { blink = !blink; draw(); continue; }   /* blink the caret */
        switch (e.type) {
        case EV_CLOSE:
            if (dirty) save_to(path[0] ? path : "/root/unsaved.txt");      /* don't lose work */
            return 0;
        case EV_RESIZE: W = e.w; H = e.h; keep_caret_visible(); break;
        case EV_KEY:    key(e.key); break;
        case EV_MOUSE:  mouse(e.x, e.y, e.buttons, e.wheel); break;
        }
        draw();
    }
}
