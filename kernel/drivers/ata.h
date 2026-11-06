#pragma once
#include "kernel.h"

#define SECTOR 512

struct disk {
    char     name[8];        /* sda, sdb... for disks; sr0 for CD drives */
    char     model[41];
    uint64_t sectors;        /* size in 512-byte sectors (0 for CD drives) */
    bool     atapi;          /* a CD/DVD drive: different command set, not readable yet */
    uint16_t io, ctrl;       /* the controller's port range */
    uint8_t  drive;          /* 0 = master, 1 = slave */
};

enum { ATA_EIO = -1, ATA_ETIMEOUT = -2, ATA_EUNSUPPORTED = -3, ATA_ERANGE = -4 };

void ata_init(void);
int  ata_count(void);
struct disk *ata_get(int i);
struct disk *ata_find(const char *name);

/* Move whole sectors between the disk and memory. 0 on success. */
int  ata_read(struct disk *d, uint64_t lba, uint32_t count, void *buf);
int  ata_write(struct disk *d, uint64_t lba, uint32_t count, const void *buf);
const char *ata_strerror(int err);
