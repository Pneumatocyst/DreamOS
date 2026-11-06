#pragma once
/* ramfs's tables, shared only with the on-disk layer (fs/diskfs.c),
   which copies them to and from the disk. Commands should use ramfs.h. */
#include "fs/ramfs.h"

extern struct fs_node fs_nodes[FS_MAX_NODES];
extern bool fs_dirty;

void fs_clear(void);            /* free everything: an empty table, not even a root */
void fs_rehome(void);
