# Building and running dream

## What you need

| tool | why |
|---|---|
| `gcc`, `make`, `ld` (binutils) | compile the kernel and the user programs |
| `limine` | the bootloader: its files go on the ISO, and its `limine` tool makes the ISO bootable from BIOS |
| `xorriso` | builds the ISO image |
| `qemu-system-x86_64` | the virtual PC dream runs in |
| `edk2-ovmf` / `ovmf` (optional) | UEFI firmware, for `make run-uefi` |
| `gdb` (optional) | for `make debug` |

### Arch Linux

```sh
sudo pacman -S --needed gcc make limine qemu-desktop libisoburn edk2-ovmf
```

### Debian / Ubuntu

Most of it is packaged:

```sh
sudo apt install build-essential xorriso qemu-system-x86 ovmf
```

Limine usually isn't, so build its small host tool from the official binary release:

```sh
git clone --depth 1 --branch v9.x-binary https://github.com/limine-bootloader/limine.git
make -C limine
sudo cp limine/limine /usr/local/bin/
sudo mkdir -p /usr/share/limine
sudo cp limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin limine/BOOTX64.EFI /usr/share/limine/
```

The Makefile asks `limine --print-datadir` where Limine's files are and falls back to
`/usr/share/limine`. On Debian the UEFI firmware lives at `/usr/share/ovmf/OVMF.fd`, so
run UEFI with `make run-uefi OVMF=/usr/share/ovmf/OVMF.fd`.

## Make targets

```sh
make            # kernel + user programs + dream.iso
make run        # boot it in QEMU (BIOS); the serial log prints in your terminal
make run-uefi   # the same with UEFI firmware
make debug      # QEMU paused, GDB attached, breakpoint on kmain
make wipe-disk  # a fresh, blank disk.img (dream formats it on boot)
make clean      # remove build/ and the ISO (keeps disk.img)
```

`disk.img` is a 32 MiB raw virtual hard disk. dream saves its whole file tree there
after every change (and your settings in `/etc/dream.conf` with it), so things you
create are still there next time. It's ignored by git.

The QEMU machine is `-M pc` (the classic i440FX chipset) because its IDE controller
is what dream's disk driver speaks.

## Fonts

The desktop's smooth text comes from `kernel/gfx/uifont_data.c`, which is generated
from the Inter fonts in `assets/fonts/` by `tools/mkfont.py` and committed, so a
normal build doesn't need Python. To change sizes or fonts:

```sh
pip install pillow
python3 tools/mkfont.py > kernel/gfx/uifont_data.c
```

## Testing without a screen

`tools/qemu_drive.py` boots dream headlessly (with a scratch disk, never your `disk.img`),
plays a script of keystrokes and mouse moves, and saves screenshots. It's how every
feature was checked.

```sh
make
python3 tools/qemu_drive.py script.txt out/
```

A script looks like this:

```
wait 6                      # seconds (boot + desktop takes about 5)
type open files\n           # type text; \n is enter
key alt-tab                 # any QEMU key name: ret, esc, f11, meta_l, ctrl-alt-t...
mouse -2000 -2000           # move the mouse (relative); this parks it in the top-left
mouse 640 400
click                       # or: click right
down                        # hold the left button...
mouse 300 0
up                          # ...and let go (a drag)
shot desktop                # writes out/desktop.png
```

Things to know:

- keys go to the **focused window**. `ctrl-alt-t` brings the terminal back to the front.
- simulated mouse movement drifts a little; park the mouse in a corner first (as above)
  for positions you can rely on, and prefer keyboard shortcuts.
- the serial log ends up in `out/serial.log`.

Screenshots need ImageMagick (`convert`) to turn QEMU's PPM files into PNGs.

## Debugging tips

- Everything the kernel prints also goes to the serial port, so it's in your terminal
  (or `serial.log`) even if the screen is broken.
- On a crash dream shows a red panic screen with the registers. Find the line with
  `addr2line -e build/dream.elf <RIP>`.
- `make debug`, then in GDB: `break <function>`, `continue`, `bt`, `info registers`.
- `dmesg` in dream's shell shows the boot log.
