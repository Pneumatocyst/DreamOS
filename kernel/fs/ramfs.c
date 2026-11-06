#include "fs/ramfs.h"
#include "fs/ramfs_internal.h"
#include "mm/heap.h"
#include "sched/task.h"

struct fs_node fs_nodes[FS_MAX_NODES];
bool fs_dirty;          /* something changed since the last save to disk */
static volatile uint64_t fs_gen;   /* counts every change, so viewers know to refresh */
static int home;

/* ---- helpers ------------------------------------------------------------------ */

static int find_child(int dir, const char *name) {
    for (int i = 1; i < FS_MAX_NODES; i++)
        if (fs_nodes[i].used && fs_nodes[i].parent == dir && !strcmp(fs_nodes[i].name, name)) return i;
    return FS_ENOENT;
}

/* Copy the next path component into out. 1 = got one, 0 = end of path. */
static int next_comp(const char **p, char *out) {
    while (**p == '/') (*p)++;
    if (!**p) return 0;
    int n = 0;
    while (**p && **p != '/') {
        if (n >= FS_NAME_MAX - 1) return FS_EINVAL;
        out[n++] = *(*p)++;
    }
    out[n] = 0;
    return 1;
}

/* ---- lookup ------------------------------------------------------------------- */

static int do_fs_lookup(int cwd, const char *path) {
    int cur = cwd;
    if (path[0] == '/') cur = 0;
    else if (path[0] == '~' && (path[1] == 0 || path[1] == '/')) { cur = home; path++; }

    char comp[FS_NAME_MAX];
    int r;
    while ((r = next_comp(&path, comp)) > 0) {
        if (!fs_nodes[cur].dir) return FS_ENOTDIR;
        if (!strcmp(comp, ".")) continue;
        if (!strcmp(comp, "..")) { cur = fs_nodes[cur].parent; continue; }
        cur = find_child(cur, comp);
        if (cur < 0) return cur;
    }
    return r < 0 ? r : cur;
}

/* Split "a/b/c" into the node for "a/b" (returned) and the name "c". */
static int do_fs_parent_of(int cwd, const char *path, char *name_out) {
    char buf[256];
    if (strlen(path) >= sizeof buf) return FS_EINVAL;
    strlcpy(buf, path, sizeof buf);
    size_t n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/') buf[--n] = 0;

    char *slash = strrchr(buf, '/');
    const char *dir, *base;
    if (!slash)            { dir = ".";  base = buf; }
    else if (slash == buf) { dir = "/";  base = buf + 1; }
    else                   { *slash = 0; dir = buf; base = slash + 1; }

    if (!*base || !strcmp(base, ".") || !strcmp(base, "..") || !strcmp(base, "~")) return FS_EINVAL;
    if (strlen(base) >= FS_NAME_MAX) return FS_EINVAL;
    strlcpy(name_out, base, FS_NAME_MAX);

    int p = fs_lookup(cwd, dir);
    if (p < 0) return p;
    return fs_nodes[p].dir ? p : FS_ENOTDIR;
}

/* ---- changing things ---------------------------------------------------------- */

static int do_fs_create_at(int parent, const char *name, bool dir) {
    if (!fs_nodes[parent].dir) return FS_ENOTDIR;
    if (find_child(parent, name) >= 0) return FS_EEXIST;

    int n = 1;
    while (n < FS_MAX_NODES && fs_nodes[n].used) n++;
    if (n == FS_MAX_NODES) return FS_ENOSPC;

    fs_nodes[n] = (struct fs_node){ .used = true, .dir = dir, .parent = parent };
    strlcpy(fs_nodes[n].name, name, FS_NAME_MAX);
    rtc_now(&fs_nodes[n].mtime);
    fs_dirty = true;
    fs_gen++;
    return n;
}

static int do_fs_create(int cwd, const char *path, bool dir) {
    char name[FS_NAME_MAX];
    int p = fs_parent_of(cwd, path, name);
    return p < 0 ? p : fs_create_at(p, name, dir);
}

static int do_fs_remove(int n) {
    if (n <= 0) return FS_EINVAL;
    if (fs_nodes[n].dir && fs_child(n, -1) >= 0) return FS_ENOTEMPTY;
    kfree(fs_nodes[n].data);
    memset(&fs_nodes[n], 0, sizeof fs_nodes[n]);
    fs_dirty = true;
    fs_gen++;
    return 0;
}

