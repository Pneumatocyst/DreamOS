/* A text console, drawn onto a surface: the whole screen in text mode, or the
   terminal window on the desktop.
   The text is a grid of cells (character + palette slot). Drawing a cell paints
   its glyph over the background gradient; scrolling shifts the grid and repaints,
   so the gradient stays put while the text moves. */
#include "drivers/console.h"
#include "drivers/fb.h"
#include "drivers/theme.h"
#include "arch/cpu.h"
#include "arch/io.h"
#include "kernel.h"

#define BAR_H    28
#define MAX_COLS 256
#define MAX_ROWS 80

struct cell { char c; uint8_t color; };
static struct cell grid[MAX_ROWS][MAX_COLS];
static int cols, rows, cx, cy, ox, oy;
static uint8_t color = PAL_FG;
static bool escape;

static struct surface *S;          /* where we draw */
static bool bar;                   /* text mode has a status bar on top; windows don't */

static void draw_cell(int col, int row) {
    struct cell *k = &grid[row][col];
    int x = ox + col * FONT_W, y = oy + row * FONT_H;
    uint32_t fg = theme->pal[k->color & ~CON_PANEL];
    if (k->c == BLOCK[0]) { gfx_fill(S, x, y + 1, FONT_W, FONT_H - 2, fg); return; }
    if (k->color & CON_PANEL) gfx_fill(S, x, y, FONT_W, FONT_H, theme->bar);   /* solid panel */
    else gfx_gradient(S, x, y, FONT_W, FONT_H, theme->bg_top, theme->bg_bottom);
    if (k->c > ' ') gfx_glyph(S, x, y, (unsigned char)k->c, fg, -1);
}

void con_bar(void) {
    if (!bar) return;
    gfx_fill(S, 0, 0, S->w, BAR_H, theme->bar);
    gfx_fill(S, 0, BAR_H, S->w, 1, theme->pal[PAL_ACCENT]);
    int ty = (BAR_H - FONT_H) / 2;
    gfx_circle(S, 24, BAR_H / 2, 4, theme->pal[PAL_ACCENT]);
    gfx_text(S, 36, ty, "dream", theme->pal[PAL_FG], theme->bar);
    gfx_text(S, 36 + 6 * FONT_W, ty, "v" DREAM_VERSION, theme->pal[PAL_DIM], theme->bar);

    uint64_t s = pit_hz() ? pit_ticks() / pit_hz() : 0;
    char right[64];
    snprintf(right, sizeof right, "%s   up %02lu:%02lu:%02lu",
             theme->name, s / 3600, (s / 60) % 60, s % 60);
    gfx_text(S, S->w - 20 - gfx_text_width(right), ty, right, theme->pal[PAL_DIM], theme->bar);
}

void con_redraw(void) {
    int top = con_top();
    gfx_gradient(S, 0, top, S->w, S->h - top, theme->bg_top, theme->bg_bottom);
    con_bar();
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
            if (grid[r][c].c != ' ' || grid[r][c].color & CON_PANEL) draw_cell(c, r);
}

void con_attach(struct surface *s, bool with_bar) {
    S = s;
    bar = with_bar;
    int margin = bar ? 20 : 10, vpad = bar ? 10 : 8;
    ox = margin;
    oy = con_top() + vpad;
    cols = (S->w - 2 * margin) / FONT_W;
    rows = (S->h - oy - vpad) / FONT_H;
    if (cols > MAX_COLS) cols = MAX_COLS;
    if (rows > MAX_ROWS) rows = MAX_ROWS;
    color = PAL_FG;
    escape = false;
    con_clear();
}

