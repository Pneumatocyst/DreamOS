#pragma once
#include "gfx/gfx.h"

/* The window manager / compositor.
 *
 * Every window owns a surface that its app draws into, whenever it likes. The
 * compositor builds each frame from layers, back to front:
 *
 *     wallpaper -> windows (shadow, frame, title bar, contents) -> panel -> pointer
 *
 * ...but only for the parts of the screen that changed ("damage"), and only into
 * the back buffer. Then it copies those parts to the real screen. */

struct window;

/* What an app tells the window manager about itself. Any of these may be NULL. */
struct app {
    const char *name;
    void (*mouse)(struct window *w, int x, int y, uint8_t buttons, int wheel);   /* x/y inside the window */
    void (*key)(struct window *w, int key);
    void (*tick)(struct window *w);              /* about 10 times a second */
    void (*redraw)(struct window *w);            /* repaint everything (the theme changed) */
    bool (*close)(struct window *w);             /* true = destroy, false = just hide */
};

struct window {
    char title[40];
    int x, y;                  /* top-left of the frame, on screen */
    int w, h;                  /* size of the contents (the title bar sits above) */
    struct surface *surf;
    const struct app *app;
    void *data;
    bool hidden;
};

#define WM_TITLE_H 30
#define WM_PANEL_H 32

/* The desktop runs as its own task. Other tasks (the shell) don't touch its
   windows directly; they post requests, and the desktop task carries them out. */
enum { WM_REQ_PAINT = 1, WM_REQ_THEME, WM_REQ_STOP, WM_REQ_FILES };

bool wm_active(void);
void wm_start(void);           /* called by the shell: set up, then start the desktop task */
void wm_stop(void);            /* called by the shell: ask the desktop to stop, wait for it */
void wm_post(int request);

struct window *wm_open(const struct app *app, const char *title, int w, int h, void *data);
void wm_close(struct window *w);
void wm_show(struct window *w);
void wm_move(struct window *w, int x, int y);
struct window *wm_find(const struct app *app);
void wm_theme_changed(void);   /* safe to call from any task */

/* the apps */
void term_open(void);
int  term_key(void);           /* next key typed into the terminal, or -1 */
void term_inject(const char *keys);
void paint_open(void);
void files_open(void);
void clock_open(void);
void about_open(void);