static int do_fs_write(int n, const char *data, size_t len, bool append) {
    if (fs_nodes[n].dir) return FS_EISDIR;
    struct fs_node *f = &fs_nodes[n];
    size_t off = append ? f->size : 0;
    int err = 0;
    if (off + len > FS_FILE_MAX) { len = FS_FILE_MAX - off; err = FS_EFBIG; }

    size_t need = off + len;
    if (need > f->cap) {
        /* grow by doubling, so appending byte by byte doesn't copy forever.
           Round up to whole sectors: the disk layer writes 512 bytes at a time. */
        size_t cap = f->cap * 2 > need ? f->cap * 2 : need;
        cap = (cap + 511) & ~(size_t)511;
        if (cap > FS_FILE_MAX) cap = FS_FILE_MAX;
        char *p = krealloc(f->data, cap);
        if (!p) return FS_ENOSPC;
        f->data = p;
        f->cap = cap;
    }
    if (len) memmove(f->data + off, data, len);
    f->size = need;
    rtc_now(&fs_nodes[n].mtime);
    fs_dirty = true;
    fs_gen++;
    return err;
}

static int do_fs_move(int n, int new_parent, const char *new_name) {
    if (n <= 0) return FS_EINVAL;
    if (!fs_nodes[new_parent].dir) return FS_ENOTDIR;
    if (fs_is_ancestor(n, new_parent)) return FS_EINVAL;   /* can't move a dir inside itself */
    int existing = find_child(new_parent, new_name);
    if (existing >= 0 && existing != n) return FS_EEXIST;
    fs_nodes[n].parent = new_parent;
    strlcpy(fs_nodes[n].name, new_name, FS_NAME_MAX);
    fs_dirty = true;
    fs_gen++;
    return 0;
}

static void do_fs_touch(int n) { rtc_now(&fs_nodes[n].mtime); fs_dirty = true;
    fs_gen++; }

/* After the on-disk layer replaces the tables, find home again. */
void fs_rehome(void) {
    home = fs_lookup(0, "/root");
    if (home < 0) home = fs_create(0, "/root", true);
    if (home < 0) home = 0;
}

/* ---- reading things ----------------------------------------------------------- */

const struct fs_node *fs_get(int n) { return &fs_nodes[n]; }
const char *fs_data(int n) { return fs_nodes[n].data ? fs_nodes[n].data : ""; }
int fs_home(void) { return home; }

static int do_fs_child(int dir, int prev) {
    for (int i = prev + 1; i < FS_MAX_NODES; i++)
        if (i && fs_nodes[i].used && fs_nodes[i].parent == dir) return i;
    return -1;
}

bool fs_is_ancestor(int a, int n) {
    for (;;) {
        if (n == a) return true;
        if (n == 0) return false;
        n = fs_nodes[n].parent;
    }
}

static void do_fs_path(int n, char *buf, size_t size) {
    if (n == 0) { strlcpy(buf, "/", size); return; }
    int chain[64], depth = 0;
    for (; n && depth < 64; n = fs_nodes[n].parent) chain[depth++] = n;
    buf[0] = 0;
    size_t len = 0;
    while (depth--) {
        len += snprintf(buf + len, len < size ? size - len : 0, "/%s", fs_nodes[chain[depth]].name);
    }
}

const char *fs_strerror(int err) {
    switch (err) {
    case FS_ENOENT:    return "no such file or directory";
    case FS_EEXIST:    return "already exists";
    case FS_ENOTDIR:   return "not a directory";
    case FS_EISDIR:    return "is a directory";
    case FS_ENOSPC:    return "out of space (too many files, or out of memory)";
    case FS_ENOTEMPTY: return "directory not empty";
    case FS_EINVAL:    return "invalid path";
    case FS_EFBIG:     return "file too large (1 MiB max), truncated";
    default:           return "unknown error";
    }
}

static void do_fs_usage(int *dirs, int *files, size_t *bytes) {
    *dirs = *files = 0;
    *bytes = 0;
    for (int i = 0; i < FS_MAX_NODES; i++) {
        if (!fs_nodes[i].used) continue;
        if (fs_nodes[i].dir) (*dirs)++;
        else { (*files)++; *bytes += fs_nodes[i].size; }
    }
}

