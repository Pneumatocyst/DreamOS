/* File and text commands: the everyday Linux toolbox, on top of ramfs. */
#include "shell/shell.h"
#include "fs/ramfs.h"
#include "drivers/console.h"

/* ---- small helpers ------------------------------------------------------------- */

/* Collect a directory's children, sorted by name. */
static int sorted_children(int dir, int *out, bool all) {
    int n = 0;
    for (int c = fs_child(dir, -1); c >= 0; c = fs_child(dir, c)) {
        if (!all && fs_get(c)->name[0] == '.') continue;
        int i = n++;
        while (i > 0 && strcmp(fs_get(out[i - 1])->name, fs_get(c)->name) > 0) { out[i] = out[i - 1]; i--; }
        out[i] = c;
    }
    return n;
}

static void print_name(int node) {
    const struct fs_node *f = fs_get(node);
    if (!sh_is_tty())  kprintf("%s", f->name);
    else if (f->dir)   kprintf(C_ACCENT "%s" C_DIM "/", f->name);
    else               kprintf(C_FG "%s", f->name);
}

/* Walk text one line at a time. */
static bool next_line(const char **p, const char *end, const char **line, size_t *len) {
    if (*p >= end) return false;
    *line = *p;
    while (*p < end && **p != '\n') (*p)++;
    *len = *p - *line;
    if (*p < end) (*p)++;      /* step over the newline */
    return true;
}

/* Pull "-n 5" out of argv; returns the index of the first non-option argument. */
static int opt_lines(int argc, char **argv, uint64_t *n) {
    *n = 10;
    int i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-n")) {
        if (!sh_parse_num(argv[i + 1], n)) kerrorf(C_ERR "%s" C_DIM ": -n wants a number\n", argv[0]);
        i += 2;
    }
    return i;
}

/* Parse flags like -la into a set of letters. Returns index of the first non-flag. */
static int flags(int argc, char **argv, const char *allowed, bool *set /* [26] */) {
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f < 'a' || *f > 'z' || !strchr(allowed, *f)) {
                kerrorf(C_ERR "%s" C_DIM ": unknown option -%c\n", argv[0], *f);
                return -1;
            }
            set[*f - 'a'] = true;
        }
    }
    return i;
}

/* ---- navigating ------------------------------------------------------------------ */

SH_CMD(pwd) {
    (void)argc; (void)argv;
    char path[256];
    fs_path(sh_cwd(), path, sizeof path);
    kprintf("%s\n", path);
}

SH_CMD(cd) {
    const char *target = argc > 1 ? argv[1] : "~";
    int n = fs_lookup(sh_cwd(), target);
    if (n < 0) { sh_perror("cd", target, n); return; }
    if (!fs_get(n)->dir) { sh_perror("cd", target, FS_ENOTDIR); return; }
    sh_set_cwd(n);
}

static void ls_long(int n) {
    const struct fs_node *f = fs_get(n);
    const struct rtc_time *t = &f->mtime;
    if (f->dir) kprintf(C_ACCENT "d" C_DIM "  %6s", "-");
    else        kprintf(C_DIM "-  " C_FG "%6u", f->size);
    kprintf(C_DIM "  %s %2u %02u:%02u  ", rtc_month_names[(t->mon + 11) % 12], t->day, t->hour, t->min);
    print_name(n);
    kprintf("\n");
}

SH_CMD(ls) {
    bool f[26] = { 0 };
    int i = flags(argc, argv, "la", f);
    if (i < 0) return;
    bool lng = f['l' - 'a'], all = f['a' - 'a'];
    const char *path = i < argc ? argv[i] : ".";

    int n = fs_lookup(sh_cwd(), path);
    if (n < 0) { sh_perror("ls", path, n); return; }
    if (!fs_get(n)->dir) {                       /* ls on a single file */
        if (lng) ls_long(n); else { print_name(n); kprintf("\n"); }
        return;
    }

    static int kids[FS_MAX_NODES];
    int count = sorted_children(n, kids, all);
    if (lng || !sh_is_tty()) {                   /* one per line, like ls into a pipe */
        for (int k = 0; k < count; k++) {
            if (lng) ls_long(kids[k]);
            else { print_name(kids[k]); kprintf("\n"); }
        }
        return;
    }
    int col = 0, width = con_cols();
    for (int k = 0; k < count; k++) {
        const struct fs_node *c = fs_get(kids[k]);
        int w = strlen(c->name) + (c->dir ? 1 : 0) + 2;
        if (col && col + w > width) { kprintf("\n"); col = 0; }
        print_name(kids[k]);
        kprintf("  ");
        col += w;
    }
    if (count) kprintf("\n");
}

