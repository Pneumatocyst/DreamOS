# dream — a tiny, good-looking x86_64 hobby OS
#
#   make            build the bootable ISO (dream.iso)
#   make run        boot it in QEMU (BIOS), serial log in this terminal
#   make run-uefi   boot it in QEMU with UEFI firmware (needs edk2-ovmf)
#   make debug      boot paused and attach GDB, breakpoint on kmain
#   make clean      remove build output (keeps disk.img)
#   make wipe-disk  start over with a blank disk.img
#   make install    put a `dreamos` command in ~/.local/bin (type dreamos anywhere to boot)

CC      := gcc
LD      := ld
QEMU    := qemu-system-x86_64
LIMINE  := $(shell limine --print-datadir 2>/dev/null || echo /usr/share/limine)
OVMF    := /usr/share/edk2/x64/OVMF.4m.fd

KERNEL  := build/dream.elf
ISO     := dream.iso
DISK    := disk.img

CFLAGS  := -std=gnu11 -O2 -g -pipe -Wall -Wextra \
           -ffreestanding -fno-stack-protector -fno-stack-check -fno-lto \
           -fno-PIC -ffunction-sections -fdata-sections \
           -m64 -march=x86-64 -mabi=sysv -mcmodel=kernel -mno-red-zone \
           -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
           -Iinclude -Ikernel -MMD -MP
LDFLAGS := -nostdlib -static -z max-page-size=0x1000 -z noexecstack \
           --gc-sections -T linker.ld

SRCS    := $(shell find kernel -name '*.c' -o -name '*.S')
OBJS    := $(SRCS:%=build/%.o) build/userbins.S.o

# User programs: compiled separately, for ring 3, against user/lib (no kernel code).
# Unlike the kernel, they may use floating point: the scheduler saves FPU state.
USER_CFLAGS := -std=gnu11 -O2 -pipe -Wall -Wextra -ffreestanding -fno-stack-protector \
               -fno-pic -fno-pie -m64 -march=x86-64 \
               -fno-builtin -Iuser/lib -Iinclude
USER_PROGS  := $(basename $(notdir $(wildcard user/*.c)))
USER_ELFS   := $(USER_PROGS:%=build/user/%.elf)
USER_LIB    := build/user/lib/crt0.o build/user/lib/dream.o build/user/lib/gui.o

# "pc" is the classic PC chipset: its IDE controller is what drivers/ata.c speaks.
QEMUFLAGS := -name dream -M pc -m 512M -cdrom $(ISO) -boot d \
             -drive file=$(DISK),format=raw,if=ide,index=0 \
             -serial stdio -no-reboot -no-shutdown -rtc base=localtime

.PHONY: all run run-uefi debug clean wipe-disk install uninstall
.SECONDARY:

all: $(ISO)

build/%.c.o: %.c
	@mkdir -p $(@D)
	@echo "  CC   $<"
	@$(CC) $(CFLAGS) -c $< -o $@

build/user/lib/%.o: user/lib/%.c user/lib/dream.h user/lib/gui.h include/dream/abi.h
	@mkdir -p $(@D)
	@echo "  CC   $< (user)"
	@$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/lib/%.o: user/lib/%.S
	@mkdir -p $(@D)
	@echo "  AS   $< (user)"
	@$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/%.o: user/%.c user/lib/dream.h user/lib/gui.h include/dream/abi.h
	@mkdir -p $(@D)
	@echo "  CC   $< (user)"
	@$(CC) $(USER_CFLAGS) -c $< -o $@

build/user/%.elf: build/user/%.o $(USER_LIB) user/user.ld
	@echo "  LD   $@"
	@$(LD) -nostdlib -static -z max-page-size=0x1000 -z noexecstack -T user/user.ld $(USER_LIB) $< -o $@

# Bake the programs into the kernel: a table of { name, start, end } plus the files.
build/userbins.S: $(USER_ELFS)
	@echo "  GEN  $@"
	@{ echo '.section .rodata'; echo '.global userbins'; echo '.balign 8'; echo 'userbins:'; \
	   for p in $(USER_PROGS); do echo "  .quad name_$$p, start_$$p, end_$$p"; done; \
	   echo '  .quad 0, 0, 0'; \
	   for p in $(USER_PROGS); do echo "name_$$p: .asciz \"$$p\""; \
	     echo ".balign 16"; echo "start_$$p: .incbin \"build/user/$$p.elf\""; echo "end_$$p:"; done; \
	   echo '.section .note.GNU-stack,"",@progbits'; } > $@

build/userbins.S.o: build/userbins.S
	@echo "  AS   $<"
	@$(CC) $(CFLAGS) -c $< -o $@

build/%.S.o: %.S
	@mkdir -p $(@D)
	@echo "  AS   $<"
	@$(CC) $(CFLAGS) -c $< -o $@

# the font is pulled in with .incbin, so rebuild when it changes
build/kernel/lib/font.S.o: assets/font.psf

$(KERNEL): $(OBJS) linker.ld
	@echo "  LD   $@"
	@$(LD) $(LDFLAGS) $(OBJS) -o $@

$(ISO): $(KERNEL) limine.conf
	@echo "  ISO  $@"
	@rm -rf build/iso && mkdir -p build/iso/boot/limine build/iso/EFI/BOOT
	@cp $(KERNEL) build/iso/boot/dream.elf
	@cp limine.conf build/iso/boot/limine/
	@cp $(LIMINE)/limine-bios.sys $(LIMINE)/limine-bios-cd.bin \
	    $(LIMINE)/limine-uefi-cd.bin build/iso/boot/limine/
	@cp $(LIMINE)/BOOTX64.EFI build/iso/EFI/BOOT/
	@xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin \
	    -no-emul-boot -boot-load-size 4 -boot-info-table -hfsplus \
	    -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin \
	    -efi-boot-part --efi-boot-image --protective-msdos-label \
	    build/iso -o $@ 2>/dev/null
	@limine bios-install $@ 2>/dev/null

# a 32 MiB virtual hard disk; dream formats it on first boot
$(DISK):
	@echo "  DISK $@ (32 MiB, blank)"
	@truncate -s 32M $@

wipe-disk:
	rm -f $(DISK)
	@$(MAKE) --no-print-directory $(DISK)

run: $(ISO) $(DISK)
	$(QEMU) $(QEMUFLAGS)

run-uefi: $(ISO) $(DISK)
	$(QEMU) $(QEMUFLAGS) -bios $(OVMF)

debug: $(ISO) $(DISK)
	$(QEMU) $(QEMUFLAGS) -s -S & \
	gdb $(KERNEL) -ex 'target remote :1234' -ex 'break kmain' -ex 'continue'

clean:
	rm -rf build $(ISO)

# A link, not a copy: the command always runs the script in this checkout.
BINDIR ?= $(HOME)/.local/bin
install:
	@mkdir -p $(BINDIR)
	@ln -sf "$(CURDIR)/tools/dreamos" $(BINDIR)/dreamos
	@echo "installed: $(BINDIR)/dreamos -> $(CURDIR)/tools/dreamos"
	@case ":$$PATH:" in *":$(BINDIR):"*) echo "type 'dreamos' anywhere to boot dream.";; \
	  *) echo "note: $(BINDIR) isn't on your PATH yet. add this to ~/.bashrc:"; \
	     echo "  export PATH=\"$(BINDIR):\$$PATH\"";; esac

uninstall:
	rm -f $(BINDIR)/dreamos

-include $(OBJS:.o=.d)
