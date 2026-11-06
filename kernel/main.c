/* Limine jumps here in 64-bit mode with paging on, a stack, and a framebuffer.
   Our job: set up the CPU tables and devices, then hand over to the shell. */
#include "kernel.h"
#include "arch/cpu.h"
#include "arch/io.h"
#include "drivers/fb.h"
#include "drivers/console.h"
#include "drivers/serial.h"
#include "drivers/keyboard.h"
#include "fs/ramfs.h"
#include "fs/diskfs.h"
#include "drivers/ata.h"
#include "mm/pmm.h"
#include "sched/task.h"
#include "mm/vmm.h"
#include "shell/shell.h"
#include "drivers/mouse.h"
#include <limine.h>

/* ---- requests to the bootloader ------------------------------------------ */
__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t requests_start[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests")))
static volatile uint64_t base_revision[] = LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_req = {
    .id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0,
};

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t requests_end[] = LIMINE_REQUESTS_END_MARKER;

/* ---- memory map ------------------------------------------------------------ */
static const char *mem_type(uint64_t t) {
    switch (t) {
    case LIMINE_MEMMAP_USABLE:                 return C_OK "usable";
    case LIMINE_MEMMAP_RESERVED:               return C_DIM "reserved";
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       return C_WARN "acpi reclaimable";
    case LIMINE_MEMMAP_ACPI_NVS:               return C_WARN "acpi nvs";
    case LIMINE_MEMMAP_BAD_MEMORY:             return C_ERR "bad";
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return C_ACCENT "bootloader";
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return C_ACCENT "kernel";
    case LIMINE_MEMMAP_FRAMEBUFFER:            return C_ACCENT "framebuffer";
    default:                                   return "?";
    }
}

uint64_t mem_usable_bytes(void) {
    uint64_t total = 0;
    if (!memmap_req.response) return 0;
    for (uint64_t i = 0; i < memmap_req.response->entry_count; i++) {
        struct limine_memmap_entry *e = memmap_req.response->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE) total += e->length;
    }
    return total;
}

void mem_print_map(void) {
    if (!memmap_req.response) { kprintf(C_ERR "no memory map\n"); return; }
    kprintf(C_DIM "  %-18s %-18s %10s  type\n", "start", "end", "size");
    for (uint64_t i = 0; i < memmap_req.response->entry_count; i++) {
        struct limine_memmap_entry *e = memmap_req.response->entries[i];
        uint64_t kib = e->length / 1024;
        kprintf("  %016lx   %016lx   %7lu %s  %s\n", e->base, e->base + e->length,
                kib >= 4096 ? kib / 1024 : kib, kib >= 4096 ? "MiB" : "KiB", mem_type(e->type));
    }
    kprintf(C_DIM "  usable total: " C_FG "%lu MiB\n", mem_usable_bytes() >> 20);
}

/* ---- boot ------------------------------------------------------------------ */
static void ok(const char *what) {
    klog(what);
    kprintf(C_DIM "[" C_OK "  ok  " C_DIM "] " C_FG "%s\n", what);
}

static void warn(const char *what) {
    klog(what);
    kprintf(C_DIM "[" C_WARN " warn " C_DIM "] " C_FG "%s\n", what);
}

/* Find the first hard disk and load (or create) the saved filesystem on it. */
static void mount_root(void) {
    char m[128];
    for (int i = 0; i < ata_count(); i++) {
        struct disk *d = ata_get(i);
        if (d->atapi) snprintf(m, sizeof m, "%s: %s (cd drive)", d->name, d->model);
        else snprintf(m, sizeof m, "%s: %s, %lu MiB", d->name, d->model, d->sectors / 2048);
        ok(m);
    }

    struct disk *disk = NULL;
    for (int i = 0; i < ata_count() && !disk; i++) if (!ata_get(i)->atapi) disk = ata_get(i);
    if (!disk) { warn("no hard disk: files live in RAM and vanish on reboot"); return; }

    int r = diskfs_load(disk);
    if (r >= 0) {
        diskfs_mount(disk);
        snprintf(m, sizeof m, "dreamfs on %s: restored %d files (save #%lu)", disk->name, r, diskfs_saves());
        ok(m);
    } else if (r == DFS_BLANK) {
        r = diskfs_save(disk);
        if (r >= 0) {
            diskfs_mount(disk);
            snprintf(m, sizeof m, "%s was blank: formatted it with dreamfs", disk->name);
            ok(m);
        }
    }
    if (r < 0) {
        snprintf(m, sizeof m, "%s: %s. running from RAM; `mkfs %s` to format it",
                 disk->name, diskfs_strerror(r), disk->name);
        warn(m);
    }
}

void kmain(void) {
    serial_init();
    if (!LIMINE_BASE_REVISION_SUPPORTED(base_revision)) halt_forever();
    if (!fb_init()) halt_forever();   /* no screen: check the serial log */

    gdt_init();
    idt_init();
    pic_init();
    pit_init(100);
    kbd_init();
    mouse_init();
    sti();

    pmm_init(memmap_req.response);    /* memory first: the screen's back buffer needs it */
    fb_backbuffer();
    vmm_init();
    sched_init("shell");              /* from here on, kmain is a task called "shell" */
    con_init();
    kprintf("\n");
    ok("framebuffer ready");
    ok("GDT loaded: kernel + user segments");
    ok("IDT loaded: 32 exceptions, 16 IRQs");
    ok("PIC remapped to vectors 32-47");
    ok("PIT ticking at 100 Hz");
    ok("scheduler: preemptive, 10 ms time slices");
    ok("PS/2 keyboard listening");
    if (mouse_present()) ok(mouse_has_wheel() ? "PS/2 mouse ready, with scroll wheel" : "PS/2 mouse ready");
    else warn("no PS/2 mouse found");
    char m[64];
    snprintf(m, sizeof m, "page allocator: %lu MiB free in 4 KiB pages",
             (pmm_total_pages() - pmm_used_pages()) * PAGE_SIZE >> 20);
    ok(m);
    fs_init();
    ok("ramfs ready");
    ata_init();
    mount_root();
    snprintf(m, sizeof m, "%d user programs in /bin (they run in ring 3)", sh_install_programs());
    ok(m);
    kprintf("\n" C_ACCENT "  welcome to dream." C_DIM "  type " C_FG "help" C_DIM " to look around, or "
            C_FG "startx" C_DIM " for the desktop.\n\n");

    shell_run();
}