static void tree_walk(int dir, char *prefix, int depth, int *nd, int *nf) {
    static int kids[16][FS_MAX_NODES];         /* one list per depth level */
    if (depth >= 16) return;
    int count = sorted_children(dir, kids[depth], true);
    for (int k = 0; k < count; k++) {
        int c = kids[depth][k];
        bool last = k == count - 1;
        kprintf(C_DIM "%s%s", prefix, last ? "`-- " : "|-- ");
        print_name(c);
        kprintf("\n");
        if (fs_get(c)->dir) {
            (*nd)++;
            size_t len = strlen(prefix);
            strlcpy(prefix + len, last ? "    " : "|   ", 5);
            tree_walk(c, prefix, depth + 1, nd, nf);
            prefix[len] = 0;
        } else (*nf)++;
    }
}

SH_CMD(tree) {
    const char *path = argc > 1 ? argv[1] : ".";
    int n = fs_lookup(sh_cwd(), path);
    if (n < 0) { sh_perror("tree", path, n); return; }
    char prefix[16 * 4 + 1] = "";
    int nd = 0, nf = 0;
    kprintf(C_ACCENT "%s\n", path);
    tree_walk(n, prefix, 0, &nd, &nf);
    kprintf(C_DIM "\n%d directories, %d files\n", nd, nf);
}

/* ---- making and removing --------------------------------------------------------- */

static int mkdir_p(const char *path) {
    char buf[256];
    if (strlcpy(buf, path, sizeof buf) >= sizeof buf) return FS_EINVAL;
    for (char *p = buf + 1;; p++) {
        if (*p != '/' && *p != 0) continue;
        char saved = *p;
        *p = 0;
        int n = fs_lookup(sh_cwd(), buf);
        if (n == FS_ENOENT) n = fs_create(sh_cwd(), buf, true);
        else if (n >= 0 && !fs_get(n)->dir) n = FS_ENOTDIR;
        if (n < 0) return n;
        *p = saved;
        if (!saved) return 0;
    }
}

SH_CMD(mkdir) {
    bool f[26] = { 0 };
    int i = flags(argc, argv, "p", f);
    if (i < 0) return;
    if (i >= argc) { kerrorf("usage: mkdir [-p] <dir>...\n"); return; }
    for (; i < argc; i++) {
        int r = f['p' - 'a'] ? mkdir_p(argv[i]) : fs_create(sh_cwd(), argv[i], true);
        if (r < 0) sh_perror("mkdir", argv[i], r);
    }
}

SH_CMD(touch) {
    if (argc < 2) { kerrorf("usage: touch <file>...\n"); return; }
    for (int i = 1; i < argc; i++) {
        int n = fs_lookup(sh_cwd(), argv[i]);
        if (n >= 0) fs_touch(n);
        else if (n == FS_ENOENT) n = fs_create(sh_cwd(), argv[i], false);
        if (n < 0) sh_perror("touch", argv[i], n);
    }
}

static int rm_tree(int n) {
    int c;
    while (fs_get(n)->dir && (c = fs_child(n, -1)) >= 0) {
        int r = rm_tree(c);
        if (r < 0) return r;
    }
    return fs_remove(n);
}

