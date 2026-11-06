# Adding things to dream

Four ways to extend dream, from smallest to biggest.

## 1. A shell command (kernel side)

Commands are plain C functions in the kernel. Look at `cmd_hello` in
`kernel/shell/shell.c`:

```c
static void cmd_hello(int argc, char **argv) {
    kprintf("hello from my own command!\n");
}
```

Add a row to the `commands` table in the same file and it shows up in `help`:

```c
    { "hello",    "say hi",                               cmd_hello    },
```

Useful tools inside a command:

- `kprintf` prints (it goes into pipes and `>` redirects too). Colour with `C_ACCENT`, `C_DIM`,
  `C_OK`, `C_WARN`, `C_ERR`, `C_FG`: `kprintf(C_OK "done\n");`
- `kerrorf` prints an error. It always goes to the screen, and makes `&&` stop
- `sh_input(argc, argv, 1, &len)` gets "the file named in argv[1], or whatever was piped in"
- `fs_lookup`, `fs_create`, `fs_write`, `fs_data` work with files (`kernel/fs/ramfs.h`)
- `sh_wait_key()` waits for a key; `task_sleep(ms)` waits without hogging the CPU

## 2. A user program (ring 3)

Drop a `.c` file in `user/`. The Makefile finds it, compiles it, and puts it in `/bin`.

```c
/* user/countdown.c */
#include "dream.h"

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 3;
    for (int i = n; i > 0; i--) {
        printf(ACCENT "%d" FG "...\n", i);
        sleep_ms(1000);
    }
    puts("lift off!\n");
    return 0;
}
```

Type `make run`, then `countdown 5` in the terminal.

The program runs in its own address space and can only reach the kernel through
system calls. `user/lib/dream.h` lists what's available: `printf`, `puts` (no newline
added), `putchar`, `getchar`, `readline`, `read`, `write`, `sleep_ms`, `uptime_ms`, `getpid`,
`readfile`, `rand`, `atoi` and some string functions.

Things to know:

- floating point works (`double`, and `printf("%.2f")`); see `user/mandel.c`
- a built-in command with the same name wins over a program in `/bin`
- add `&` to run it in the background: `countdown 9 &`. Its output goes to the serial log
- if it crashes, dream prints "segmentation fault" and carries on; try `crashme`
- **ctrl+c** stops it, and so does **end task** in the monitor

## 3. A program with a window (ring 3)

This is the way to add an app: a normal program in `user/` that opens a window. It
stays out of the kernel, can't crash the system, and shows up in `/bin` like any other
program. (`user/notes.c` is a complete one.)

```c
/* user/clicker.c */
#include "gui.h"

int main(void) {
    int w = 300, h = 180, clicks = 0;
    int win = win_open("clicker", w, h, ICON_ABOUT, 0, 0);   /* 0, 0: fixed size */
    if (win < 0) { puts("clicker needs the desktop: type startx first.\n"); return 1; }
    bool hot = false;
    uint8_t held = 0;
    for (;;) {
        char n[16];
        snprintf(n, sizeof n, "%d", clicks);
        draw_fill(0, 0, w, h, COLOR_UI(UI_BG));
        draw_text(FONT_BIG, (w - text_width(FONT_BIG, n)) / 2, 30, n, COLOR_UI(UI_TEXT));
        draw_button(w / 2 - 60, 110, 120, 36, "click me", hot, true);

        struct event e;
        win_event(win, &e, 1000);              /* shows what we drew, then waits up to 1 s */
        if (e.type == EV_CLOSE) return 0;
        if (e.type == EV_MOUSE) {
            hot = e.x >= w / 2 - 60 && e.x < w / 2 + 60 && e.y >= 110 && e.y < 146;
            if (hot && (e.buttons & 1) && !(held & 1)) clicks++;   /* pressed just now */
            held = e.buttons;
        }
    }
}
```

`make run`, then in the terminal: `clicker &` (the `&` keeps the terminal free).

How it works: the `draw_*` functions don't draw anything themselves. They add commands
to a buffer ("a rounded box here, text there"), and `win_flush` (or `win_event`, which
flushes first) sends the whole batch to the kernel in one system call. The kernel draws
it with dream's own fonts, colours and widgets, so programs stay small and match the
built-in apps. It's the same idea as a display server like X11 or Wayland.

| function | does |
|---|---|
| `win_open(title, w, h, icon, min_w, min_h)` | open a window; give `min_w`/`min_h` to make it resizable |
| `win_event(win, &e, wait_ms)` | flush the drawing, then wait for an event. 1 = got one, 0 = timed out |
| `win_flush(win)` | send the drawing now |
| `win_title`, `win_close`, `win_size`, `win_front` | change the window |
| `notify(win, title, body)` | a notification card |
| `text_width(font, text)` | how wide text will be, for centring and right-aligning |
| `draw_fill`, `draw_round`, `draw_round_a`, `draw_ring`, `draw_circle`, `draw_line` | shapes |
| `draw_text`, `draw_text_fit` | text in `FONT_UI`, `FONT_BOLD`, `FONT_SMALL`, `FONT_TITLE`, `FONT_BIG`, `FONT_DISPLAY` or `FONT_MONO` (8 px per letter) |
| `draw_glyph`, `draw_icon`, `draw_button`, `draw_field`, `draw_scrollbar` | the same widgets the built-in apps use |

Events (`struct event`, see `include/dream/abi.h`):

| `e.type` | means |
|---|---|
| `EV_KEY` | `e.key`: ASCII, ctrl+letter is 1-26, `KEY_UP` etc. for the rest |
| `EV_MOUSE` | `e.x`, `e.y` inside the window, `e.buttons` (1 left, 2 right), `e.wheel` |
| `EV_RESIZE` | `e.w`, `e.h`: the new size. Redraw everything |
| `EV_REDRAW` | the theme changed. Redraw everything |
| `EV_CLOSE` | the close button (or the desktop is going away). Save if you need to, then return |

