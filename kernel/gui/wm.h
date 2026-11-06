#pragma once
#include "gfx/gfx.h"
#include <dream/abi.h>     /* UI_* colours, ICON_* / GLYPH_*, events, drawing commands */

/* The window manager / compositor.
 *
 * Every window owns a surface that its app draws into, whenever it likes. The
 * compositor builds each frame from layers, back to front:
 *
 *     wallpaper -> windows (shadow, frame, contents) -> top bar + dock (frosted glass)
 *               -> menus, notifications, launcher, lock screen -> pointer
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
    void (*resize)(struct window *w);            /* the surface has a new size: repaint it */
    int icon;                                    /* ICON_* for the dock and the launcher */
};

struct window {
    char title[48];
    int x, y;                  /* top-left of the frame, on screen */
    int w, h;                  /* size of the contents (the title bar sits above) */
    struct surface *surf;
    const struct app *app;
    void *data;
    bool hidden;               /* closed but kept (the terminal), or minimized */
    bool minimized;            /* hidden, and shown in the dock */
    bool maximized;
    int min_w, min_h;          /* 0 = can't be resized */
    struct rect restore;       /* where it was before maximizing / snapping */
    int anim;                  /* 0..256: how "there" it is (fades in and out) */
    int anim_step;             /* + fading in, - fading out, 0 still */
    int anim_then;             /* what to do when a fade-out ends */
};

#define WM_TITLE_H 38          /* title bar */
#define WM_PANEL_H 30          /* the bar along the top of the screen */

/* ---- colours every app uses, worked out from the theme ------------------------------- */
uint32_t ui(int which);

/* small widgets drawn the same way everywhere (gui/widgets.c) */
void ui_button(struct surface *s, struct rect r, const char *label, bool hot, bool primary);
void ui_icon_button(struct surface *s, struct rect r, int icon, bool hot, bool active);
void ui_field(struct surface *s, struct rect r, const char *text, const char *placeholder, bool focused, bool secret);
void ui_toggle(struct surface *s, int x, int y, bool on);
void ui_scrollbar(struct surface *s, int x, int y, int h, int total, int visible, int pos);

/* ---- icons (gui/icons.c): drawn from shapes, at any size ------------------------------ */
void icon_app(struct surface *s, int icon, int x, int y, int size);            /* a coloured tile */
void icon_glyph(struct surface *s, int glyph, int x, int y, int size, uint32_t c);

/* ---- the desktop task --------------------------------------------------------------- */

/* The desktop runs as its own task. Other tasks (the shell) don't touch its windows
   directly; they post requests, and the desktop task carries them out. */
enum { WM_REQ_PAINT = 1, WM_REQ_THEME, WM_REQ_STOP, WM_REQ_FILES, WM_REQ_LOCK, WM_REQ_NOTIFY,
       WM_REQ_OPEN, WM_REQ_WALLPAPER };

bool wm_active(void);
void wm_start(void);           /* called by the shell: set up, then start the desktop task */
void wm_stop(void);            /* called by the shell: ask the desktop to stop, wait for it */
void wm_post(int request);
void wm_notify(const char *title, const char *body);   /* a pop-up card; safe from any task */
void wm_open_app(const char *name);                    /* by name; safe from any task */
void wm_lock(void);                                    /* show the lock screen */

/* Run fn(arg) on the desktop task and wait for it to finish (from any other task).
   false if the desktop isn't running. */
bool wm_call(void (*fn)(void *), void *arg);

/* Held while drawing into a window's surface from outside the desktop task, and by
   the desktop while it swaps a surface (resize) or frees one (close). */
#include "sched/task.h"
extern struct mutex wm_surface_lock;

/* windows for user programs (gui/uwin.c) */
struct process;
void uwin_pump(void);
void uwin_owner_gone(struct process *p);
void uwin_desktop_stopping(void);
uint64_t sys_window(struct process *p, uint64_t req);
uint64_t sys_draw(struct process *p, uint64_t h, uint64_t buf, uint64_t len);
uint64_t sys_event(struct process *p, uint64_t h, uint64_t out, uint64_t wait_ms);
uint64_t sys_winctl(struct process *p, uint64_t h, uint64_t op, uint64_t arg);
uint64_t sys_measure(struct process *p, uint64_t font, uint64_t str);

/* the rest of the system calls for apps (sys/syscalls.c) */
uint64_t sys_stat(struct process *p, uint64_t path, uint64_t out);
uint64_t sys_fsop(struct process *p, uint64_t op, uint64_t path, uint64_t arg);
uint64_t sys_sysinfo(struct process *p, uint64_t out);
uint64_t sys_tasks(struct process *p, uint64_t out, uint64_t max);
uint64_t sys_config(struct process *p, uint64_t op, uint64_t key, uint64_t val);
uint64_t sys_theme(struct process *p, uint64_t op, uint64_t arg, uint64_t out);
uint64_t sys_passwd(struct process *p, uint64_t op, uint64_t pw);
uint64_t sys_spawn(struct process *p, uint64_t prog, uint64_t arg);
uint64_t sys_terminal(struct process *p, uint64_t cmd);

struct window *wm_open(const struct app *app, const char *title, int w, int h, void *data);
void wm_close(struct window *w);
void wm_show(struct window *w);
void wm_move(struct window *w, int x, int y);
void wm_resizable(struct window *w, int min_w, int min_h);
void wm_set_title(struct window *w, const char *title);
struct window *wm_find(const struct app *app);
struct window *wm_find_icon(int icon);          /* any window showing this app icon */
void wm_theme_changed(void);   /* safe to call from any task */
void wm_wallpaper_changed(void);
int  wm_screen_w(void);
int  wm_screen_h(void);

/* wallpapers (gui/wallpaper.c) */
extern const char *const wallpaper_names[];
extern const int wallpaper_count;
void wallpaper_draw(struct surface *s, const char *style);

/* the apps, and how to start them */
struct app_entry { const char *name; const char *about; int icon; void (*open)(void); bool in_dock; };
extern const struct app_entry app_list[];
extern const int app_count;
const struct app_entry *app_by_name(const char *name);

void term_open(void);
int  term_key(void);           /* next key typed into the terminal, or -1 */
void term_inject(const char *keys);
void paint_open(void);
void files_open(void);
void clock_open(void);
void about_open(void);
void notes_open(void);
void notes_open_file(const char *path);
void calc_open(void);
void monitor_open(void);
void settings_open(void);
void snake_open(void);

/* ---- settings that survive a reboot (sys/config.c, stored in /etc/dream.conf) -------- */
const char *config_get(const char *key, const char *fallback);
void config_set(const char *key, const char *value);
void config_apply(void);       /* at boot: theme etc. */
bool passwd_set(void);
bool passwd_check(const char *pw);
void passwd_change(const char *pw);   /* "" removes it */

/* the clipboard, shared by every app and program (sys/clipboard.c) */
#define CLIP_MAX (64 * 1024)
void clipboard_set(const char *text, size_t len);
size_t clipboard_get(char *out, size_t max);     /* bytes copied */
size_t clipboard_len(void);

/* CPU usage, 0..100, over the last half second or so (sys/stats.c) */
int  cpu_usage(void);
void cpu_sample(void);
