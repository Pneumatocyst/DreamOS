#pragma once
#include "drivers/ata.h"

/* dreamfs: saves the whole ramfs tree to a disk and loads it back at boot.
 *
 * On-disk layout (one sector = 512 bytes):
 *   sector 0        superblock: magic "DREAMFS1", where things are, a checksum
 *   sectors 1-28    node table: every file and directory (name, parent, size, time)
 *   sectors 29+     file contents, each file starting on a fresh sector
 */

enum { DFS_OK = 0, DFS_BLANK = -9, DFS_FOREIGN = -10, DFS_CORRUPT = -11, DFS_FULL = -12,
       DFS_NODISK = -13 };

int  diskfs_load(struct disk *d);        /* >= 0: number of files restored */
int  diskfs_save(struct disk *d);        /* >= 0: number of files saved */
void diskfs_mount(struct disk *d);       /* remember which disk to save to */
struct disk *diskfs_mounted(void);
int  diskfs_autosave(void);              /* save if anything changed; 0 if nothing to do */
const char *diskfs_strerror(int err);
uint64_t diskfs_saves(void);             /* how many times this disk has been saved to */
