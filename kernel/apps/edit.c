/* edit: a small nano-style text editor.
 *
 * The whole file lives in one flat array. The cursor is just an index into it:
 * typing a letter shifts everything after the cursor one byte right, backspace
 * shifts it back. Simple, and fast enough for files up to the 1 MiB limit.
 *
 * Each keypress, we redraw the whole screen into the console grid. The console
 * only repaints cells that actually changed, so this stays quick.
 *
 *   ^S save   ^Q quit   ^K cut line   ^U paste   ^F find   ^G go to line
 */
#include "shell/shell.h"
#include "drivers/console.h"
#include "drivers/keyboard.h"
#include "drivers/theme.h"
#include "fs/ramfs.h"
#include "fs/diskfs.h"
#include "mm/heap.h"

#define CAP    FS_FILE_MAX
#define GUTTER 6                       /* "  12  " line numbers */
#define PANEL(c) ((c) | CON_PANEL)

static char  *buf;                     /* kmalloc'd while the editor is open */
static size_t len, cur;                /* file length, cursor position (bytes) */
static size_t want_col;                /* column up/down try to stay in */
static int    top, left;               /* first line / column on screen */
static bool   modified;
static const char *name;

static char  *clip;                    /* ^K / ^U clipboard, kept between edits */
static size_t clip_len;
static bool   last_was_cut;

static char   msg[128];
static uint8_t msg_color;
static bool   prompting;               /* cursor belongs to the message line */

static int rows, cols, text_rows;

/* ---- finding our way around the text ------------------------------------------- */

static size_t line_start(size_t i) { while (i > 0 && buf[i - 1] != '\n') i--; return i; }
static size_t line_end(size_t i)   { while (i < len && buf[i] != '\n') i++; return i; }

static int line_of(size_t i) {
    int n = 0;
    for (size_t k = 0; k < i; k++) if (buf[k] == '\n') n++;
    return n;
}

static size_t start_of_line(int ln) {
    size_t i = 0;
    while (ln > 0 && i < len) if (buf[i++] == '\n') ln--;
    return i;
}

static void set_msg(uint8_t color, const char *text) {
    strlcpy(msg, text, sizeof msg);
    msg_color = color;
}

/* ---- drawing ------------------------------------------------------------------- */

static int put_str(int col, int row, const char *s, uint8_t color) {
    for (; *s && col < cols; s++) con_put(col++, row, *s, color);
    return col;
}

static void fill_to_end(int col, int row, uint8_t color) {
    while (col < cols) con_put(col++, row, ' ', color);
}

static void render(void) {
    int ln = line_of(cur), lines = line_of(len) + 1;
    int col = cur - line_start(cur);
    int width = cols - GUTTER;

    /* scroll so the cursor is always visible */
    if (ln < top) top = ln;
    if (ln >= top + text_rows) top = ln - text_rows + 1;
    if (col < left) left = col;
    if (col >= left + width) left = col - width + 1;

    /* header */
    char info[64];
    int c = put_str(0, 0, "  edit  ", PANEL(PAL_ACCENT));
    c = put_str(c, 0, name, PANEL(PAL_FG));
    if (modified) c = put_str(c, 0, "  [modified]", PANEL(PAL_WARN));
    snprintf(info, sizeof info, "ln %d/%d  col %d  %lu bytes  ", ln + 1, lines, col + 1,
             (uint64_t)len);
    int ic = cols - (int)strlen(info);
    fill_to_end(c, 0, PANEL(PAL_DIM));
    if (ic > c) put_str(ic, 0, info, PANEL(PAL_DIM));

    /* the text, with line numbers */
    size_t i = start_of_line(top);
    for (int r = 0; r < text_rows; r++) {
        int row = r + 1, n = top + r;
        if (n >= lines) {
            put_str(0, row, "   ~  ", PAL_DIM);
            fill_to_end(GUTTER, row, PAL_FG);
            continue;
        }
        char num[8];
        snprintf(num, sizeof num, "%4d  ", n + 1);
        put_str(0, row, num, n == ln ? PAL_ACCENT : PAL_DIM);

        size_t end = line_end(i);
        for (int x = 0; x < width; x++) {
            size_t at = i + left + x;
            char ch = at < end ? buf[at] : ' ';
            if (ch == '\t') ch = ' ';
            else if ((ch < 32 || ch > 126) && ch != ' ') ch = '.';
            con_put(GUTTER + x, row, ch, PAL_FG);
        }
        i = end < len ? end + 1 : len;
    }

    /* message line and the shortcut bar */
    c = put_str(1, rows - 2, msg, msg_color);
    con_put(0, rows - 2, ' ', PAL_FG);
    fill_to_end(c, rows - 2, PAL_FG);

    static const char *keys[][2] = {
        { "^S", "save" }, { "^Q", "quit" }, { "^K", "cut line" }, { "^U", "paste" },
        { "^F", "find" }, { "^G", "go to line" },
    };
    c = put_str(0, rows - 1, " ", PANEL(PAL_FG));
    for (unsigned k = 0; k < sizeof keys / sizeof keys[0]; k++) {
        c = put_str(c, rows - 1, keys[k][0], PANEL(PAL_ACCENT));
        c = put_str(c, rows - 1, " ", PANEL(PAL_DIM));
        c = put_str(c, rows - 1, keys[k][1], PANEL(PAL_DIM));
        c = put_str(c, rows - 1, "   ", PANEL(PAL_DIM));
    }
    fill_to_end(c, rows - 1, PANEL(PAL_DIM));

    if (prompting) con_goto(1 + strlen(msg), rows - 2);
    else con_goto(GUTTER + col - left, 1 + ln - top);
}