/* ---- the starting tree -------------------------------------------------------- */

static void seed(const char *path, const char *text) {
    int n = fs_create(0, path, false);
    if (n >= 0) fs_write(n, text, strlen(text), false);
}

void fs_clear(void) {
    for (int i = 0; i < FS_MAX_NODES; i++) kfree(fs_nodes[i].data);
    memset(fs_nodes, 0, sizeof fs_nodes);
}

static void do_fs_init(void) {
    fs_clear();
    fs_nodes[0] = (struct fs_node){ .used = true, .dir = true, .name = "/", .parent = 0 };
    rtc_now(&fs_nodes[0].mtime);

    fs_create(0, "/etc", true);
    fs_create(0, "/root", true);
    fs_create(0, "/tmp", true);
    home = fs_lookup(0, "/root");

    seed("/etc/hostname", "dream\n");
    seed("/etc/os-release",
         "NAME=\"dream\"\n"
         "VERSION=\"" DREAM_VERSION "\"\n"
         "PRETTY_NAME=\"dream " DREAM_VERSION "\"\n"
         "HOME_URL=\"file:///home/lynwood/Projects/dream\"\n");
    seed("/etc/motd", "files are kept in memory and saved to disk after every change.\n");
    seed("/root/readme.txt",
         "welcome to dream.\n"
         "\n"
         "your files are saved to disk, so they're still here after a reboot.\n"
         "\n"
         "things to try:\n"
         "  ls -l /etc\n"
         "  cat /etc/os-release\n"
         "  echo \"remember the milk\" > notes.txt\n"
         "  echo \"and the eggs\" >> notes.txt\n"
         "  cat notes.txt | wc\n"
         "  mkdir -p projects/os && cd projects/os\n"
         "  tree /\n"
         "  dmesg | grep PIT\n"
         "  history | tail -n 3\n"
         "  pwd | copy        (then ctrl+v pastes it)\n"
         "  mandel 20 &       (runs in the background)\n");
}

/* ---- locking ------------------------------------------------------------------------
 * The shell, the desktop (file browser) and user programs all use the filesystem,
 * from different tasks. Each public function below holds the lock while it works,
 * so nobody sees the tree half-changed. */

static struct mutex fs_mutex;
void fs_lock(void)   { mutex_lock(&fs_mutex); }
void fs_unlock(void) { mutex_unlock(&fs_mutex); }
uint64_t fs_generation(void) { return fs_gen; }

int fs_lookup(int cwd, const char *path) { fs_lock(); int r = do_fs_lookup(cwd, path); fs_unlock(); return r; }
int fs_parent_of(int cwd, const char *path, char *name_out) { fs_lock(); int r = do_fs_parent_of(cwd, path, name_out); fs_unlock(); return r; }
int fs_create_at(int parent, const char *name, bool dir) { fs_lock(); int r = do_fs_create_at(parent, name, dir); fs_unlock(); return r; }
int fs_create(int cwd, const char *path, bool dir) { fs_lock(); int r = do_fs_create(cwd, path, dir); fs_unlock(); return r; }
int fs_remove(int n) { fs_lock(); int r = do_fs_remove(n); fs_unlock(); return r; }
int fs_write(int n, const char *data, size_t len, bool append) { fs_lock(); int r = do_fs_write(n, data, len, append); fs_unlock(); return r; }
int fs_move(int n, int new_parent, const char *new_name) { fs_lock(); int r = do_fs_move(n, new_parent, new_name); fs_unlock(); return r; }
void fs_touch(int n) { fs_lock(); do_fs_touch(n); fs_unlock(); }
int fs_child(int dir, int prev) { fs_lock(); int r = do_fs_child(dir, prev); fs_unlock(); return r; }
void fs_path(int n, char *buf, size_t size) { fs_lock(); do_fs_path(n, buf, size); fs_unlock(); }
void fs_usage(int *dirs, int *files, size_t *bytes) { fs_lock(); do_fs_usage(dirs, files, bytes); fs_unlock(); }
void fs_init(void) { fs_lock(); do_fs_init(); fs_unlock(); }
