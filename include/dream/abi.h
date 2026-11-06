#pragma once
/* The agreement between the kernel and user programs: system call numbers, and
 * what the window system's messages look like. Both sides include this file, so
 * they can't disagree. Only plain C types here: no kernel internals. */
#include <stdint.h>

/* ---- system calls: number in rax, arguments in rdi, rsi, rdx, result in rax -------- */
enum {
    SYS_EXIT = 0, SYS_WRITE, SYS_READ, SYS_SLEEP, SYS_GETPID, SYS_UPTIME, SYS_READFILE,
    SYS_WRITEFILE,   /* (path, buf, len)        -> bytes written; creates the file */
    SYS_LISTDIR,     /* (path, buf, len)        -> bytes: one name per line, folders end in / */
    SYS_FILEINFO,    /* (path)                  -> size in bytes, FILE_IS_DIR, or -1 */
    SYS_WINDOW,      /* (struct win_open *)     -> window handle, or -1 (no desktop?) */
    SYS_DRAW,        /* (handle, cmds, bytes)   -> 0: draw a batch of commands */
    SYS_EVENT,       /* (handle, struct event *, wait ms) -> 1 = got one, 0 = waited, none */
    SYS_WINCTL,      /* (handle, WINCTL_*, arg) -> depends */
    SYS_MEASURE,     /* (font, text)            -> width in pixels */
    SYS_CLIPBOARD,   /* (CLIP_GET, buf, len) -> bytes copied out; (CLIP_SET, buf, len) -> 0 */
};
enum { CLIP_GET, CLIP_SET };
#define FILE_IS_DIR (-2)

/* ---- windows ------------------------------------------------------------------------- */
struct win_open {
    const char *title;
    int32_t w, h;          /* size of the contents */
    int32_t icon;          /* ICON_*: what the dock and title bar show */
    int32_t min_w, min_h;  /* 0 = fixed size; otherwise resizable down to this */
};

enum { WINCTL_TITLE = 1,   /* arg: const char *          */
       WINCTL_CLOSE,       /* the window goes away         */
       WINCTL_SIZE,        /* -> w | (h << 32)             */
       WINCTL_NOTIFY,      /* arg: const char * "title\nbody" */
       WINCTL_FRONT };     /* bring it forward              */

/* What happened. Coordinates are inside the window's contents. */
enum { EV_KEY = 1, EV_MOUSE, EV_RESIZE, EV_CLOSE, EV_REDRAW };
struct event {
    uint8_t  type;
    uint8_t  buttons;      /* EV_MOUSE: 1 left, 2 right, 4 middle */
    int16_t  x, y;         /* EV_MOUSE */
    int16_t  wheel;        /* EV_MOUSE: + down, - up */
    uint16_t key;          /* EV_KEY: ASCII, 1-26 = ctrl+letter, KEY_* for the rest */
    uint16_t w, h;         /* EV_RESIZE: the new size */
};

/* keys that aren't characters */
enum { KEY_UP = 0x80, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
       KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_DEL,
       KEY_SUPER, KEY_ALTTAB, KEY_ALTF4,
       KEY_F1 = 0x90 };     /* F1..F12 = KEY_F1 + 0..11 */

/* ---- drawing ------------------------------------------------------------------------
 * A program sends a batch of these in one SYS_DRAW; the kernel draws them into the
 * window with the system's fonts and colours. Each command is a struct dcmd, then
 * its text (if any), padded so the next command starts on a 4-byte boundary. */
enum {
    DRAW_FILL = 1,     /* x y w h c1 */
    DRAW_ROUND,        /* x y w h r c1, alpha in c2 (0 = solid) */
    DRAW_RING,         /* x y w h r c1, alpha in c2: a 1px rounded outline */
    DRAW_CIRCLE,       /* x y r c1 */
    DRAW_LINE,         /* x y -> w h (the other end), r = thickness, c1 */
    DRAW_TEXT,         /* font x y c1 text; w > 0 = cut off with "..." at that width */
    DRAW_GLYPH,        /* x y r=size arg=GLYPH_* c1 */
    DRAW_ICON,         /* x y r=size arg=ICON_* */
    DRAW_BUTTON,       /* x y w h text, arg: 1 = hovered, 2 = primary */
    DRAW_FIELD,        /* x y w h text, arg: 1 = focused, 2 = secret; c2 = placeholder? no */
    DRAW_SCROLLBAR,    /* x y h, c1 = total, c2 = visible, arg = position */
};
struct dcmd {
    uint8_t  op, font;
    uint16_t size;         /* bytes in this command, text and padding included */
    int16_t  x, y, w, h;
    int16_t  r;
    uint16_t arg;
    uint32_t c1, c2;
    char     text[];
};

/* fonts */
enum { FONT_UI, FONT_BOLD, FONT_SMALL, FONT_TITLE, FONT_BIG, FONT_DISPLAY, FONT_MONO };

/* Colours are 0xRRGGBB, or COLOR_UI(slot) for "whatever the theme uses for that". */
#define COLOR_UI(slot) (0x01000000u | (slot))
enum {
    UI_BG,        /* window background */
    UI_SIDEBAR,   /* a slightly different background for side panels and toolbars */
    UI_RAISED,    /* buttons, fields, cards */
    UI_HOVER,     /* something under the pointer */
    UI_LINE,      /* dividers and borders */
    UI_SEL,       /* the selected row */
    UI_TEXT, UI_DIM, UI_ACCENT, UI_OK, UI_WARN, UI_ERR,
    UI_ON_ACCENT, /* text that sits on an accent-coloured button */
};

enum {
    ICON_NONE, ICON_TERMINAL, ICON_FILES, ICON_NOTES, ICON_CALC, ICON_PAINT, ICON_MONITOR,
    ICON_SETTINGS, ICON_CLOCK, ICON_SNAKE, ICON_ABOUT, ICON_APPS,
    /* small glyphs, drawn in one colour */
    GLYPH_FOLDER, GLYPH_FILE, GLYPH_PROGRAM, GLYPH_UP, GLYPH_HOME, GLYPH_PLUS, GLYPH_TRASH,
    GLYPH_PEN, GLYPH_POWER, GLYPH_SEARCH, GLYPH_LOCK, GLYPH_RESTART, GLYPH_EXIT, GLYPH_SAVE,
    GLYPH_OPEN, GLYPH_NEW, GLYPH_ROOT, GLYPH_BIN, GLYPH_PALETTE, GLYPH_INFO, GLYPH_CHIP, GLYPH_BELL,
};