/* ---- little prompts on the message line ---------------------------------------- */

/* Ask for a line of text. false if cancelled with Esc or ^C. */
static bool ask_text(const char *label, char *out, size_t n) {
    size_t l = strlen(out);
    prompting = true;
    for (;;) {
        snprintf(msg, sizeof msg, "%s%s", label, out);
        msg_color = PAL_ACCENT;
        render();
        int k = sh_wait_key();
        if (k == '\n') break;
        if (k == 27 || k == 3) { prompting = false; set_msg(PAL_DIM, "cancelled"); return false; }
        if (k == '\b' && l) out[--l] = 0;
        else if (k >= 32 && k < 127 && l + 1 < n) { out[l++] = k; out[l] = 0; }
    }
    prompting = false;
    msg[0] = 0;
    return true;
}

/* Ask a yes/no question. Returns 'y', 'n', or 0 for cancel. */
static int ask_yn(const char *question) {
    prompting = true;
    set_msg(PAL_WARN, question);
    for (;;) {
        render();
        int k = sh_wait_key();
        if (k == 27 || k == 3) { prompting = false; set_msg(PAL_DIM, "cancelled"); return 0; }
        k |= 32;                         /* lower-case it, so Y and N work too */
        if (k == 'y' || k == 'n') { prompting = false; msg[0] = 0; return k; }
    }
}

/* ---- editing ------------------------------------------------------------------- */

static bool insert(const char *s, size_t n) {
    if (len + n > CAP) { set_msg(PAL_ERR, "the file is full (1 MiB is the limit)"); return false; }
    memmove(buf + cur + n, buf + cur, len - cur);
    memcpy(buf + cur, s, n);
    len += n;
    cur += n;
    modified = true;
    return true;
}

static void erase(size_t at, size_t n) {
    memmove(buf + at, buf + at + n, len - at - n);
    len -= n;
    modified = true;
}

static void newline(void) {              /* keep the current line's indentation */
    size_t ls = line_start(cur), indent = 0;
    while (ls + indent < cur && buf[ls + indent] == ' ') indent++;
    if (!insert("\n", 1)) return;
    insert(buf + ls, indent);
}

static void cut_line(void) {
    size_t ls = line_start(cur), le = line_end(cur);
    if (le < len) le++;                  /* take the newline too */
    if (ls == le) return;
    if (!last_was_cut) clip_len = 0;     /* consecutive ^K's pile up, like nano */
    size_t n = le - ls;
    if (clip_len + n > CAP) n = CAP - clip_len;
    memcpy(clip + clip_len, buf + ls, n);
    clip_len += n;
    erase(ls, le - ls);
    cur = ls;
}

static bool save(void) {
    int f = fs_lookup(sh_cwd(), name);
    if (f == FS_ENOENT) f = fs_create(sh_cwd(), name, false);
    if (f >= 0 && fs_get(f)->dir) f = FS_EISDIR;
    if (f >= 0) f = fs_write(f, buf, len, false);
    if (f < 0) { set_msg(PAL_ERR, fs_strerror(f)); return false; }
    modified = false;

    int r = diskfs_autosave();
    snprintf(msg, sizeof msg, "saved %lu bytes%s", (uint64_t)len,
             r < 0 ? " (but writing to disk failed!)" : diskfs_mounted() ? " to disk" : " (RAM only)");
    msg_color = r < 0 ? PAL_ERR : PAL_OK;
    return r >= 0;
}

static void find(void) {
    static char query[64];
    if (!ask_text("find: ", query, sizeof query) || !query[0]) return;
    size_t ql = strlen(query);
    for (size_t step = 1; step <= len; step++) {            /* search forward, wrapping */
        size_t at = (cur + step) % (len + 1);
        if (at + ql <= len && !memcmp(buf + at, query, ql)) {
            cur = at;
            want_col = cur - line_start(cur);
            set_msg(PAL_DIM, step > len - cur && at < cur ? "found (wrapped to the top)" : "found");
            return;
        }
    }
    set_msg(PAL_WARN, "not found");
}

