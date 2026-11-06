/* Windows for user programs.
 *
 * A program in /bin can't touch the window manager's memory: it lives in ring 3, in
 * its own address space. So it works through system calls, the way an X11 or
 * Wayland client talks to its display server:
 *
 *   SYS_WINDOW   ask for a window               -> a handle (a small number)
 *   SYS_DRAW     send a batch of drawing commands ("a rounded box here, text there")
 *   SYS_EVENT    wait for something to happen (a key, a click, a resize, close)
 *   SYS_WINCTL   change the title, close it, ...
 *
 * The kernel does the actual drawing, with the same fonts, colours and widgets as
 * the built-in apps, so programs stay small and look like the rest of dream.
 *
 * Two tasks are involved: the program's own (making system calls) and the desktop's
 * (which owns the window list). Window creation and destruction happen on the desktop
 * task (wm_call); drawing happens on the program's task, under wm_surface_lock so a
 * resize can't swap the surface out from under it. */
#include "gui/wm.h"
#include "proc/process.h"
#include "sched/task.h"
#include "mm/vmm.h"
#include "arch/cpu.h"
#include "arch/io.h"

#define MAX_UWIN 16
#define QLEN     64
#define BAD      ((uint64_t)-1)

struct uwin {
    bool used;
    bool dead;                    /* its program ended or closed it: the desktop removes it */
    struct process *owner;
    struct window *w;             /* NULL once the window itself is gone */
    struct app app;               /* each one its own, so it can have its own icon */
    struct event q[QLEN];
    volatile unsigned qh, qt;
    /* filled in by sys_window for the desktop task to use */
    char title[48];
    int width, height, min_w, min_h;
};
static struct uwin uw[MAX_UWIN];

/* ---- events: the desktop task puts them in, the program takes them out ------------ */

static void push(struct uwin *u, struct event e) {
    uint64_t f = irq_save();
    unsigned last = (u->qh + QLEN - 1) % QLEN;
    /* a stream of mouse moves: keep only the newest, so a busy program doesn't fall behind */
    if (e.type == EV_MOUSE && u->qh != u->qt && u->q[last].type == EV_MOUSE &&
        u->q[last].buttons == e.buttons && !u->q[last].wheel && !e.wheel) {
        u->q[last] = e;
    } else {
        unsigned next = (u->qh + 1) % QLEN;
        if (next != u->qt) { u->q[u->qh] = e; u->qh = next; }
    }
    irq_restore(f);
}

static void on_mouse(struct window *w, int x, int y, uint8_t buttons, int wheel) {
    push(w->data, (struct event){ .type = EV_MOUSE, .buttons = buttons, .x = x, .y = y, .wheel = wheel });
}
static void on_key(struct window *w, int k) { push(w->data, (struct event){ .type = EV_KEY, .key = k }); }
static void on_redraw(struct window *w) { push(w->data, (struct event){ .type = EV_REDRAW }); }
static void on_resize(struct window *w) {
    push(w->data, (struct event){ .type = EV_RESIZE, .w = w->w, .h = w->h });
}
static bool on_close(struct window *w) {
    /* Hide it and tell the program; it decides (it might want to save first). */
    push(w->data, (struct event){ .type = EV_CLOSE });
    return false;
}

/* ---- jobs the desktop task runs for us --------------------------------------------- */

static void do_create(void *arg) {
    struct uwin *u = arg;
    u->w = wm_open(&u->app, u->title, u->width, u->height, u);
    if (u->w && u->min_w) wm_resizable(u->w, u->min_w, u->min_h);
}

static void do_title(void *arg) { struct uwin *u = arg; if (u->w) wm_set_title(u->w, u->title); }
static void do_front(void *arg) { struct uwin *u = arg; if (u->w) wm_show(u->w); }

/* Called by the desktop task every frame: take away windows whose programs are done. */
void uwin_pump(void) {
    for (int i = 0; i < MAX_UWIN; i++) {
        struct uwin *u = &uw[i];
        if (!u->used || !u->dead) continue;
        if (u->w) {
            mutex_lock(&wm_surface_lock);
            struct window *w = u->w;
            u->w = NULL;
            mutex_unlock(&wm_surface_lock);
            wm_close(w);
        }
        if (!u->owner) u->used = false;          /* the program is gone too: slot's free */
    }
    process_reap();
}

