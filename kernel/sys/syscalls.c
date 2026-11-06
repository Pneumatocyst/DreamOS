/* System calls for the apps in /bin: files beyond read/write, information about
 * the system, tasks, settings, themes, starting programs. (The basics and the window
 * system are in proc/process.c and gui/uwin.c.)
 *
 * The same rules as everywhere: check every pointer, copy strings in byte by byte,
 * and never hand a program a kernel address. */
#include "gui/wm.h"
#include "proc/process.h"
#include "fs/ramfs.h"
#include "fs/diskfs.h"
#include "drivers/fb.h"
#include "drivers/rtc.h"
#include "drivers/theme.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/task.h"
#include "shell/shell.h"
#include "arch/cpu.h"
#include "arch/io.h"

#define BAD ((uint64_t)-1)

static bool out_ok(struct process *p, uint64_t ptr, uint64_t len) { return vmm_user_ok(p->pml4, ptr, len, true); }

/* ---- files -------------------------------------------------------------------------------- */

uint64_t sys_stat(struct process *p, uint64_t path, uint64_t out) {
    char name[256];
    if (!user_string(p, path, name, sizeof name) || !out_ok(p, out, sizeof(struct file_stat))) return BAD;
    fs_lock();
    int n = fs_lookup(sh_cwd(), name);
    if (n < 0) { fs_unlock(); return (uint64_t)(int64_t)n; }
    const struct fs_node *f = fs_get(n);
    struct file_stat st = { .size = f->size, .is_dir = f->dir, .year = f->mtime.year, .mon = f->mtime.mon,
                            .day = f->mtime.day, .hour = f->mtime.hour, .min = f->mtime.min, .sec = f->mtime.sec };
    if (f->dir) for (int c = fs_child(n, -1); c >= 0; c = fs_child(n, c)) st.children++;
    fs_unlock();
    memcpy((void *)out, &st, sizeof st);
    return 0;
}

static int remove_tree(int n) {
    int c;
    while (fs_get(n)->dir && (c = fs_child(n, -1)) >= 0) {
        int r = remove_tree(c);
        if (r < 0) return r;
    }
    return fs_remove(n);
}

uint64_t sys_fsop(struct process *p, uint64_t op, uint64_t path, uint64_t arg) {
    char name[256], other[FS_NAME_MAX];
    if (!user_string(p, path, name, sizeof name)) return BAD;
    if (op == FSOP_RENAME && !user_string(p, arg, other, sizeof other)) return BAD;
    int r;
    fs_lock();
    if (op == FSOP_MKDIR || op == FSOP_CREATE) {
        r = fs_create(sh_cwd(), name, op == FSOP_MKDIR);
    } else {
        int n = fs_lookup(sh_cwd(), name);
        if (n < 0) r = n;
        else if (n == 0) r = FS_EINVAL;                               /* not the root */
        else if (op == FSOP_REMOVE) r = fs_is_ancestor(n, sh_cwd()) ? E_BUSY : remove_tree(n);
        else if (op == FSOP_RENAME) {
            r = !other[0] || strchr(other, '/') || !strcmp(other, ".") || !strcmp(other, "..")
                ? FS_EINVAL : fs_move(n, fs_get(n)->parent, other);
        } else r = FS_EINVAL;
    }
    fs_unlock();
    if (r >= 0) { diskfs_autosave(); r = 0; }
    return (uint64_t)(int64_t)r;
}

/* ---- the system ------------------------------------------------------------------------------ */

uint64_t sys_sysinfo(struct process *p, uint64_t out) {
    if (!out_ok(p, out, sizeof(struct sysinfo))) return BAD;
    struct sysinfo si;
    memset(&si, 0, sizeof si);
    si.mem_used = pmm_used_pages() * PAGE_SIZE;
    si.mem_total = pmm_total_pages() * PAGE_SIZE;
    si.uptime_ms = pit_ticks() * 1000 / pit_hz();
    cpu_sample();
    si.cpu = cpu_usage();
    struct task_info ti[32];
    si.ntasks = task_list(ti, 32);
    si.total_ticks = sched_total_ticks();
    si.hz = (int32_t)pit_hz();
    si.screen_w = fb.width;
    si.screen_h = fb.height;
    si.fs_generation = fs_generation();
    size_t bytes;
    fs_usage(&si.dirs, &si.files, &bytes);
    si.fs_bytes = bytes;
    si.max_nodes = FS_MAX_NODES;
    struct disk *d = diskfs_mounted();
    if (d) {
        strlcpy(si.disk, d->name, sizeof si.disk);
        strlcpy(si.disk_model, d->model, sizeof si.disk_model);
        si.disk_mb = d->sectors / 2048;
        si.disk_saves = diskfs_saves();
    }
    strlcpy(si.theme, theme->name, sizeof si.theme);
    strlcpy(si.hostname, sh_hostname(), sizeof si.hostname);
    struct rtc_time t;
    rtc_now(&t);
    si.year = t.year; si.mon = t.mon; si.day = t.day;
    si.hour = t.hour; si.min = t.min; si.sec = t.sec;
    si.weekday = rtc_weekday(&t);
    memcpy((void *)out, &si, sizeof si);
    return 0;
}

