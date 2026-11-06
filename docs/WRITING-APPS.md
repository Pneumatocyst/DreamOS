# Adding things to dream

Three ways to extend dream, from smallest to biggest.

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

- no floating point (the kernel doesn't save FPU registers between tasks yet)
- a built-in command with the same name wins over a program in `/bin`
- if it crashes, dream prints "segmentation fault" and carries on; try `crashme`
- **ctrl+c** stops it, and so does **end task** in the monitor

## 3. A desktop app

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