static void go_to_line(void) {
    char in[12] = "";
    uint64_t n;
    if (!ask_text("go to line: ", in, sizeof in)) return;
    if (!sh_parse_num(in, &n) || n == 0) { set_msg(PAL_ERR, "that's not a line number"); return; }
    cur = start_of_line(n - 1);
    want_col = 0;
}

static void move_vertical(int dir) {
    size_t ls = line_start(cur);
    if (dir < 0) {
        if (ls == 0) { cur = 0; return; }
        size_t prev = line_start(ls - 1);
        cur = prev + want_col < ls - 1 ? prev + want_col : ls - 1;
    } else {
        size_t le = line_end(cur);
        if (le == len) { cur = len; return; }
        size_t next = le + 1, next_end = line_end(next);
        cur = next + want_col < next_end ? next + want_col : next_end;
    }
}

/* ---- the main loop ------------------------------------------------------------- */

SH_CMD(edit) {
    if (argc != 2) { kerrorf("usage: edit <file>\n"); return; }
    if (!sh_is_tty()) { kerrorf(C_ERR "edit" C_DIM ": can't run inside a pipe or redirect\n"); return; }

    if (!clip) clip = kmalloc(CAP);
    buf = kmalloc(CAP);
    if (!buf || !clip) { kfree(buf); kerrorf(C_ERR "edit" C_DIM ": out of memory\n"); return; }

    name = argv[1];
    len = cur = want_col = 0;
    top = left = 0;
    modified = last_was_cut = prompting = false;

    int f = fs_lookup(sh_cwd(), name);
    if (f >= 0) {
        if (fs_get(f)->dir) { sh_perror("edit", name, FS_EISDIR); kfree(buf); return; }
        len = fs_get(f)->size;
        memcpy(buf, fs_data(f), len);
        set_msg(PAL_DIM, "");
    } else if (f == FS_ENOENT) {
        char leaf[FS_NAME_MAX];
        int p = fs_parent_of(sh_cwd(), name, leaf);
        if (p < 0) { sh_perror("edit", name, p); kfree(buf); return; }
        set_msg(PAL_DIM, "new file: it's created when you save");
    } else { sh_perror("edit", name, f); kfree(buf); return; }

    rows = con_rows();
    cols = con_cols();
    text_rows = rows - 3;                /* header, message line, shortcut bar */
    con_save();

    for (;;) {
        render();
        int k = sh_wait_key();
        bool cutting = false;
        if (k != 0x13) msg[0] = 0;

        switch (k) {
        case KEY_LEFT:  if (cur) cur--; want_col = cur - line_start(cur); break;
        case KEY_RIGHT: if (cur < len) cur++; want_col = cur - line_start(cur); break;
        case KEY_UP:    move_vertical(-1); break;
        case KEY_DOWN:  move_vertical(+1); break;
        case KEY_HOME:  cur = line_start(cur); want_col = 0; break;
        case KEY_END:   cur = line_end(cur); want_col = cur - line_start(cur); break;
        case KEY_PGUP:  for (int i = 0; i < text_rows; i++) move_vertical(-1); break;
        case KEY_PGDN:  for (int i = 0; i < text_rows; i++) move_vertical(+1); break;

        case '\n':      newline(); want_col = cur - line_start(cur); break;
        case '\t':      insert("    ", 4); want_col = cur - line_start(cur); break;
        case '\b':
            if (cur) { cur--; erase(cur, 1); }
            want_col = cur - line_start(cur);
            break;
        case KEY_DEL:   if (cur < len) erase(cur, 1); break;

        case 0x13: save(); break;                               /* ^S */
        case 0x0b: cut_line(); cutting = true; want_col = 0; break;   /* ^K */
        case 0x15:                                              /* ^U */
            if (clip_len) insert(clip, clip_len);
            want_col = cur - line_start(cur);
            break;
        case 0x06: find(); break;                               /* ^F */
        case 0x07: go_to_line(); break;                         /* ^G */

        case 0x11: case 0x18: {                                 /* ^Q or ^X: quit */
            if (modified) {
                int a = ask_yn("save changes before quitting? (y = save, n = discard, esc = keep editing)");
                if (a == 0) break;
                if (a == 'y' && !save()) break;
            }
            con_restore();
            kfree(buf);
            buf = NULL;
            return;
        }
        default:
            if (k >= 32 && k < 127) {
                char ch = k;
                insert(&ch, 1);
                want_col = cur - line_start(cur);
            }
        }
        last_was_cut = cutting;
    }
}
