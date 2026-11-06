# How dream works

This is a tour from the moment the PC switches on to the moment a pixel of a window
lands on the screen. Each section names the files to read next.

## 1. Booting

The firmware (BIOS or UEFI) loads **Limine** from the ISO. Limine reads `limine.conf`,
loads `dream.elf` into the top 2 GiB of the address space (a "higher-half" kernel; see
`linker.ld`), switches the CPU into 64-bit long mode with paging on, sets up a framebuffer,
and jumps to `kmain` in `kernel/main.c`. Before jumping it fills in the **requests** dream
left in a special section: here, the memory map.

`kmain` then sets things up in this order:

```
serial (COM1, for logs)    GDT + TSS     IDT (exceptions, IRQs, int 0x80)
PIC remap (IRQs at 32-47)  PIT at 100 Hz keyboard + mouse        interrupts on
pmm (page allocator) → back buffer → vmm (page tables) → scheduler ("shell" task)
console → ramfs → ATA disks → dreamfs (load or format) → /bin programs → shell_run()
```

`shell_run` loads `/etc/dream.conf` (the theme, and whether to start the desktop) and
then either starts the desktop or shows the prompt.

## 2. The CPU tables (`kernel/arch/`)

- **GDT** (`gdt.c`): the segments. In 64-bit mode they mostly just say "kernel" or "user",
  plus the **TSS**, which tells the CPU which stack to use when an interrupt arrives while
  a user program is running.
- **IDT** (`idt.c`, `isr.S`): 256 entries saying where to go on each interrupt. The stubs in
  `isr.S` save every register into a `struct regs` and call C.
- **PIC** (`pic.c`): the old interrupt controller, remapped so hardware IRQs don't collide
  with CPU exceptions. **PIT** (`pit.c`): the timer, 100 ticks a second. Each tick drives
  the scheduler.

## 3. Memory (`kernel/mm/`)

- **pmm.c**: one bit per 4 KiB physical page, built from Limine's memory map.
- **heap.c**: `kmalloc`/`kfree`. Small requests come from size classes (32 to 2048 bytes)
  carved out of pages; big ones get whole pages. Every block has a header with a magic
  number, so a double free panics with a clear message instead of corrupting memory.
- **vmm.c**: page tables. Each user program gets its own 4-level table (PML4). The kernel
  half (entries 256-511) is shared by everyone but marked supervisor-only.

Physical memory is reached through Limine's **HHDM** (a direct map of all RAM at a fixed
offset), so the kernel can touch any page without mapping it first.

## 4. Tasks and scheduling (`kernel/sched/`)

A task is a stack plus saved registers. `switch.S` is the whole context switch: push the
callee-saved registers, save `rsp`, load the other task's `rsp`, pop, `ret`.

On every timer tick, `sched_tick` charges the tick to the running task and picks the next
ready one, round-robin. Sleeping tasks wake when their time comes. The **idle** task runs
`hlt` when nobody else wants the CPU, which is also how CPU usage is measured: the share
of ticks that went to idle is the share of time the machine was idle (`sys/stats.c`).

Each task also has a 512-byte area for its **FPU/SSE registers**. The scheduler saves the
outgoing task's with `fxsave` and loads the incoming one's with `fxrstor`, so user programs
can use floating point without trampling each other. (The kernel itself is still compiled
without floating point, so interrupt handlers never touch those registers.) `fputest`
checks it: a background task parks a value in `xmm0` while `mandel` does float maths.

Tasks share data through `struct mutex` (wait by yielding) or, for tiny critical sections,
by turning interrupts off (`irq_save`/`irq_restore`). dream runs on one CPU, so that's enough.

Usually three tasks run: **shell**, **desktop** and **idle**. Each running program adds one more.

## 5. User programs (`kernel/proc/`, `user/`)

Programs in `user/*.c` are compiled separately against `user/lib` (a tiny libc), linked at
`0x400000`, and baked into the kernel image. At boot they're copied into `/bin`.

Running one (`process_spawn`): create a page table, load the ELF's segments, map a stack
at the top of the lower half with `argc`/`argv` on it, make a task, and drop into ring 3
with `iretq` (`arch/user.S`). From then on the program can only ask the kernel for things
with `int 0x80` (rax = call number, rdi/rsi/rdx = arguments). The numbers are in
`include/dream/abi.h`, which the kernel and programs share:

| calls | for |
|---|---|
| exit, write, read, sleep, getpid, uptime | the basics |
| readfile, writefile, listdir, fileinfo | files (whole files at a time: simple, and plenty for 1 MiB files) |
| stat, fsop (mkdir, create, remove, rename) | more files (what the file manager needs) |
| window, draw, event, winctl, measure | the window system (section 8) |
| sysinfo, tasks, kill | memory, uptime, CPU, clock, disk; the task list (the monitor) |
| config, theme, passwd | settings, themes and wallpapers, the lock screen password |
| spawn, terminal | start a program in the background; type a command into the terminal |
| clipboard | get or set the clipboard's text |

These live in `proc/process.c`, `gui/uwin.c` and `sys/syscalls.c`. Every app except the
terminal is a program built on them. Every pointer a program passes is checked against its own page tables first
(`vmm_user_ok`), and strings are copied in byte by checked byte (`user_string`).

The shell normally waits for a program to finish. With `&` at the end of the line, or
when the desktop starts one (the dock, the launcher, opening a file), it runs in the
**background** instead: `program_start` marks it detached, its output goes to the serial
log, and `process_reap` frees it after it ends.