SH_CMD(rm) {
    bool f[26] = { 0 };
    int i = flags(argc, argv, "rf", f);
    if (i < 0) return;
    if (i >= argc) { kerrorf("usage: rm [-r] <path>...\n"); return; }
    for (; i < argc; i++) {
        int n = fs_lookup(sh_cwd(), argv[i]);
        if (n < 0) { if (!f['f' - 'a']) sh_perror("rm", argv[i], n); continue; }
        if (fs_get(n)->dir && !f['r' - 'a']) { sh_perror("rm", argv[i], FS_EISDIR); continue; }
        if (fs_is_ancestor(n, sh_cwd())) {
            kerrorf(C_ERR "rm" C_DIM ": %s: " C_FG "refusing to remove a directory you're inside\n", argv[i]);
            continue;
        }
        int r = rm_tree(n);
        if (r < 0) sh_perror("rm", argv[i], r);
    }
}

SH_CMD(rmdir) {
    if (argc < 2) { kerrorf("usage: rmdir <dir>...\n"); return; }
    for (int i = 1; i < argc; i++) {
        int n = fs_lookup(sh_cwd(), argv[i]);
        if (n >= 0 && !fs_get(n)->dir) n = FS_ENOTDIR;
        if (n >= 0 && fs_is_ancestor(n, sh_cwd())) n = FS_EINVAL;
        if (n >= 0) n = fs_remove(n);
        if (n < 0) sh_perror("rmdir", argv[i], n);
    }
}

/* Work out where `dst` points for mv/cp: into a directory, or a new/replaced name. */
static int resolve_dst(int src, const char *dst, char *name, int *existing) {
    *existing = -1;
    int d = fs_lookup(sh_cwd(), dst);
    if (d >= 0 && fs_get(d)->dir) {
        strlcpy(name, fs_get(src)->name, FS_NAME_MAX);
        int e = -1;
        for (int c = fs_child(d, -1); c >= 0; c = fs_child(d, c))
            if (!strcmp(fs_get(c)->name, name)) e = c;
        *existing = e;
        return d;
    }
    if (d >= 0) *existing = d;
    else if (d != FS_ENOENT) return d;
    return fs_parent_of(sh_cwd(), dst, name);
}

SH_CMD(mv) {
    if (argc != 3) { kerrorf("usage: mv <from> <to>\n"); return; }
    int s = fs_lookup(sh_cwd(), argv[1]);
    if (s < 0) { sh_perror("mv", argv[1], s); return; }
    char name[FS_NAME_MAX];
    int existing;
    int parent = resolve_dst(s, argv[2], name, &existing);
    if (parent < 0) { sh_perror("mv", argv[2], parent); return; }
    if (existing == s) return;
    if (existing >= 0) {
        if (fs_get(existing)->dir || fs_get(s)->dir) { sh_perror("mv", argv[2], FS_EEXIST); return; }
        fs_remove(existing);                   /* replacing a file, like Linux does */
    }
    int r = fs_move(s, parent, name);
    if (r < 0) sh_perror("mv", argv[2], r);
}

SH_CMD(cp) {
    if (argc != 3) { kerrorf("usage: cp <file> <to>\n"); return; }
    int s = fs_lookup(sh_cwd(), argv[1]);
    if (s < 0) { sh_perror("cp", argv[1], s); return; }
    if (fs_get(s)->dir) { kerrorf(C_ERR "cp" C_DIM ": %s: " C_FG "copying directories isn't supported yet\n", argv[1]); return; }
    char name[FS_NAME_MAX];
    int existing;
    int parent = resolve_dst(s, argv[2], name, &existing);
    if (parent < 0) { sh_perror("cp", argv[2], parent); return; }
    if (existing == s) { kerrorf(C_ERR "cp" C_DIM ": source and destination are the same file\n"); return; }
    int d = existing >= 0 ? existing : fs_create_at(parent, name, false);
    if (d >= 0 && fs_get(d)->dir) d = FS_EISDIR;
    if (d >= 0) d = fs_write(d, fs_data(s), fs_get(s)->size, false);
    if (d < 0) sh_perror("cp", argv[2], d);
}

/* ---- reading text ------------------------------------------------------------------ */

SH_CMD(cat) {
    int i = 1;
    do {
        size_t len;
        const char *d = sh_input(argc, argv, i, &len);
        if (!d) continue;
        kwrite(d, len);
        if (sh_is_tty() && len && d[len - 1] != '\n') kprintf("\n");
    } while (++i < argc);
}

