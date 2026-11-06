# Changelog

## unreleased

- **Floating point for programs**: SSE is switched on and every task's FPU/SSE registers
  are saved and restored on each task switch (`fxsave`/`fxrstor`). User programs are now
  compiled with floating point, and their `printf` understands `%f` / `%.2f`.
- New program `mandel` (the Mandelbrot set in colour, with doubles) and a `fputest`
  command that proves tasks keep their own FPU registers.
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
