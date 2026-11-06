/* Disk commands: look at drives, peek and poke raw sectors, save the filesystem. */
#include "shell/shell.h"
#include "drivers/ata.h"
#include "fs/diskfs.h"
#include "fs/ramfs.h"

static void size_str(char *out, size_t n, uint64_t sectors) {
    uint64_t kib = sectors / 2;
    if (kib >= 1024 * 1024) snprintf(out, n, "%lu.%luG", kib >> 20, ((kib & 0xfffff) * 10) >> 20);
    else                    snprintf(out, n, "%lu.%luM", kib >> 10, ((kib & 0x3ff) * 10) >> 10);
}

SH_CMD(lsblk) {
    (void)argc; (void)argv;
    kprintf(C_DIM "%-6s %8s  %-5s %-20s %s\n", "NAME", "SIZE", "TYPE", "MODEL", "MOUNT");
    for (int i = 0; i < ata_count(); i++) {
        struct disk *d = ata_get(i);
        char size[16] = "-";
        if (!d->atapi) size_str(size, sizeof size, d->sectors);
        kprintf(C_ACCENT "%-6s " C_FG "%8s  %-5s %-20s ", d->name, size, d->atapi ? "rom" : "disk", d->model);
        if (d == diskfs_mounted()) kprintf(C_OK "/" C_DIM " (dreamfs)");
        kprintf("\n");
    }
    if (!ata_count()) kprintf(C_DIM "no drives found\n");
}

/* The classic hex + ASCII view: offset, 16 bytes in hex, the same bytes as text. */
static void hexdump(const uint8_t *p, size_t len, uint64_t base) {
    for (size_t off = 0; off < len; off += 16) {
        kprintf(C_DIM "%08lx  ", base + off);
        for (size_t i = 0; i < 16; i++) {
            if (off + i < len) kprintf("%s%02x ", p[off + i] ? C_FG : C_DIM, p[off + i]);
            else kprintf("   ");
            if (i == 7) kprintf(" ");
        }
        kprintf(C_DIM " |" C_ACCENT);
        for (size_t i = 0; i < 16 && off + i < len; i++) {
            uint8_t c = p[off + i];
            kprintf("%c", c >= 32 && c < 127 ? c : '.');
        }
        kprintf(C_DIM "|\n");
    }
}

static struct disk *disk_arg(const char *cmd, const char *name) {
    struct disk *d = ata_find(name);
    if (!d) kerrorf(C_ERR "%s" C_DIM ": %s: " C_FG "no such disk (see lsblk)\n", cmd, name);
    return d;
}

SH_CMD(blkread) {
    uint64_t lba;
    if (argc != 3 || !sh_parse_num(argv[2], &lba)) { kerrorf("usage: blkread <disk> <sector>    e.g. blkread sda 0\n"); return; }
    struct disk *d = disk_arg("blkread", argv[1]);
    if (!d) return;
    static uint8_t buf[SECTOR] __attribute__((aligned(16)));
    int r = ata_read(d, lba, 1, buf);
    if (r) { kerrorf(C_ERR "blkread" C_DIM ": " C_FG "%s\n", ata_strerror(r)); return; }
    kprintf(C_DIM "%s sector %lu (byte %lu)\n", d->name, lba, lba * SECTOR);
    hexdump(buf, SECTOR, lba * SECTOR);
}

SH_CMD(blkwrite) {
    uint64_t lba;
    if (argc != 4 || !sh_parse_num(argv[2], &lba)) {
        kerrorf("usage: blkwrite <disk> <sector> \"text\"    (sector 0-28 holds dreamfs: pick 10000+)\n");
        return;
    }
    struct disk *d = disk_arg("blkwrite", argv[1]);
    if (!d) return;
    static uint8_t buf[SECTOR] __attribute__((aligned(16)));
    memset(buf, 0, sizeof buf);
    strlcpy((char *)buf, argv[3], sizeof buf);
    int r = ata_write(d, lba, 1, buf);
    if (r) { kerrorf(C_ERR "blkwrite" C_DIM ": " C_FG "%s\n", ata_strerror(r)); return; }
    kprintf(C_OK "wrote" C_FG " %u bytes" C_DIM " to %s sector %lu. read it back: " C_FG "blkread %s %lu\n",
            (unsigned)strlen(argv[3]), d->name, lba, d->name, lba);
    if (d == diskfs_mounted() && lba < 64)
        kprintf(C_WARN "careful:" C_DIM " low sectors hold dreamfs; the next save will rewrite them.\n");
}

SH_CMD(hexdump) {
    size_t len;
    const char *d = sh_input(argc, argv, 1, &len);
    if (d) hexdump((const uint8_t *)d, len, 0);
}

SH_CMD(sync) {
    (void)argc; (void)argv;
    struct disk *d = diskfs_mounted();
    if (!d) { kerrorf(C_ERR "sync" C_DIM ": no disk mounted; files only live in RAM\n"); return; }
    int r = diskfs_save(d);
    if (r < 0) { kerrorf(C_ERR "sync" C_DIM ": " C_FG "%s\n", diskfs_strerror(r)); return; }
    kprintf(C_OK "saved" C_FG " %d files" C_DIM " to %s (save #%lu)\n", r, d->name, diskfs_saves());
}

SH_CMD(mkfs) {
    if (argc != 2) { kerrorf("usage: mkfs <disk>    " C_WARN "erases the disk" C_DIM " and starts a fresh dreamfs\n"); return; }
    struct disk *d = disk_arg("mkfs", argv[1]);
    if (!d) return;
    fs_init();                      /* a fresh default tree... */
    sh_set_cwd(fs_home());
    int r = diskfs_save(d);         /* ...written over whatever was there */
    if (r < 0) { kerrorf(C_ERR "mkfs" C_DIM ": " C_FG "%s\n", diskfs_strerror(r)); return; }
    diskfs_mount(d);
    kprintf(C_OK "formatted" C_FG " %s" C_DIM " with dreamfs and mounted it at /\n", d->name);
}
