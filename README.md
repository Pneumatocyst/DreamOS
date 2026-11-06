# dream

A small, good-looking x86_64 hobby OS for learning how computers really start up.
Written in C, booted by [Limine](https://codeberg.org/Limine/Limine), run in QEMU.

```
make run          # build dream.iso and boot it (serial log appears in this terminal)
make run-uefi     # same, with UEFI firmware
make debug        # boot paused, GDB attached, breakpoint on kmain
```

Needs: `gcc make limine xorriso qemu` →
`sudo pacman -S --needed qemu-desktop libisoburn edk2-ovmf`

## What's in it (v0.1)

- **Framebuffer console**: Terminus 8x16 font, gradient background, status bar with clock
- **4 themes**: `midnight`, `nostromo`, `cosmos`, `ember`; switch live with `theme <name>`
- **Interrupts**: own GDT and IDT, remapped PIC, 100 Hz PIT timer, PS/2 keyboard
- **Memory allocator**: a bitmap page allocator for physical RAM, plus `kmalloc`/`kfree`
  (size classes for small blocks, whole pages for big ones). `free`, `memtest`
- **ATA disk driver** (PIO): finds IDE disks and CD drives, reads and writes sectors
- **dreamfs**: the file tree is saved to `disk.img` after every change and restored at boot.
  Layout: sector 0 superblock, 1-28 node table, 29+ file data. `make wipe-disk` starts fresh.
- **Shell** with pipes `|`, redirects `>` `>>`, `;`, `&&`, "quotes" and history
  - files: `ls cd pwd tree mkdir touch cat cp mv rm rmdir`
  - text: `echo grep head tail wc`
  - disks: `lsblk blkread blkwrite hexdump sync mkfs`
  - system: `uname hostname whoami date uptime free mem lscpu dmesg history sleep`
  - dream: `help fetch theme clear hello crash reboot shutdown`
- **edit** (also `nano`): full-screen editor with line numbers, auto-indent, cut/paste,
  find and go-to-line. ^S save, ^Q quit, ^K cut, ^U paste, ^F find, ^G go to line
- **PS/2 mouse** with scroll wheel. In QEMU, click into the window to capture the mouse;
  Ctrl+Alt+G releases it
- **Desktop** (`startx`, leave with `exit`): double-buffered compositor with a wallpaper,
  draggable windows with rounded corners and shadows, a top panel with app buttons and
  a clock. Apps: terminal (the real shell), files, paint, clock, about. Follows the theme.
- **File browser** (`files`, or the panel): folders and files with icons and sizes, a
  preview pane, new folder / new file / rename / delete, keyboard or mouse. Opening a
  text file edits it in the terminal; opening a program runs it there. Updates live.
- **Multitasking**: kernel tasks with their own stacks, a preemptive round-robin scheduler
  (10 ms slices off the timer), sleeping, and an idle task. The shell and the desktop run
  side by side; `ps` lists tasks and their CPU time
- **User mode**: programs run in ring 3, each in its own address space (4-level page
  tables, kernel half shared but not user-accessible). TSS for ring 3 -> 0, `int 0x80`
  system calls with checked user pointers, an ELF loader, argc/argv, pipes into programs,
  Ctrl+C. A crashing program gets a "segmentation fault"; the kernel carries on.
  Programs live in `user/`, get built separately and installed into `/bin` at boot:
  `hi`, `counter`, `spin`, `crashme`, `upper`, `guess`
- **Panic screen** with a register dump. Try `crash pf`, then `addr2line` the RIP.
- **Serial log** mirrored to your terminal

## Map of the source

```
kernel/main.c            boot sequence, Limine requests, memory map
kernel/arch/             the CPU: gdt, idt + isr.S stubs, pic, pit, port I/O
kernel/drivers/          fb (pixels + glyphs), console (text grid), theme, keyboard, serial
kernel/lib/              string, printf, the embedded font
kernel/mm/pmm.c          physical page allocator (one bit per 4 KiB page)
kernel/mm/heap.c         kmalloc / kfree
kernel/drivers/ata.c     the disk driver
kernel/fs/ramfs.c        the in-memory file tree
kernel/fs/diskfs.c       saving/loading that tree to disk (dreamfs)
kernel/shell/shell.c     line editor, pipes/redirects, command table; add commands here
kernel/shell/cmd_*.c     the Linux-style commands
kernel/apps/edit.c       the text editor
kernel/gfx/gfx.c         drawing: surfaces, shapes, smooth lines, text, clipping, damage
kernel/mm/vmm.c          page tables: per-program address spaces
kernel/proc/process.c    ELF loader, system calls, killing crashed programs
kernel/arch/user.S       the door into ring 3 (iretq)
user/lib/                the tiny C library programs link against (syscalls, printf...)
user/*.c                 the programs. add a .c file here and it appears in /bin
kernel/sched/task.c      tasks and the scheduler
kernel/sched/switch.S    the context switch (16 instructions)
kernel/gui/wm.c          window manager + compositor + panel (runs as the "desktop" task)
kernel/gui/term.c        the terminal window (the shell draws into it)
kernel/gui/files.c       the file browser
kernel/gui/clock.c       analog clock          kernel/gui/about.c   about window
kernel/apps/paint.c      the paint program
kernel/drivers/mouse.c   the mouse driver
kernel/panic.c           the red screen
linker.ld                where everything is placed in memory (higher half)
```

Suggested reading order: `main.c` → `arch/idt.c` + `arch/isr.S` → `drivers/console.c` → `shell/shell.c`.

## Roadmap: one lesson per step

1. **Your own command.** Add a `color` or `cowsay` command to `shell.c`.
2. ~~**Physical memory manager.**~~ done A bitmap of free 4 KiB frames built from the memory map; `mem` shows free/used.
3. ~~**Kernel heap.**~~ done `kmalloc` / `kfree` on top of the frame allocator.
4. **Paging.** Build your own page tables instead of Limine's; map and unmap pages.
5. ~~**Tasks.**~~ done Kernel threads and a round-robin scheduler driven by the PIT.
6. ~~**User mode.**~~ done TSS, `syscall`/`sysret`, run a ring-3 program.
7. **Files.** Load a ramdisk as a Limine module, read a tar archive, add `ls` and `cat`.
8. ~~**Graphics.**~~ done: double buffering, mouse, windows, desktop.

## Resources

- OSDev wiki: <https://wiki.osdev.org>
- *Operating Systems: Three Easy Pieces*: <https://pages.cs.wisc.edu/~remzi/OSTEP/>
- Limine protocol: <https://codeberg.org/Limine/limine-protocol>
- Intel SDM Vol. 3 (system programming), for when you want the real answers
