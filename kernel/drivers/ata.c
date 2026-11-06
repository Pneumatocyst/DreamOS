/* ATA disk driver, PIO mode.
 *
 * The classic IDE controller has two "buses" (primary at port 0x1f0, secondary
 * at 0x170), each with up to two drives (master and slave). Talking to a drive is:
 *
 *   1. pick the drive and the sector number by writing to a few I/O ports
 *   2. write a command (read sectors, write sectors, identify...)
 *   3. wait until the status port says the drive is ready (DRQ)
 *   4. move the 512-byte sector through the data port, 2 bytes at a time
 *
 * "PIO" means the CPU does that copying itself. Real drivers use DMA (the disk
 * writes straight into memory) and interrupts instead of polling, but PIO is
 * the clearest way to see what's going on. */
#include "drivers/ata.h"
#include "arch/io.h"
#include "sched/task.h"

/* register offsets from the bus's io base */
#define REG_DATA     0
#define REG_ERROR    1
#define REG_COUNT    2
#define REG_LBA_LO   3
#define REG_LBA_MID  4
#define REG_LBA_HI   5
#define REG_DRIVE    6
#define REG_STATUS   7   /* reading */
#define REG_COMMAND  7   /* writing */

/* status bits */
#define ST_ERR  0x01
#define ST_DRQ  0x08     /* data request: ready to move a sector */
#define ST_DF   0x20     /* drive fault */
#define ST_BSY  0x80

/* commands */
#define CMD_READ          0x20
#define CMD_WRITE         0x30
#define CMD_FLUSH         0xe7
#define CMD_IDENTIFY      0xec
#define CMD_IDENTIFY_PKT  0xa1

static const struct { uint16_t io, ctrl; } buses[2] = { { 0x1f0, 0x3f6 }, { 0x170, 0x376 } };

static struct disk disks[4];
static int ndisks, nsd, nsr;

/* Reading the alternate status port takes ~100ns; four reads give the
   drive the 400ns it needs after we select it. */
static void delay400(uint16_t ctrl) { for (int i = 0; i < 4; i++) inb(ctrl); }

static int wait(uint16_t io, bool want_drq) {
    for (uint32_t i = 0; i < 2000000; i++) {
        uint8_t s = inb(io + REG_STATUS);
        if (s & ST_BSY) continue;
        if (s & (ST_ERR | ST_DF)) return ATA_EIO;
        if (!want_drq || (s & ST_DRQ)) return 0;
    }
    return ATA_ETIMEOUT;
}

/* ATA strings are stored with each pair of bytes swapped. */
static void ata_string(char *out, const uint16_t *words, int nwords) {
    for (int i = 0; i < nwords; i++) {
        out[i * 2]     = words[i] >> 8;
        out[i * 2 + 1] = words[i] & 0xff;
    }
    int n = nwords * 2;
    out[n] = 0;
    while (n > 0 && out[n - 1] == ' ') out[--n] = 0;
}