/* Same as con_attach, but keep the text: the terminal window was resized. */
void con_reattach(struct surface *s) {
    uint64_t f = irq_save();
    S = s;
    int margin = bar ? 20 : 10, vpad = bar ? 10 : 8;
    cols = (S->w - 2 * margin) / FONT_W;
    rows = (S->h - oy - vpad) / FONT_H;
    if (cols > MAX_COLS) cols = MAX_COLS;
    if (rows > MAX_ROWS) rows = MAX_ROWS;
    if (rows < 1) rows = 1;
    if (cy >= rows) {                 /* keep the cursor's line on screen: drop lines off the top */
        int drop = cy - rows + 1;
        memmove(grid[0], grid[drop], sizeof(grid[0]) * (MAX_ROWS - drop));
        for (int r = MAX_ROWS - drop; r < MAX_ROWS; r++)
            for (int c = 0; c < MAX_COLS; c++) grid[r][c] = (struct cell){ ' ', PAL_FG };
        cy -= drop;
    }
    con_redraw();
    irq_restore(f);
}

void con_init(void) { con_attach(fb_screen(), true); }

/* In text mode, push what changed to the real screen. (On the desktop, the
   window manager notices the terminal window changed and does it instead.) */
void con_flush(void) { if (S == fb_screen()) fb_present(); }

void con_clear(void) {
    for (int r = 0; r < MAX_ROWS; r++)
        for (int c = 0; c < MAX_COLS; c++) grid[r][c] = (struct cell){ ' ', PAL_FG };
    cx = cy = 0;
    con_redraw();
}

static void scroll(void) {
    memmove(grid[0], grid[1], sizeof(grid[0]) * (rows - 1));
    for (int c = 0; c < MAX_COLS; c++) grid[rows - 1][c] = (struct cell){ ' ', PAL_FG };
    /* wipe the text area back to the gradient, then repaint every visible cell */
    gfx_gradient(S, ox, oy, cols * FONT_W, rows * FONT_H, theme->bg_top, theme->bg_bottom);
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
            if (grid[r][c].c != ' ' || grid[r][c].color & CON_PANEL) draw_cell(c, r);
    cy = rows - 1;
}

static void newline(void) {
    cx = 0;
    if (++cy >= rows) scroll();
}

void con_putc(char ch) {
    if (escape) {                     /* ESC + digit picks a palette slot */
        escape = false;
        if (ch >= '0' && ch < '0' + PAL_COUNT) color = ch - '0';
        return;
    }
    switch (ch) {
    case '\033': escape = true; return;
    case '\n':   newline(); color = PAL_FG; return;
    case '\r':   cx = 0; return;
    case '\t':   do con_putc(' '); while (cx % 4); return;
    case '\b':
        if (cx > 0) cx--;
        grid[cy][cx] = (struct cell){ ' ', PAL_FG };
        draw_cell(cx, cy);
        return;
    }
    if (cx >= cols) newline();
    grid[cy][cx] = (struct cell){ ch, color };
    draw_cell(cx, cy);
    cx++;
}

void con_write(const char *s) { while (*s) con_putc(*s++); }

/* A slim "beam" cursor, drawn over the cell it sits in. */
void con_cursor(bool visible) {
    if (cx >= cols) return;
    if (visible) gfx_fill(S, ox + cx * FONT_W, oy + cy * FONT_H + 1, 2, FONT_H - 2, theme->pal[PAL_ACCENT]);
    else draw_cell(cx, cy);
}

int con_cols(void) { return cols; }
int con_rows(void) { return rows; }
int con_top(void)  { return bar ? BAR_H + 1 : 0; }

/* ---- full-screen apps (the editor) ------------------------------------------- */

void con_goto(int col, int row) { cx = col; cy = row; }

/* Put one character anywhere. Only repaints if the cell actually changed,
   so an app can redraw its whole screen every keypress and stay fast. */
void con_put(int col, int row, char c, uint8_t color) {
    if (col < 0 || col >= cols || row < 0 || row >= rows) return;
    struct cell *k = &grid[row][col];
    if (k->c == c && k->color == color) return;
    k->c = c;
    k->color = color;
    draw_cell(col, row);
}

/* Like a terminal's "alternate screen": stash the shell's screen, give it back later. */
static struct cell saved[MAX_ROWS][MAX_COLS];
static int saved_cx, saved_cy;

void con_save(void) { memcpy(saved, grid, sizeof grid); saved_cx = cx; saved_cy = cy; }
void con_restore(void) { memcpy(grid, saved, sizeof grid); cx = saved_cx; cy = saved_cy; con_redraw(); }
