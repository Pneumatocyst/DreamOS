# dream — project handoff

> Context for an AI assistant (or a person) picking up this project. First written 2026-11-05
> at the end of the first build session; updated 2026-11-06 after the 0.2 session (see
> section 9). The code lives on GitHub at `Pneumatocyst/dream`.

## 1. What this is

**dream** is a hobby x86_64 operating system that **Lynwood** is building **to learn how
operating systems work**, with an emphasis on it looking good ("simple, sleek, for learning,
with tools and aesthetic"). It started life named *lumen* and was renamed *dream* the same day.

Everything was built in one long session with Claude Code (CLI) on Lynwood's machine. Lynwood
wrote one piece of kernel code personally (the `hello` shell command); Claude wrote the rest and
explained it. All of it boots and was verified in QEMU, usually with screenshots.

- Language: C (gnu11, freestanding) + a little GNU assembler (AT&T syntax). No Rust.
- Bootloader: **Limine** (protocol base revision 3), BIOS and UEFI ISO.
- Emulator: **QEMU**, machine type `-M pc` (i440fx, so the legacy IDE controller exists).
- Size: about 10,000 lines of C/assembly (plus a 4,700-line generated font table).
  Git repo on GitHub (`Pneumatocyst/dream`, branch `main`).

## 2. How Lynwood works (important)

- **Machine:** Arch Linux with **Omarchy** (Hyprland). Shell: bash. Editor: Neovim.
- **Experience:** new to OS development. When a dense, multi-step tutorial was given, they said
  *"shit I'm confused. What am I doing?"* After that, teaching **one small step at a time, in
  plain language, waiting for them** worked well (they successfully wrote the `hello` command).
- **Then they asked to "accelerate"**: since then, features are built fully by the assistant,
  tested by booting, explained afterwards, and committed.
- So: **when teaching, go slowly and concretely; when they ask for a feature, build it completely,
  verify it, then explain it briefly.** Keep their `cmd_hello` in `kernel/shell/shell.c` (it has
  harmless unused-parameter warnings; it's theirs, don't "fix" it unasked).
- They like themed/sci-fi names and aesthetics (their other projects include an Aliens/LV-426
  desktop theme and a space theme called The Cosmos). dream has a `nostromo` green-phosphor theme.
- `sudo` doesn't work from Claude Code's `!` prefix (no TTY): ask them to run sudo commands in a
  real terminal.

## 3. Building and running

```sh
sudo pacman -S --needed gcc make limine qemu-desktop libisoburn edk2-ovmf   # one time
cd ~/Projects/dream
make            # builds kernel + user programs + dream.iso
make run        # boots in QEMU (BIOS); serial log prints in the terminal
make run-uefi   # same with OVMF
make debug      # QEMU paused + GDB attached, breakpoint on kmain
make wipe-disk  # fresh blank disk.img (dream formats it on boot)
make clean      # removes build/ and the ISO (keeps disk.img)
```

- `make install` links `tools/dreamos` to `~/.local/bin/dreamos`: typing `dreamos` boots dream
  (`dreamos uefi|debug|build|wipe|help`). Older: a launcher script `~/.local/bin/dream` (on PATH) runs `make run` from anywhere; `dream build`,
  `dream uefi`, `dream debug` too. There's also an app-launcher entry
  (`~/.local/share/applications/dream.desktop`).
- `disk.img` (32 MiB raw, git-ignored) holds the saved filesystem across boots.
- In the QEMU window: click to capture the mouse, **Ctrl+Alt+G** releases it.
- `limine.h` is vendored in `include/` (from the limine-protocol repo). Limine's data files come
  from the system package (`limine --print-datadir` → `/usr/share/limine`).
- `compile_flags.txt` makes clangd work in Neovim.

### Headless testing (how every feature was verified)

`tools/qemu_drive.py script.txt outdir/` boots the ISO headless with a scratch disk and plays
a script (`wait`, `type`, `key`, `mouse`, `click`, `down`/`up`, `shot`); see docs/BUILDING.md.
Gotchas: keys go to the **focused window** (`key ctrl-alt-t` refocuses the terminal); QEMU drops
or scales big mouse moves, so the script moves in 40 px steps, and you should park the mouse
with `mouse -2000 -2000` before moves you need to be exact; overshoot when dragging to an edge.
The scratch disk persists settings between runs in the same outdir: delete `scratch.img` for
a clean boot.

## 4. Architecture (what exists, in boot order)

(See docs/ARCHITECTURE.md for the long version. The table below was written for 0.1;
section 9 lists what 0.2 added.)

```
kmain (kernel/main.c)
 ├─ serial (COM1 log)          ├─ GDT + TSS        ├─ IDT (exceptions, IRQs, int 0x80)
 ├─ PIC remap (32-47)          ├─ PIT 100 Hz       ├─ PS/2 keyboard + mouse (wheel)
 ├─ pmm (bitmap page alloc) → back buffer → vmm_init → sched_init("shell")
 ├─ console (text grid on the back buffer, themed)
 ├─ ramfs (in-memory tree) → ATA probe → dreamfs load/format from sda
 ├─ install /bin programs (baked into the kernel image)
 └─ shell_run()  ← the boot thread becomes the "shell" task
```

### Kernel subsystems (paths under `kernel/`)

| Area | Files | Notes |
|---|---|---|
| CPU | `arch/gdt.c`, `idt.c`, `isr.S`, `pic.c`, `pit.c`, `io.h`, `user.S` | GDT: null, kcode 0x08, kdata 0x10, udata 0x18, ucode 0x20, TSS 0x28. `irq_save/irq_restore` = interrupts-off lock. `enter_user` builds an iretq frame into ring 3. |
| Memory | `mm/pmm.c`, `heap.c`, `vmm.c` | pmm: bitmap of 4 KiB pages from Limine's memmap, next-fit, HHDM for access. heap: `kmalloc/kfree/krealloc/kzalloc`, size classes 32..2048 + whole pages, magic headers (panics on double free). vmm: per-process PML4, kernel half (256-511) copied, user pages `P|W|U`, `vmm_user_ok` validates user pointers. SMAP turned off. |
| Scheduling | `sched/task.c`, `switch.S`, `task.h` | Kernel tasks, 64 KiB stacks, round-robin, preemptive on every PIT tick (10 ms), `task_sleep/yield/exit`, idle task, reaping. Switches TSS.rsp0 and CR3 per task. Recursive `struct mutex` (yield while waiting). |
| Processes | `proc/process.c`, `process.h` | ELF64 loader (PT_LOAD), user stack at 0x7ffffffff000 (64 KiB), argc/argv. Syscalls via `int 0x80` (rax=num, rdi/rsi/rdx args): 0 exit, 1 write, 2 read, 3 sleep, 4 getpid, 5 uptime_ms, 6 readfile. Ring-3 faults → "segmentation fault" message, program killed, kernel fine. Ctrl+C sets `killed`, checked on return-to-user from IRQs and in syscalls. |
| Graphics | `gfx/gfx.c`, `drivers/fb.c` | Surfaces (0xRRGGBB) with clip + damage rects; fill, blend, gradient, AA rounded rects/circles/thick lines (integer fixed-point, no floats in kernel), PSF2 font text (Terminus 8x16, `assets/font.psf`), scaled text, blit, pointer sprite. fb: double buffering, `fb_present` copies only damage. |
| Console | `drivers/console.c`, `theme.c` | Cell grid (char + palette slot) drawn onto any surface; text mode has a status bar. Colour escapes `ESC+digit` (`C_FG`, `C_ACCENT`...). Themes: midnight, nostromo, cosmos, ember. Alternate screen (`con_save/restore`) for the editor. |
| Drivers | `drivers/keyboard.c`, `mouse.c`, `ata.c`, `rtc.c`, `serial.c` | Scancode set 1 incl. arrows/Home/End/PgUp/PgDn/Del, Ctrl → ^A..^Z. Mouse IRQ12, IntelliMouse wheel knock. ATA PIO LBA28 polling, IDENTIFY (sdX disks, srX ATAPI). CMOS RTC (QEMU `-rtc base=localtime`). |
| Filesystem | `fs/ramfs.c`, `diskfs.c` | ramfs: up to 1024 nodes, files up to 1 MiB in kmalloc'd buffers, paths with `~`, `.`, `..`. Every public fn takes the fs mutex; `fs_generation()` counter bumps on changes. dreamfs: snapshot of the whole tree to sda — sector 0 superblock ("DREAMFS1", node count, FNV-1a checksum), node table from sector 1, file data after; autosaved after every shell command when dirty. Old 256-node disks still load. |
| Shell | `shell/shell.c`, `cmd_fs.c`, `cmd_sys.c`, `cmd_disk.c`, `programs.c` | Tokenizer with quotes, `|`, `>`, `>>`, `;`, `&&` (failure = a `kerrorf` happened). stdout capture via `kout_redirect`; `kerrorf` always goes to screen. History. Built-ins first, then `/bin/<name>` programs. |
| Apps | `apps/edit.c`, `apps/paint.c` | nano-style editor (^S ^Q ^K ^U ^F ^G, auto-indent, line numbers). Paint (window app). |
| Desktop | `gui/wm.c`, `term.c`, `files.c`, `clock.c`, `about.c` | Compositor (wallpaper with glows + grain, AA rounded windows, soft shadows, title bars, close buttons, top panel with app buttons + clock + exit, software pointer), focus/raise/drag, damage-driven redraw. Runs as its own **desktop task**; the shell posts requests through a mailbox (`wm_post`: paint, files, theme, stop). Terminal window = the shell's console retargeted; closing it hides it. |

### Commands (≈45 built-ins)

- files: `ls [-la] cd pwd tree mkdir [-p] touch cat cp mv rm [-rf] rmdir`
- text: `echo grep [-vc] head/tail [-n] wc hexdump`
- editor/apps: `edit` (= `nano`), `paint`, `files`, `startx`, `exit`
- disks: `lsblk blkread blkwrite sync mkfs`
- system: `uname [-a] hostname whoami date uptime free mem memtest lscpu dmesg ps history sleep`
- dream: `help fetch theme clear hello crash reboot shutdown`
- user programs in `/bin` (ring 3, from `user/`): `hi counter spin crashme upper guess`

### User space (`user/`)

- `user/lib/`: `crt0.S` (_start → main → exit), `dream.c/.h` tiny libc: syscall wrappers,
  `printf` (%d %u %x %s %c %p), `puts`, `getchar`, `readline` (with echo), `atoi`, `rand`, string fns,
  colour macros.
- `user/user.ld`: programs linked at 0x400000. Built with `-mno-sse` (the kernel doesn't save
  FPU/SSE state on task switches).
- **Adding a program = drop a `.c` file in `user/`.** The Makefile compiles it, generates
  `build/userbins.S` (incbin table), and the kernel copies it into `/bin` at boot. Built-ins shadow
  `/bin` names (that's why the example program is `hi`, not `hello`).

## 5. History (git log)

| Commit | What |
|---|---|
| `107a46f` | dream 0.1: kernel, framebuffer console + themes, shell, ramfs, ATA driver, dreamfs, editor |
| `2b2a3f3` | page allocator + kmalloc; files up to 1 MiB |
| `0757b21` | PS/2 mouse, pointer, paint |
| `aafbbed` | gfx library, double buffering, desktop (startx), terminal/paint/clock/about windows |
| `2e1d652` | preemptive multitasking; desktop as its own task |
| `b5de850` | user mode: ring 3, page tables, TSS, syscalls, ELF loader, /bin programs |
| `f4217c8` | file browser; mutexes for fs, disk saves and ATA |

Pre-git steps (same day): initial scaffold as *lumen*, Linux-style commands + pipes, rename to
*dream*, disk driver + persistence, text editor, `~/.local/bin/dream` launcher.

## 6. Known limitations / rough edges

- One terminal; while a program runs, the terminal belongs to it (can't run `ps` meanwhile).
- No FPU/SSE state saving → kernel and user code are compiled without SSE/floats.
- ATA is PIO + polling, LBA28 (≤128 GiB); CD drives detected but unreadable.
- dreamfs rewrites the whole tree on every save (fine at this size); no journaling.
- The terminal window is fixed-size; windows can't be resized or minimized.
- Single CPU only (no SMP); locks assume that.
- Keys go to the focused window; long-running *kernel* commands (not user programs) still
  hold the shell task but no longer freeze the desktop.
- `fs_get()`/`fs_data()` return pointers without holding the lock (fine for reads/glitches).

## 7. Ideas for next steps (offered to Lynwood, not yet chosen)

- Desktop: icon view / drag-and-drop in files, a text viewer window, resizable windows, more apps.
- Faster syscalls with `syscall`/`sysret`; more syscalls (open/close/write files, spawn).
- Multiple terminals / running programs in the background (`&`).
- Saving FPU state so programs can use floating point.
- A real on-disk filesystem (e.g., read FAT or ext2), CD (ATAPI) reading.
- Sound (PC speaker / Sound Blaster / AC97), networking (e1000/virtio-net), a game.
- Teaching exercises: write a new `/bin` program in `user/` (copy `user/hi.c`).

## 8. Conventions to keep

- Comments explain *why* and teach (the code is meant to be read and learned from).
- Every feature gets: build with no new warnings → boot test (headless QEMU + screenshots) →
  README update → one git commit with a descriptive message
  (ending `Co-Authored-By: Claude …`).
- Kernel is freestanding: no libc; use `kprintf`, `snprintf`, `kmalloc`, the `str*` in `lib/string.c`.
- Never touch the user's `disk.img` in tests; use a scratch image.
- UI text is lower-case and friendly ("dream is fine."); colours come from the theme palette.

## 9. The 0.2 session (2026-11-06, Claude Code on the web)

Lynwood asked to "add the stuff normal operating systems have", make it "more modern and
sleek", and put it on GitHub with documentation. Built in a cloud container (Ubuntu), with
Limine's v9.x-binary release built from source because Ubuntu doesn't package it.

Added:
- `kernel/gfx/ui.c`: anti-aliased Inter text (`gfx_utext`, fonts `font_small/ui/bold/title/big/display`,
  generated into `kernel/gfx/uifont_data.c` by `tools/mkfont.py`), `gfx_round_ex/_a`, `gfx_ring`,
  `gfx_shadow`, `gfx_blur`
- `kernel/gui/wm.c` rewritten: glass top bar (launcher, focused title, clock + calendar, CPU graph,
  power menu), dock, min/max/close, edge resize, snap, fades, launcher, right-click menu,
  notifications, alt+tab, lock screen. `struct app` gained `resize` and `icon`
  (use designated initializers). `ui(UI_*)` gives theme-derived colours.
- `gui/icons.c` (vector icons), `gui/wallpaper.c`, `gui/widgets.c`, `gui/apps.c` (the app list)
- apps: `apps/notes.c`, `calc.c`, `monitor.c`, `settings.c`, `snake.c`; files restyled; about.c removed
  (about is a settings page)
- `sys/config.c`: `/etc/dream.conf` key=value settings, lock password (salted FNV-1a, /etc/passwd.dream);
  `sys/stats.c`: CPU usage
- keyboard: alt, super, F-keys (`KEY_SUPER`, `KEY_ALTTAB`, `KEY_ALTF4`, `KEY_F1+n`); `kbd_alt()`
- `con_reattach` (terminal resize), `task_kill`, `power_reboot/power_off`
- shell: `open notify lock passwd kill wallpaper`; `theme` saves; desktop autostarts at boot
- docs/: BUILDING, DESKTOP, ARCHITECTURE, WRITING-APPS, screenshots; CHANGELOG.md

Known rough edges after 0.2:
- `theme_set` from the settings app redraws the console from the desktop task while the shell
  task may also draw into it (tiny race, never seen to matter).
- Paint's canvas isn't resizable (it would lose the drawing).
- One notes window at a time (the app keeps its state in statics, like the other apps).
- The original `edit` (text-mode editor) still exists; on the desktop, files opens notes instead.
- The wallpaper is rebuilt on every theme change (~100 ms at 1280x800).