If a program touches memory it shouldn't, the CPU raises a fault in ring 3. Instead of
panicking, dream prints "segmentation fault", ends that program and carries on. Ctrl+C
and `kill` set a `killed` flag that the kernel checks on the way back to user mode.

## 6. Files (`kernel/fs/`)

- **ramfs.c** holds the whole tree in memory: up to 1024 nodes, files up to 1 MiB each.
  Paths understand `/`, `.`, `..` and `~`. A generation counter goes up on every change,
  so the file browser knows when to refresh.
- **diskfs.c** ("dreamfs") saves a snapshot of the tree to the first IDE disk: a
  superblock with a checksum in sector 0, a node table, then file contents. The shell
  saves after every command that changed something, and apps save after they write.
- **drivers/ata.c** talks to the disk: PIO mode, LBA28, polling.

## 7. Drawing (`kernel/gfx/`, `kernel/drivers/fb.c`)

Everything draws into **surfaces**: blocks of `0xRRGGBB` pixels with a clip rectangle and a
**damage** rectangle that grows to cover whatever was drawn.

- `fb.c` keeps a **back buffer** in RAM. Drawing happens there; `fb_present` copies only the
  damaged area to the real framebuffer, so you never see half a frame.
- `gfx.c`: fills, gradients, anti-aliased circles and thick lines, rounded rectangles,
  the 8x16 Terminus bitmap font (used by the terminal), blits and the mouse pointer.
- `ui.c`: see-through rounded rectangles, 1-pixel rounded outlines, soft shadows, blur,
  and **smooth text**.

The kernel is compiled without floating point (user programs get it, the kernel
doesn't), so all of this is integer maths in fixed point: 256 means "one whole pixel" or
"fully covered". Edges are smooth because a pixel that's 40% inside a shape gets 40%
of its colour.

### Smooth text

The kernel can't run a font engine, so `tools/mkfont.py` does that on the build machine:
it renders every printable character of Inter at a few sizes (12, 14, 20, 34 and 96 px)
and stores how much of each pixel the glyph covers (0-255). `gfx_utext` blends the text
colour into the surface by that amount. Each glyph also records how far to advance, in
1/16ths of a pixel, so spacing stays even.

## 8. The desktop (`kernel/gui/`)

The desktop is its own task (`desktop_task` in `wm.c`). About 100 times a second it:

1. reads mouse and keyboard events and handles them: moving and resizing windows, the
   dock, menus, or passing the event on to an app;
2. runs each app's `tick` about ten times a second;
3. collects **damage**: from things it changed, and from windows whose surfaces the apps
   drew into;
4. **composes** each damaged rectangle, back to front, and copies it to the screen.

```
wallpaper → windows (shadow, body, title bar) → snap preview
          → top bar (glass) → dock (glass) → menus → alt+tab → notifications → pointer
```

The launcher and the lock screen replace all of that with a blurred snapshot of the
screen (or the wallpaper), with their own content on top.

**Frosted glass** is a copy, a blur and a tint: copy what's under the top bar or dock
into a scratch surface, box-blur it three times (which comes out close to a gaussian
blur), mix in the theme colour, and copy it back through a rounded mask. Because the
blur reads pixels from all around, any damage that touches the bar or the dock is
widened to cover the whole of it.

**Windows** are `struct window`: a position, a size, a surface the app draws into,
and an `app` with callbacks (`mouse`, `key`, `tick`, `redraw`, `resize`, `close`).
Resizing allocates a new surface and calls `resize`, so the app repaints at the new size.
Opening, closing and minimizing fade the window over four frames.

Other tasks never touch windows directly. The shell asks the desktop for things through a
small **mailbox** (`wm_post`, `wm_notify`, `wm_open_app`) and the desktop does them on
its own task, so no locks are needed around the window list.

### Windows for programs (`gui/uwin.c`)

A ring-3 program can't touch the window list, so it works the way an X11 or Wayland
client talks to its display server:

1. `SYS_WINDOW` asks for a window. The program's task can't create it directly (the
   window list belongs to the desktop task), so it hands the job over with `wm_call`
   and waits a frame for it to be done. It gets back a handle, a small number.
2. `SYS_DRAW` sends a batch of drawing commands (`struct dcmd`: fill, rounded box, text,
   button...). The kernel runs them into the window's surface with its own fonts and
   colours, so programs stay small and look like everything else.
3. `SYS_EVENT` waits for keys, clicks, resizes and the close button. The window's
   callbacks (run by the desktop task) push events into a little queue per window.

Programs draw from their own tasks, so **`wm_surface_lock`** keeps them and the
desktop apart: a program holds it for a whole batch, and the desktop holds it while it
collects damage and composes, and while it swaps a surface (resize) or frees one
(close). Without it, the desktop could copy a window to the screen halfway through a
batch and never notice the rest (that bug happened, and this lock is the fix).

When a program ends, its windows are closed on the next frame; when the desktop stops,
programs with windows are stopped too.

The **terminal** is the shell's console, retargeted to draw into a window's surface.
The shell reads keys from the terminal's queue instead of the keyboard. It doesn't know
it's in a window.

## 9. Settings (`kernel/sys/config.c`)

`config_get`/`config_set` read and write `key=value` lines in `/etc/dream.conf`. Because
it's an ordinary file, dreamfs saves it with everything else and it's there after a
reboot. The lock screen password is a salted, repeated FNV-1a hash in `/etc/passwd.dream`.