SH_CMD(wc) {
    /* -l lines, -w words, -c bytes (any mix); none = all three */
    bool want[3] = { false, false, false }, any = false;
    int first = 1;
    while (first < argc && argv[first][0] == '-' && argv[first][1]) {
        for (const char *f = argv[first] + 1; *f; f++) {
            int i = *f == 'l' ? 0 : *f == 'w' ? 1 : *f == 'c' ? 2 : -1;
            if (i < 0) { kerrorf("usage: wc [-lwc] [file]\n"); return; }
            want[i] = any = true;
        }
        first++;
    }
    size_t len;
    const char *d = sh_input(argc, argv, first, &len);
    if (!d) return;
    uint64_t lines = 0, words = 0;
    bool in_word = false;
    for (size_t i = 0; i < len; i++) {
        if (d[i] == '\n') lines++;
        bool space = d[i] == ' ' || d[i] == '\n' || d[i] == '\t';
        if (!space && !in_word) words++;
        in_word = !space;
    }
    if (!any) { kprintf(C_FG "%7lu %7lu %7lu" C_DIM "  %s\n", lines, words, (uint64_t)len, argc > first ? argv[first] : ""); return; }
    uint64_t v[3] = { lines, words, len };
    bool sp = false;
    for (int i = 0; i < 3; i++) if (want[i]) { kprintf("%s%lu", sp ? " " : "", v[i]); sp = true; }
    kprintf(argc > first ? " %s\n" : "\n", argc > first ? argv[first] : "");
}

SH_CMD(head) {
    uint64_t want;
    int i = opt_lines(argc, argv, &want);
    size_t len;
    const char *d = sh_input(argc, argv, i, &len);
    if (!d) return;
    const char *p = d, *line;
    size_t ll;
    for (uint64_t k = 0; k < want && next_line(&p, d + len, &line, &ll); k++) {
        kwrite(line, ll);
        kprintf("\n");
    }
}

SH_CMD(tail) {
    uint64_t want;
    int i = opt_lines(argc, argv, &want);
    size_t len;
    const char *d = sh_input(argc, argv, i, &len);
    if (!d) return;
    const char *p = d, *line;
    size_t ll;
    uint64_t total = 0;
    while (next_line(&p, d + len, &line, &ll)) total++;
    p = d;
    for (uint64_t k = 0; next_line(&p, d + len, &line, &ll); k++) {
        if (k + want < total) continue;
        kwrite(line, ll);
        kprintf("\n");
    }
}

static const char *find(const char *hay, size_t hl, const char *needle, size_t nl) {
    for (size_t i = 0; nl <= hl && i + nl <= hl; i++)
        if (!memcmp(hay + i, needle, nl)) return hay + i;
    return NULL;
}

SH_CMD(grep) {
    bool f[26] = { 0 };
    int i = flags(argc, argv, "vc", f);
    if (i < 0) return;
    if (i >= argc) { kerrorf("usage: grep [-v] [-c] <pattern> [file]\n"); return; }
    const char *pat = argv[i];
    size_t pl = strlen(pat), len;
    const char *d = sh_input(argc, argv, i + 1, &len);
    if (!d) return;

    bool invert = f['v' - 'a'], count_only = f['c' - 'a'];
    uint64_t matches = 0;
    const char *p = d, *line;
    size_t ll;
    while (next_line(&p, d + len, &line, &ll)) {
        const char *hit = find(line, ll, pat, pl);
        if (!hit == !invert) continue;
        matches++;
        if (count_only) continue;
        if (hit && sh_is_tty() && pl) {          /* highlight every match */
            const char *at = line, *end = line + ll;
            while ((hit = find(at, end - at, pat, pl))) {
                kprintf(C_FG); kwrite(at, hit - at);
                kprintf(C_ACCENT); kwrite(hit, pl);
                at = hit + pl;
            }
            kprintf(C_FG); kwrite(at, end - at);
        } else kwrite(line, ll);
        kprintf("\n");
    }
    if (count_only) kprintf("%lu\n", matches);
}