static void probe(int bus, int drive) {
    uint16_t io = buses[bus].io, ctrl = buses[bus].ctrl;
    outb(ctrl, 0x02);                           /* nIEN: no interrupts, we'll poll */
    outb(io + REG_DRIVE, 0xa0 | drive << 4);
    delay400(ctrl);
    outb(io + REG_COUNT, 0);
    outb(io + REG_LBA_LO, 0);
    outb(io + REG_LBA_MID, 0);
    outb(io + REG_LBA_HI, 0);
    outb(io + REG_COMMAND, CMD_IDENTIFY);

    uint8_t s = inb(io + REG_STATUS);
    if (s == 0 || s == 0xff) return;            /* nobody home */
    for (uint32_t i = 0; i < 2000000 && (inb(io + REG_STATUS) & ST_BSY); i++) {}

    /* CD drives refuse IDENTIFY and leave a signature in the LBA registers. */
    bool atapi = false;
    uint8_t mid = inb(io + REG_LBA_MID), hi = inb(io + REG_LBA_HI);
    if (mid == 0x14 && hi == 0xeb) {
        atapi = true;
        outb(io + REG_COMMAND, CMD_IDENTIFY_PKT);
        delay400(ctrl);
    } else if (mid || hi) {
        return;                                 /* SATA or something else */
    }
    if (wait(io, true) < 0) return;

    static uint16_t id[256];
    insw(io + REG_DATA, id, 256);

    struct disk *d = &disks[ndisks++];
    d->io = io;
    d->ctrl = ctrl;
    d->drive = drive;
    d->atapi = atapi;
    ata_string(d->model, id + 27, 20);
    if (atapi) {
        snprintf(d->name, sizeof d->name, "sr%d", nsr++);
        d->sectors = 0;
    } else {
        snprintf(d->name, sizeof d->name, "sd%c", 'a' + nsd++);
        bool lba48 = id[83] & (1 << 10);
        d->sectors = lba48 ? (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32
                           : (uint64_t)id[60] | (uint64_t)id[61] << 16;
    }
}

void ata_init(void) {
    for (int bus = 0; bus < 2; bus++) {
        if (inb(buses[bus].io + REG_STATUS) == 0xff) continue;   /* floating bus: no controller */
        for (int drive = 0; drive < 2; drive++) probe(bus, drive);
    }
}

int ata_count(void) { return ndisks; }
struct disk *ata_get(int i) { return i >= 0 && i < ndisks ? &disks[i] : NULL; }

struct disk *ata_find(const char *name) {
    if (!strncmp(name, "/dev/", 5)) name += 5;
    for (int i = 0; i < ndisks; i++) if (!strcmp(disks[i].name, name)) return &disks[i];
    return NULL;
}

/* We use 28-bit LBA: sector numbers up to 2^28, i.e. disks up to 128 GiB. */
static int transfer(struct disk *d, uint64_t lba, uint32_t count, void *buf, bool write) {
    if (d->atapi) return ATA_EUNSUPPORTED;
    if (lba + count > d->sectors || lba + count > (1u << 28)) return ATA_ERANGE;
    uint16_t io = d->io;
    uint16_t *p = buf;

    while (count) {
        uint32_t n = count > 256 ? 256 : count;      /* one command moves at most 256 sectors */
        outb(io + REG_DRIVE, 0xe0 | d->drive << 4 | ((lba >> 24) & 0x0f));
        delay400(d->ctrl);
        int r = wait(io, false);
        if (r) return r;
        outb(io + REG_COUNT, n == 256 ? 0 : n);       /* 0 means 256 */
        outb(io + REG_LBA_LO, lba);
        outb(io + REG_LBA_MID, lba >> 8);
        outb(io + REG_LBA_HI, lba >> 16);
        outb(io + REG_COMMAND, write ? CMD_WRITE : CMD_READ);

        for (uint32_t k = 0; k < n; k++) {
            if ((r = wait(io, true))) return r;
            if (write) {
                /* word by word: some real drives choke on `rep outsw` */
                for (int w = 0; w < 256; w++) outw(io + REG_DATA, p[w]);
            } else {
                insw(io + REG_DATA, p, 256);
            }
            p += 256;
        }
        if (write) {
            outb(io + REG_COMMAND, CMD_FLUSH);       /* make the drive commit its cache */
            if ((r = wait(io, false))) return r;
        }
        lba += n;
        count -= n;
    }
    return 0;
}

/* The controller does one thing at a time; tasks take turns. */
static struct mutex ata_mutex;

int ata_read(struct disk *d, uint64_t lba, uint32_t count, void *buf) {
    mutex_lock(&ata_mutex);
    int r = transfer(d, lba, count, buf, false);
    mutex_unlock(&ata_mutex);
    return r;
}

int ata_write(struct disk *d, uint64_t lba, uint32_t count, const void *buf) {
    mutex_lock(&ata_mutex);
    int r = transfer(d, lba, count, (void *)buf, true);
    mutex_unlock(&ata_mutex);
    return r;
}

const char *ata_strerror(int err) {
    switch (err) {
    case ATA_EIO:          return "the drive reported an error";
    case ATA_ETIMEOUT:     return "the drive stopped responding";
    case ATA_EUNSUPPORTED: return "CD drives aren't supported yet";
    case ATA_ERANGE:       return "sector is past the end of the disk";
    default:               return "unknown error";
    }
}
