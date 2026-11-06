#include "fs/diskfs.h"
#include "fs/ramfs_internal.h"
#include "mm/heap.h"

#define MAGIC        "DREAMFS1"
#define VERSION      1
#define TABLE_LBA    1
/* the table's size depends on how many nodes it holds; the superblock records that,
   so disks saved when dream allowed 256 nodes still load now that it allows 1024 */
#define TABLE_SECTORS_FOR(n) (((n) * sizeof(struct dnode) + SECTOR - 1) / SECTOR)
#define TABLE_SECTORS TABLE_SECTORS_FOR(FS_MAX_NODES)
#define DATA_LBA     (TABLE_LBA + TABLE_SECTORS)

struct __attribute__((packed)) superblock {
    char     magic[8];
    uint32_t version;
    uint32_t nodes;          /* entries in the node table */
    uint32_t table_lba;
    uint32_t data_lba;
    uint32_t data_sectors;   /* how much of the data area is in use */
    uint32_t checksum;       /* of the node table, to catch half-written saves */
    uint64_t saves;
    uint8_t  unused[SECTOR - 40];
};

/* One file or directory, as stored on disk. */
struct __attribute__((packed)) dnode {
    uint8_t  used, dir;
    uint16_t reserved;
    int32_t  parent;
    uint32_t size;
    uint32_t data;           /* first sector of its contents, relative to data_lba */
    char     name[FS_NAME_MAX];
    struct rtc_time mtime;
};

_Static_assert(sizeof(struct superblock) == SECTOR, "superblock must be one sector");

static struct superblock sb __attribute__((aligned(16)));
static struct dnode table[TABLE_SECTORS * SECTOR / sizeof(struct dnode)] __attribute__((aligned(16)));
static struct disk *mounted;

static uint32_t fnv1a(const void *p, size_t n) {
    const uint8_t *b = p;
    uint32_t h = 2166136261u;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

static bool all_zero(const void *p, size_t n) {
    const uint8_t *b = p;
    while (n--) if (*b++) return false;
    return true;
}

static int do_save(struct disk *d) {
    if (!d) return DFS_NODISK;
    memset(table, 0, sizeof table);
    uint32_t next = 0;
    int files = 0;

    /* 1. file contents first... */
    for (int i = 0; i < FS_MAX_NODES; i++) {
        struct fs_node *n = &fs_nodes[i];
        if (!n->used) continue;
        struct dnode *t = &table[i];
        t->used = 1;
        t->dir = n->dir;
        t->parent = n->parent;
        t->size = n->size;
        t->mtime = n->mtime;
        strlcpy(t->name, n->name, FS_NAME_MAX);
        if (n->dir) continue;

        uint32_t secs = (n->size + SECTOR - 1) / SECTOR;
        if (DATA_LBA + next + secs > d->sectors) return DFS_FULL;
        t->data = next;
        if (secs) {
            int r = ata_write(d, DATA_LBA + next, secs, n->data);   /* cap is sector-rounded */
            if (r) return r;
        }
        next += secs;
        files++;
    }

    /* 2. ...then the table that points at them... */
    int r = ata_write(d, TABLE_LBA, TABLE_SECTORS, table);
    if (r) return r;

    /* 3. ...and the superblock last, so a save cut off halfway is caught by the checksum. */
    uint64_t saves = !memcmp(sb.magic, MAGIC, 8) ? sb.saves + 1 : 1;
    memset(&sb, 0, sizeof sb);
    memcpy(sb.magic, MAGIC, 8);
    sb.version = VERSION;
    sb.nodes = FS_MAX_NODES;
    sb.table_lba = TABLE_LBA;
    sb.data_lba = DATA_LBA;
    sb.data_sectors = next;
    sb.checksum = fnv1a(table, sizeof table);
    sb.saves = saves;
    if ((r = ata_write(d, 0, 1, &sb))) return r;

    fs_dirty = false;
    return files;
}

static int do_load(struct disk *d) {
    if (!d) return DFS_NODISK;
    int r = ata_read(d, 0, 1, &sb);
    if (r) return r;
    if (all_zero(&sb, sizeof sb)) return DFS_BLANK;
    if (memcmp(sb.magic, MAGIC, 8) || sb.version != VERSION || !sb.nodes || sb.nodes > FS_MAX_NODES) {
        memset(&sb, 0, sizeof sb);
        return DFS_FOREIGN;
    }
    uint32_t tsecs = TABLE_SECTORS_FOR(sb.nodes);
    memset(table, 0, sizeof table);
    if ((r = ata_read(d, sb.table_lba, tsecs, table))) return r;
    if (fnv1a(table, tsecs * SECTOR) != sb.checksum || !table[0].used || !table[0].dir) return DFS_CORRUPT;

    /* Everything checks out: replace what's in memory with what's on disk. */
    fs_clear();
    int files = 0;
    for (uint32_t i = 0; i < sb.nodes; i++) {
        struct dnode *t = &table[i];
        if (!t->used) continue;
        struct fs_node *n = &fs_nodes[i];
        n->used = true;
        n->dir = t->dir;
        n->parent = t->parent >= 0 && t->parent < FS_MAX_NODES ? t->parent : 0;
        n->mtime = t->mtime;
        memcpy(n->name, t->name, FS_NAME_MAX);
        n->name[FS_NAME_MAX - 1] = 0;
        if (t->dir) continue;

        n->size = t->size <= FS_FILE_MAX ? t->size : FS_FILE_MAX;
        uint32_t secs = (n->size + SECTOR - 1) / SECTOR;
        if (secs) {
            n->cap = secs * SECTOR;
            n->data = kmalloc(n->cap);
            if (!n->data) { n->used = false; continue; }
            if ((r = ata_read(d, sb.data_lba + t->data, secs, n->data))) return r;
        }
        files++;
    }
    fs_rehome();
    fs_dirty = false;
    return files;
}

/* Hold the filesystem lock for the whole save, so the copy on disk is consistent. */
int diskfs_save(struct disk *d) { fs_lock(); int r = do_save(d); fs_unlock(); return r; }
int diskfs_load(struct disk *d) { fs_lock(); int r = do_load(d); fs_unlock(); return r; }

void diskfs_mount(struct disk *d) { mounted = d; }
struct disk *diskfs_mounted(void) { return mounted; }
uint64_t diskfs_saves(void) { return sb.saves; }

int diskfs_autosave(void) {
    fs_lock();
    int r = mounted && fs_dirty ? do_save(mounted) : 0;
    fs_unlock();
    return r < 0 ? r : 0;
}

const char *diskfs_strerror(int err) {
    switch (err) {
    case DFS_FOREIGN: return "the disk holds something that isn't dreamfs";
    case DFS_CORRUPT: return "the saved filesystem is damaged (checksum mismatch)";
    case DFS_FULL:    return "the disk is full";
    case DFS_NODISK:  return "no disk";
    default:          return ata_strerror(err);
    }
}