/* The program ended (normally, crashed or was stopped). Runs on its task, interrupts off. */
void uwin_owner_gone(struct process *p) {
    for (int i = 0; i < MAX_UWIN; i++) {
        if (!uw[i].used || uw[i].owner != p) continue;
        uw[i].dead = true;
        uw[i].owner = NULL;
        if (!uw[i].w) uw[i].used = false;
    }
}

/* The desktop is shutting down: its windows are about to vanish. Programs lose them. */
void uwin_desktop_stopping(void) {
    mutex_lock(&wm_surface_lock);
    for (int i = 0; i < MAX_UWIN; i++) {
        if (!uw[i].used) continue;
        uw[i].w = NULL;
        uw[i].dead = true;
        if (uw[i].owner) uw[i].owner->killed = true;   /* like ctrl+c */
        else uw[i].used = false;
    }
    mutex_unlock(&wm_surface_lock);
}

/* ---- the system calls ------------------------------------------------------------------ */

static struct uwin *mine(struct process *p, uint64_t h) {
    if (h >= MAX_UWIN || !uw[h].used || uw[h].owner != p || uw[h].dead) return NULL;
    return &uw[h];
}

uint64_t sys_window(struct process *p, uint64_t req) {
    struct win_open o;
    if (!wm_active() || !vmm_user_ok(p->pml4, req, sizeof o, false)) return BAD;
    memcpy(&o, (const void *)req, sizeof o);
    if (o.w < 40 || o.h < 20 || o.w > wm_screen_w() || o.h > wm_screen_h() - WM_PANEL_H - WM_TITLE_H) return BAD;

    struct uwin *u = NULL;
    uint64_t f = irq_save();
    for (int i = 0; i < MAX_UWIN && !u; i++) if (!uw[i].used) u = &uw[i];
    if (u) { memset(u, 0, sizeof *u); u->used = true; u->owner = p; }
    irq_restore(f);
    if (!u) return BAD;

    if (!user_string(p, (uint64_t)o.title, u->title, sizeof u->title)) strlcpy(u->title, p->name, sizeof u->title);
    u->width = o.w;
    u->height = o.h;
    u->min_w = o.min_w > 0 ? o.min_w : 0;
    u->min_h = o.min_h > 0 ? o.min_h : 0;
    u->app = (struct app){ .name = p->name, .mouse = on_mouse, .key = on_key, .redraw = on_redraw,
                           .resize = on_resize, .close = on_close,
                           .icon = o.icon > ICON_NONE && o.icon < ICON_APPS ? o.icon : ICON_ABOUT };
    if (!wm_call(do_create, u) || !u->w) { u->used = false; return BAD; }
    return u - uw;
}

static uint32_t colour(uint32_t c) {
    return (c & 0xff000000) == 0x01000000 ? ui(c & 0xff) : (c & 0xffffff);
}

static const struct uifont *font_of(int f) {
    static const struct uifont *const fonts[] = { &font_ui, &font_bold, &font_small, &font_title, &font_big, &font_display };
    return f >= 0 && f < 6 ? fonts[f] : NULL;     /* NULL: FONT_MONO, the terminal's font */
}

static void run(struct surface *s, const struct dcmd *c, const char *text) {
    uint32_t c1 = colour(c->c1);
    switch (c->op) {
    case DRAW_FILL:   gfx_fill(s, c->x, c->y, c->w, c->h, c1); break;
    case DRAW_ROUND:  gfx_round_a(s, c->x, c->y, c->w, c->h, c->r, c1, c->c2 ? (int)c->c2 : 256); break;
    case DRAW_RING:   gfx_ring(s, c->x, c->y, c->w, c->h, c->r, c1, c->c2 ? (int)c->c2 : 256); break;
    case DRAW_CIRCLE: if (c->r > 0 && c->r < 2000) gfx_circle(s, c->x, c->y, c->r, c1); break;
    case DRAW_LINE:   gfx_line(s, c->x, c->y, c->w, c->h, c->r < 1 ? 1 : c->r > 64 ? 64 : c->r, c1); break;
    case DRAW_TEXT: {
        const struct uifont *f = font_of(c->font);
        if (!f) gfx_text(s, c->x, c->y, text, c1, -1);
        else if (c->w > 0) gfx_utext_fit(s, f, c->x, c->y, text, c1, c->w);
        else gfx_utext(s, f, c->x, c->y, text, c1);
        break;
    }
    case DRAW_GLYPH:  if (c->r > 0 && c->r <= 256) icon_glyph(s, c->arg, c->x, c->y, c->r, c1); break;
    case DRAW_ICON:   if (c->r > 0 && c->r <= 256) icon_app(s, c->arg, c->x, c->y, c->r); break;
    case DRAW_BUTTON: ui_button(s, (struct rect){ c->x, c->y, c->w, c->h }, text, c->arg & 1, c->arg & 2); break;
    case DRAW_FIELD:  ui_field(s, (struct rect){ c->x, c->y, c->w, c->h }, text, NULL, c->arg & 1, c->arg & 2); break;
    case DRAW_SCROLLBAR: ui_scrollbar(s, c->x, c->y, c->h, (int)c->c1, (int)c->c2, c->arg); break;
    }
}