Colours: `0xRRGGBB`, or `COLOR_UI(UI_TEXT)`, `COLOR_UI(UI_ACCENT)` and so on to follow the theme.

Files: `readfile`, `writefile` (creates or replaces), `listdir` (one name per line, folders
end in `/`) and `fileinfo` (size, `FILE_IS_DIR`, or -1). The clipboard: `clip_get`, `clip_set`.

To put it in the dock and the launcher, add a launcher function next to `notes_open` in
`kernel/gui/apps.c` that calls `program_start("clicker", ...)`, and a row in `app_list`.

## 4. A built-in desktop app (kernel side)

The older way, still used by files, settings, the calculator and the rest: the app is
part of the kernel. It's faster to write (no system calls) but a bug in it is a bug in
the kernel. New apps should be programs (section 3).

### Writing one

A desktop app is a `struct app` with some callbacks, plus a function that opens its window.
Here is a complete one that counts clicks:

```c
/* kernel/apps/counter.c */
#include "gui/wm.h"
#include "drivers/mouse.h"

static int clicks;
static uint8_t held;

static void draw(struct window *w) {
    struct surface *s = w->surf;
    gfx_fill(s, 0, 0, s->w, s->h, ui(UI_BG));
    char n[16];
    snprintf(n, sizeof n, "%d", clicks);
    gfx_utext_center(s, &font_big, s->w / 2, 40, n, ui(UI_TEXT));
    ui_button(s, (struct rect){ s->w / 2 - 60, 110, 120, 36 }, "click me", false, true);
}

static void mouse(struct window *w, int x, int y, uint8_t buttons, int wheel) {
    (void)wheel;
    bool click = (buttons & MOUSE_LEFT) && !(held & MOUSE_LEFT);   /* pressed just now */
    held = buttons;
    if (click && rect_contains((struct rect){ w->w / 2 - 60, 110, 120, 36 }, x, y)) {
        clicks++;
        draw(w);
    }
}

static void redraw(struct window *w) { draw(w); }

const struct app counter_app = {
    .name = "counter", .mouse = mouse, .redraw = redraw, .icon = ICON_ABOUT,
};

void counter_open(void) {
    struct window *w = wm_find(&counter_app);
    if (w) { wm_show(w); return; }               /* already open: bring it forward */
    w = wm_open(&counter_app, "counter", 300, 180, NULL);
    if (w) draw(w);
}
```

Then:

1. declare `void counter_open(void);` in `kernel/gui/wm.h`
2. add it to `app_list` in `kernel/gui/apps.c`. The last field says whether it's pinned
   to the dock:

   ```c
   { "counter", "counts your clicks", ICON_ABOUT, counter_open, false },
   ```

That's it: it's in the launcher, `open counter` works, and the dock shows it while it's open.

### The callbacks

| callback | called when |
|---|---|
| `mouse(w, x, y, buttons, wheel)` | the pointer moves or a button changes over the window (x/y are inside its contents). While a button is held, the window keeps getting events even outside its edges |
| `key(w, key)` | a key is pressed while the window is focused. Letters are ASCII; ctrl+letter is 1-26; arrows etc. are `KEY_*` in `drivers/keyboard.h` |
| `tick(w)` | about 10 times a second, while the window is visible |
| `redraw(w)` | the theme changed: repaint everything with the new colours |
| `resize(w)` | the window got a new size (`w->w`, `w->h`) and a fresh surface |
| `close(w)` | the close button. Return `true` to destroy the window, `false` to just hide it |

Apps draw whenever they like: anything drawn into `w->surf` marks that area as
damaged, and the desktop puts it on screen next frame. All callbacks run on the
desktop task, one at a time, so apps don't need locks.

To make a window resizable, call `wm_resizable(w, min_w, min_h)` after opening it, and
handle `resize`.

### Looking like the rest of dream

- colours: `ui(UI_BG)`, `UI_SIDEBAR`, `UI_RAISED`, `UI_HOVER`, `UI_SEL`, `UI_LINE`, `UI_TEXT`,
  `UI_DIM`, `UI_ACCENT`, `UI_OK`, `UI_WARN`, `UI_ERR`. They follow the theme
- text: `gfx_utext(s, &font_ui, x, y, "text", colour)`. Fonts: `font_small` (12 px),
  `font_ui` (14), `font_bold` (14 semibold), `font_title` (20), `font_big` (34),
  `font_display` (96, digits only). `gfx_utext_fit` cuts long text off with "..."
- widgets (`gui/widgets.c`): `ui_button`, `ui_icon_button`, `ui_field`, `ui_toggle`,
  `ui_scrollbar`
- shapes: `gfx_round` (rounded rectangle), `gfx_round_a` (see-through), `gfx_ring` (outline),
  `gfx_circle`, `gfx_line` (smooth, any thickness)
- icons: `icon_app(s, ICON_*, x, y, size)` for app tiles, `icon_glyph(s, GLYPH_*, x, y, size, colour)`
  for small symbols. New ones go in `gui/icons.c`, drawn from shapes on a 48 x 48 grid
- tell the user something: `wm_notify("title", "message")`
- remember something across reboots: `config_set("key", "value")`, `config_get("key", "default")`

The existing apps are good examples: `apps/calc.c` is small and self-contained,
`apps/notes.c` shows keyboard handling and files, `apps/monitor.c` shows live data and
graphs, and `apps/settings.c` shows a sidebar with pages.
