#pragma once
/* Windows for programs. Open one, draw into it, react to events:
 *
 *     int win = win_open("hello", 300, 200, ICON_ABOUT, 0, 0);
 *     for (;;) {
 *         draw_fill(0, 0, 300, 200, COLOR_UI(UI_BG));
 *         draw_text(FONT_TITLE, 20, 20, "hi!", COLOR_UI(UI_TEXT));
 *         win_flush(win);                       // nothing appears until you flush
 *         struct event e;
 *         if (win_event(win, &e, 1000) && e.type == EV_CLOSE) break;
 *     }
 *
 * The draw_* calls just add commands to a buffer; win_flush sends the whole batch to
 * the kernel in one system call, and the kernel draws it with dream's own fonts,
 * colours and widgets. Colours: 0xRRGGBB, or COLOR_UI(UI_TEXT) etc. to follow the theme. */
#include "dream.h"

int  win_open(const char *title, int w, int h, int icon, int min_w, int min_h);   /* -1: no desktop */
int  win_event(int win, struct event *e, int wait_ms);   /* 1 = got one, 0 = nothing in time */
void win_flush(int win);
void win_title(int win, const char *title);
void win_close(int win);
void win_size(int win, int *w, int *h);
void win_front(int win);
void notify(int win, const char *title, const char *body);
int  text_width(int font, const char *s);

/* The theme's colours as real 0xRRGGBB values (kept up to date for you: win_event
   refreshes them when the theme changes). Use these when you need to mix shades. */
uint32_t ui(int slot);                                    /* UI_BG, UI_TEXT, ... */
const struct theme_info *current_theme(void);
uint32_t mix(uint32_t a, uint32_t b, int t);              /* t: 0 = a ... 256 = b */

void draw_fill(int x, int y, int w, int h, uint32_t c);
void draw_round(int x, int y, int w, int h, int r, uint32_t c);
void draw_round_a(int x, int y, int w, int h, int r, uint32_t c, int alpha);   /* 0..256 */
void draw_ring(int x, int y, int w, int h, int r, uint32_t c);
void draw_circle(int x, int y, int r, uint32_t c);
void draw_line(int x0, int y0, int x1, int y1, int thick, uint32_t c);
void draw_text(int font, int x, int y, const char *s, uint32_t c);
void draw_text_fit(int font, int x, int y, const char *s, uint32_t c, int max_w);
void draw_glyph(int glyph, int x, int y, int size, uint32_t c);
void draw_icon(int icon, int x, int y, int size);
void draw_button(int x, int y, int w, int h, const char *label, bool hot, bool primary);
void draw_field(int x, int y, int w, int h, const char *text, bool focused, bool secret);
void draw_scrollbar(int x, int y, int h, int total, int visible, int pos);
void draw_ring_a(int x, int y, int w, int h, int r, uint32_t c, int alpha);
void draw_gradient(int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom);
void draw_text_center(int font, int cx, int y, const char *s, uint32_t c);
void draw_toggle(int x, int y, bool on);                  /* a 40 x 22 switch */
void draw_wallpaper(int x, int y, int w, int h, const char *style);

/* A simple main loop for apps: it waits for events and calls you back. Any callback may
   be NULL. tick runs every tick_ms while nothing else happens (0 = never). It returns
   when the window is closed (if close is NULL or returns true). */
struct app_loop {
    void (*draw)(void);                                   /* repaint everything */
    void (*key)(int key);
    void (*mouse)(int x, int y, uint8_t buttons, int wheel);
    void (*resize)(int w, int h);                         /* NULL: just draw again */
    void (*tick)(void);
    bool (*close)(void);                                  /* false = stay open */
    int tick_ms;
};
void app_run(int win, const struct app_loop *a);
