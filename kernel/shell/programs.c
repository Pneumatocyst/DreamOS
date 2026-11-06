/* The user programs from user/ are compiled separately and baked into the kernel
   image (see the Makefile). At boot we copy them into /bin, so they're ordinary
   files: you can ls them, hexdump them, and run them. */
#include "shell/shell.h"
#include "fs/ramfs.h"

struct userbin { const char *name; const uint8_t *start, *end; };
extern const struct userbin userbins[];

int sh_install_programs(void) {
    int bin = fs_lookup(0, "/bin");
    if (bin == FS_ENOENT) bin = fs_create(0, "/bin", true);
    if (bin < 0 || !fs_get(bin)->dir) return 0;
    int count = 0;
    for (const struct userbin *u = userbins; u->name; u++) {
        size_t size = u->end - u->start;
        int f = fs_lookup(bin, u->name);
        if (f == FS_ENOENT) f = fs_create_at(bin, u->name, false);
        if (f < 0 || fs_get(f)->dir) continue;
        /* only rewrite it if it changed, so the disk isn't touched on every boot */
        if (fs_get(f)->size != size || memcmp(fs_data(f), u->start, size)) fs_write(f, (const char *)u->start, size, false);
        count++;
    }
    return count;
}