uint64_t sys_tasks(struct process *p, uint64_t out, uint64_t max) {
    struct task_info ti[32];
    if (max > 32) max = 32;
    if (!out_ok(p, out, max * sizeof(struct task_entry))) return BAD;
    int n = task_list(ti, (int)max);
    struct task_entry *e = (struct task_entry *)out;
    for (int i = 0; i < n; i++) {
        struct task_entry te = { .id = ti[i].id, .state = ti[i].state, .ticks = ti[i].ticks, .user = ti[i].user };
        strlcpy(te.name, ti[i].name, sizeof te.name);
        memcpy(&e[i], &te, sizeof te);
    }
    return n;
}

uint64_t sys_config(struct process *p, uint64_t op, uint64_t key, uint64_t val) {
    char k[32], v[64];
    if (!user_string(p, key, k, sizeof k) || !k[0] || strchr(k, '=') || strchr(k, '\n')) return BAD;
    if (op == CONFIG_GET) {
        if (!out_ok(p, val, sizeof v)) return BAD;
        const char *got = config_get(k, NULL);
        if (!got) return BAD;
        strlcpy(v, got, sizeof v);
        memcpy((void *)val, v, sizeof v);
        return 0;
    }
    if (op == CONFIG_SET) {
        if (!user_string(p, val, v, sizeof v) || strchr(v, '\n')) return BAD;
        config_set(k, v);
        return 0;
    }
    return BAD;
}

static void fill_theme(struct theme_info *ti, const struct theme *t, bool current) {
    memset(ti, 0, sizeof *ti);
    strlcpy(ti->name, t->name, sizeof ti->name);
    ti->bg_top = t->bg_top;
    ti->bg_bottom = t->bg_bottom;
    ti->bar = t->bar;
    for (int i = 0; i < 6; i++) ti->pal[i] = t->pal[i];
    if (current) for (int i = 0; i <= UI_ON_ACCENT; i++) ti->ui[i] = ui(i);
}

uint64_t sys_theme(struct process *p, uint64_t op, uint64_t arg, uint64_t out) {
    char name[32];
    switch (op) {
    case THEME_GET: {
        if (!out_ok(p, out, sizeof(struct theme_info))) return BAD;
        int i = (int)(int64_t)arg;
        if (i >= theme_count) return BAD;
        struct theme_info ti;
        fill_theme(&ti, i < 0 ? theme : &themes[i], i < 0 || &themes[i] == theme);
        memcpy((void *)out, &ti, sizeof ti);
        return theme_count;
    }
    case THEME_SET:
        if (!user_string(p, arg, name, sizeof name) || theme_set(name) < 0) return BAD;
        config_set("theme", name);
        return 0;
    case WALLPAPER_SET:
        if (!user_string(p, arg, name, sizeof name)) return BAD;
        for (int i = 0; i < wallpaper_count; i++) {
            if (strcmp(name, wallpaper_names[i])) continue;
            config_set("wallpaper", name);
            wm_wallpaper_changed();
            return 0;
        }
        return BAD;
    case WALLPAPER_NAME:
        if (arg >= (uint64_t)wallpaper_count || !out_ok(p, out, 16)) return BAD;
        strlcpy(name, wallpaper_names[arg], 16);
        memcpy((void *)out, name, 16);
        return wallpaper_count;
    }
    return BAD;
}

uint64_t sys_passwd(struct process *p, uint64_t op, uint64_t pw) {
    char s[32];
    if (op == PW_ISSET) return passwd_set();
    if (op == PW_CLEAR) { passwd_change(""); return 0; }
    if (op == PW_SET && user_string(p, pw, s, sizeof s) && s[0]) { passwd_change(s); return 0; }
    return BAD;
}

uint64_t sys_spawn(struct process *p, uint64_t prog, uint64_t arg) {
    char name[64], a[200];
    if (!user_string(p, prog, name, sizeof name)) return BAD;
    if (arg && !user_string(p, arg, a, sizeof a)) return BAD;
    char *argv[3] = { name, arg ? a : NULL, NULL };
    struct process *np = program_start(name, arg ? 2 : 1, argv);
    return np && np->task ? (uint64_t)np->task->id : BAD;
}

static void show_terminal(void *arg) { (void)arg; term_open(); }

uint64_t sys_terminal(struct process *p, uint64_t cmd) {
    char line[256];
    if (!user_string(p, cmd, line, sizeof line)) return BAD;
    if (!sh_at_prompt()) return BAD;               /* something's running there already */
    term_inject(line);
    wm_call(show_terminal, NULL);
    return 0;
}