uint64_t sys_draw(struct process *p, uint64_t h, uint64_t buf, uint64_t len) {
    struct uwin *u = mine(p, h);
    if (!u || len > 256 * 1024 || !vmm_user_ok(p->pml4, buf, len, false)) return BAD;
    mutex_lock(&wm_surface_lock);
    if (!u->w) { mutex_unlock(&wm_surface_lock); return BAD; }
    struct surface *s = u->w->surf;
    uint64_t off = 0;
    while (off + sizeof(struct dcmd) <= len) {
        struct dcmd c;
        memcpy(&c, (const void *)(buf + off), sizeof c);      /* copy it, so it can't change under us */
        if (c.size < sizeof c || off + c.size > len) break;
        char text[256];
        size_t tl = c.size - sizeof c;
        if (tl > sizeof text - 1) tl = sizeof text - 1;
        memcpy(text, (const void *)(buf + off + sizeof c), tl);
        text[tl] = 0;
        run(s, &c, text);
        off += c.size;
    }
    mutex_unlock(&wm_surface_lock);
    return 0;
}

uint64_t sys_event(struct process *p, uint64_t h, uint64_t out, uint64_t wait_ms) {
    struct uwin *u = h < MAX_UWIN && uw[h].used && uw[h].owner == p ? &uw[h] : NULL;
    if (!u || !vmm_user_ok(p->pml4, out, sizeof(struct event), true)) return BAD;
    uint64_t until = pit_ticks() + (wait_ms * pit_hz() + 999) / 1000;
    for (;;) {
        if (u->qt != u->qh) {
            uint64_t f = irq_save();
            struct event e = u->q[u->qt];
            u->qt = (u->qt + 1) % QLEN;
            irq_restore(f);
            memcpy((void *)out, &e, sizeof e);
            return 1;
        }
        if (!u->w || u->dead) {                    /* the window is gone: say so */
            struct event e = { .type = EV_CLOSE };
            memcpy((void *)out, &e, sizeof e);
            return 1;
        }
        if (p->killed || pit_ticks() >= until) return 0;
        task_sleep(10);
    }
}

uint64_t sys_winctl(struct process *p, uint64_t h, uint64_t op, uint64_t arg) {
    struct uwin *u = mine(p, h);
    if (!u) return BAD;
    char text[160];
    switch (op) {
    case WINCTL_TITLE:
        if (!user_string(p, arg, text, sizeof u->title)) return BAD;
        strlcpy(u->title, text, sizeof u->title);
        return wm_call(do_title, u) ? 0 : BAD;
    case WINCTL_CLOSE:
        u->dead = true;                            /* the desktop takes it away next frame */
        return 0;
    case WINCTL_SIZE:
        return u->w ? (uint64_t)u->w->w | (uint64_t)u->w->h << 32 : BAD;
    case WINCTL_NOTIFY: {
        if (!user_string(p, arg, text, sizeof text)) return BAD;
        char *nl = strchr(text, '\n');
        if (nl) *nl = 0;
        wm_notify(text, nl ? nl + 1 : "");
        return 0;
    }
    case WINCTL_FRONT:
        return wm_call(do_front, u) ? 0 : BAD;
    }
    return BAD;
}

uint64_t sys_measure(struct process *p, uint64_t font, uint64_t str) {
    char text[256];
    if (!user_string(p, str, text, sizeof text)) return BAD;
    const struct uifont *f = font_of((int)font);
    return f ? (uint64_t)gfx_utext_width(f, text) : strlen(text) * FONT_W;
}
