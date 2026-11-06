# Changelog

## unreleased

- **Shell scripts** ([docs/SCRIPTING.md](docs/SCRIPTING.md)): variables (`$x`, `${x}`, `$1`,
  `$#`, `$@`, `$?`, `$RANDOM`...), `$((arithmetic))`, `$(command)` substitution,
  `if`/`elif`/`else`, `while`, `until`, `for`, `break`, `continue`, comments, and
  `test` / `[ ]`, `true`, `false`, `read`, `set`, `unset`, `shift`, `sh` / `source`,
  `exit` inside scripts. Files starting with `#!` run like programs; `~/.dreamrc` runs at
  startup. All of it works on one line at the prompt too.
- Commands have a real exit status: `$?`, `&&` and the new `||` use it, and programs'
  exit codes count.
- Scripts in `scripts/` are installed into `/bin`: `todo`, `countdown`, `greet`.
- `echo -n`, `wc -l/-w/-c`.
- Pipelines own their capture buffers, so scripts can use pipes while they're themselves
  in one, and the tokenizer no longer shares one buffer between nested lines.

- **Floating point for programs**: SSE is switched on and every task's FPU/SSE registers
  are saved and restored on each task switch (`fxsave`/`fxrstor`). User programs are now
  compiled with floating point, and their `printf` understands `%f` / `%.2f`.
- New program `mandel` (the Mandelbrot set in colour, with doubles) and a `fputest`
  command that proves tasks keep their own FPU registers.
- **Programs can open windows.** New system calls for files (`writefile`, `listdir`,
  `fileinfo`), windows (`window`, `draw`, `event`, `winctl`, `measure`) and the clipboard,
  shared through `include/dream/abi.h`. Programs send batches of drawing commands and the
  kernel draws them with the system fonts and widgets (`user/lib/gui.h`).
- **Every app moved out of the kernel** except the terminal: files, calculator, paint,
  monitor, settings, clock and snake are now programs in `/bin` (`user/*.c`), like notes.
  New system calls for them: `stat`, `fsop` (mkdir/create/remove/rename), `sysinfo`,
  `tasks`, `kill`, `config`, `theme`, `passwd`, `spawn`, `terminal`; new drawing commands
  for gradients and wallpaper previews; and `app_run`, `ui()` and `mix()` in the GUI library.
- Paint remembers its strokes: **ctrl+z** undo, resizable, and the picture follows theme changes.
- **Notes moved out of the kernel**: it's now `user/notes.c`, an ordinary ring-3 program.
  Several can be open at once, and a crash in one can't take dream down.
- **Background programs**: `prog &` in the shell; the dock, the launcher and the file
  manager start programs in the background too. Their output goes to the serial log.
- **Clipboard**: select, copy, cut and paste in notes; `ctrl+v` at the shell prompt;
  `copy` and `paste` commands.
- `snprintf` and `%f` in the user C library.
- Fix: the desktop could copy a program's window to the screen halfway through a batch of
  drawing and then miss the rest, leaving a blank page (about 1 time in 6). The compositor
  now holds the surface lock while it collects damage and composes.
- The welcome `readme.txt` no longer claims files vanish on reboot.
- The mouse uses QEMU's absolute "vmmouse" when available, so it works without capture.
- `dreamos` command (`make install`).

## 0.2: the desktop grows up (2026-11-06)

**Look and feel**
- Smooth, anti-aliased text everywhere on the desktop, set in Inter (rendered at build
  time by `tools/mkfont.py`, since the kernel has no font engine or floating point)
- Frosted-glass top bar and dock (real blur, done in integer maths)
- New window style: unified title bars, hairline edges, larger soft shadows, app icons,
  minimize / maximize / close buttons, fade in and out
- App icons drawn from shapes, so they're sharp at any size and follow the theme
- Five wallpapers painted from maths in the theme's colours: aurora, dunes, glow, nebula, plain
- Resize pointer when hovering a window edge

**Desktop features**
- Dock with running indicators; click to open, raise or minimize
- Launcher (super key or the moon) with search
- Resizable windows, maximize (button, double-click, f11), snap to half by dragging to an edge
- Alt+tab switcher, alt+f4, ctrl+alt+t
- Notifications (`wm_notify`, and `notify` in the shell)
- Right-click desktop menu
- Calendar pop-up from the clock; live CPU graph in the top bar
- Power menu: lock, leave desktop, restart, shut down
- Lock screen with an optional password; dream boots straight into it when one is set

**Apps**
- New: notes (text editor), calculator, monitor (CPU/memory graphs, tasks, end task),
  settings (appearance, desktop, system, about), snake
- Files: places sidebar, icon toolbar, breadcrumbs, resizable; text files open in notes
- Terminal: resizable, keeps its text when resized
- Clock and paint restyled; "about" is now a page in settings

**System**
- Settings saved in `/etc/dream.conf` and restored at boot (theme, wallpaper, autostart, ...)
- The desktop starts at boot (can be turned off in settings)
- Keyboard driver: alt, super and function keys
- CPU usage measured from the idle task's share of timer ticks
- `task_kill` for user programs
- New shell commands: `open`, `notify`, `lock`, `passwd`, `kill`, `wallpaper`;
  `theme` now remembers your choice
- `tools/qemu_drive.py` for scripted, headless testing with screenshots
- Documentation in `docs/`

## 0.1 (2026-11-05)

First version: Limine boot, GDT/IDT/PIC/PIT, framebuffer console with themes, shell with
pipes and redirects, page allocator and heap, ATA driver and dreamfs, text editor, PS/2
mouse and paint, double-buffered desktop with windows, preemptive multitasking, user mode
with system calls and an ELF loader, and a file browser.
