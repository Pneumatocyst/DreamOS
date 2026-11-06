#pragma once
#include "kernel.h"
#include "drivers/rtc.h"

/* ramfs: the file tree, kept in memory. File contents live in kmalloc'd buffers
   that grow as you write. fs/diskfs.c saves it all to disk.
   Nodes are referred to by their index in the table; 0 is the root "/". */

#define FS_NAME_MAX  32
#define FS_MAX_NODES 1024                /* files + directories */
#define FS_FILE_MAX  (1024 * 1024)       /* 1 MiB per file */

enum {
    FS_ENOENT = -1, FS_EEXIST = -2, FS_ENOTDIR = -3, FS_EISDIR = -4,
    FS_ENOSPC = -5, FS_ENOTEMPTY = -6, FS_EINVAL = -7, FS_EFBIG = -8,
};

struct fs_node {
    bool used, dir;
    char name[FS_NAME_MAX];
    int parent;
    char *data;               /* file contents (kmalloc'd); NULL until first written */
    uint32_t size, cap;       /* bytes in use / bytes allocated */
    struct rtc_time mtime;
};

void fs_init(void);
int  fs_home(void);

/* Paths may be absolute (/etc), relative (notes.txt), or start with ~ (home). */
int  fs_lookup(int cwd, const char *path);
int  fs_parent_of(int cwd, const char *path, char *name_out);
int  fs_create(int cwd, const char *path, bool dir);
int  fs_create_at(int parent, const char *name, bool dir);
int  fs_remove(int node);
int  fs_write(int node, const char *data, size_t len, bool append);
int  fs_move(int node, int new_parent, const char *new_name);
void fs_touch(int node);

const struct fs_node *fs_get(int node);
const char *fs_data(int node);
int  fs_child(int dir, int prev);          /* iterate: start with prev = -1 */
bool fs_is_ancestor(int ancestor, int node);
void fs_path(int node, char *buf, size_t n);
const char *fs_strerror(int err);
void fs_usage(int *dirs, int *files, size_t *bytes);

/* Hold the lock yourself to do several steps as one (it's fine to call fs_* inside). */
void fs_lock(void);
void fs_unlock(void);
uint64_t fs_generation(void);              /* changes whenever anything in the tree does */
