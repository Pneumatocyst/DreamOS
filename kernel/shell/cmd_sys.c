/* System commands: who, what, when, and how much. */
#include "shell/shell.h"
#include "arch/cpu.h"
#include "arch/io.h"
#include "drivers/keyboard.h"
#include "drivers/rtc.h"
#include "fs/ramfs.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "sched/task.h"

SH_CMD(whoami) { (void)argc; (void)argv; kprintf("root\n"); }

SH_CMD(uname) {
    bool all = false, any = false, s = false, n = false, r = false, m = false;
    for (int i = 1; i < argc; i++) {
        for (const char *f = argv[i] + (argv[i][0] == '-'); *f; f++) {
            switch (*f) {
            case 'a': all = true; break;
            case 's': s = true; break;
            case 'n': n = true; break;
            case 'r': r = true; break;
            case 'm': m = true; break;
            default: kerrorf(C_ERR "uname" C_DIM ": unknown option -%c\n", *f); return;
            }
            any = true;
        }
    }
    if (!any) s = true;
    if (all) s = n = r = m = true;
    const char *parts[4] = { s ? "dream" : NULL, n ? sh_hostname() : NULL,
                             r ? DREAM_VERSION : NULL, m ? "x86_64" : NULL };
    bool first = true;
    for (int i = 0; i < 4; i++) {
        if (!parts[i]) continue;
        kprintf("%s%s", first ? "" : " ", parts[i]);
        first = false;
    }
    kprintf("\n");
}

SH_CMD(hostname) {
    if (argc < 2) { kprintf("%s\n", sh_hostname()); return; }
    if (strlen(argv[1]) > 30) { kerrorf(C_ERR "hostname" C_DIM ": too long (30 max)\n"); return; }
    int f = fs_lookup(0, "/etc/hostname");
    if (f == FS_ENOENT) f = fs_create(0, "/etc/hostname", false);
    char buf[34];
    int len = snprintf(buf, sizeof buf, "%s\n", argv[1]);
    if (f >= 0) f = fs_write(f, buf, len, false);
    if (f < 0) sh_perror("hostname", "/etc/hostname", f);
}

SH_CMD(date) {
    (void)argc; (void)argv;
    struct rtc_time t;
    rtc_now(&t);
    kprintf("%s %s %2u %02u:%02u:%02u %u\n", rtc_day_names[rtc_weekday(&t)],
            rtc_month_names[(t.mon + 11) % 12], t.day, t.hour, t.min, t.sec, t.year);
}

SH_CMD(free) {
    (void)argc; (void)argv;
    uint64_t total = pmm_total_pages() * PAGE_SIZE, used = pmm_used_pages() * PAGE_SIZE;
    kprintf(C_DIM "            %10s %10s %10s\n", "total", "used", "free");
    kprintf(C_ACCENT "memory   " C_FG "%9lu M %9lu K %9lu M" C_DIM "   in 4 KiB pages\n",
            total >> 20, used >> 10, (total - used) >> 20);

    struct heap_stats h;
    heap_stats(&h);
    kprintf(C_ACCENT "kmalloc  " C_FG "%10s %9lu K %10s" C_DIM "   %lu live allocations, %lu ever\n",
            "", h.live_bytes >> 10, "", h.live_allocs, h.total_allocs);

    int dirs, files;
    size_t bytes;
    fs_usage(&dirs, &files, &bytes);
    kprintf(C_ACCENT "files    " C_FG "%10s %9lu K %10s" C_DIM "   %d files, %d dirs (of %d)\n",
            "", (uint64_t)bytes >> 10, "", files, dirs, FS_MAX_NODES);
}

/* Hammer the allocator: lots of allocations of mixed sizes, each filled with a
   pattern, checked, and freed. If the heap hands out overlapping memory, the
   patterns get mixed up and we catch it. Afterwards, everything should be back. */
SH_CMD(memtest) {
    (void)argc; (void)argv;
    enum { N = 200 };
    static uint8_t *ptrs[N];
    static size_t sizes[N];
    uint64_t pages_before = pmm_used_pages();
    struct heap_stats before;
    heap_stats(&before);
    uint32_t seed = (uint32_t)pit_ticks() | 1;
    uint64_t total = 0;

    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < N; i++) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;   /* xorshift */
            sizes[i] = (i % 10 == 0) ? 4096 + seed % 200000 : 1 + seed % 3000;
            ptrs[i] = kmalloc(sizes[i]);
            if (!ptrs[i]) { kerrorf(C_ERR "memtest" C_DIM ": kmalloc(%lu) failed\n", (uint64_t)sizes[i]); return; }
            memset(ptrs[i], (uint8_t)(i + round), sizes[i]);
            total += sizes[i];
        }
        for (int i = 0; i < N; i++) {
            for (size_t k = 0; k < sizes[i]; k++) {
                if (ptrs[i][k] != (uint8_t)(i + round)) {
                    kerrorf(C_ERR "memtest" C_DIM ": block %d was overwritten: allocations overlap!\n", i);
                    return;
                }
            }
        }
        for (int i = 0; i < N; i += 2) kfree(ptrs[i]);       /* free in a scattered order */
        for (int i = 1; i < N; i += 2) kfree(ptrs[i]);
        kprintf(C_DIM "  round %d: " C_FG "%d allocations" C_DIM ", every byte checked\n", round + 1, N);
    }

    struct heap_stats after;
    heap_stats(&after);
    int64_t kept = (int64_t)pmm_used_pages() - (int64_t)pages_before;
    kprintf(C_OK "passed" C_FG ": %lu KiB allocated and freed" C_DIM ", live allocations %lu -> %lu\n",
            total >> 10, before.live_allocs, after.live_allocs);
    kprintf(C_DIM "  %ld pages kept by the small-block caches for reuse. run memtest again:"
            " this number should stay near 0.\n", kept);
}

SH_CMD(dmesg) {
    (void)argc; (void)argv;
    size_t len;
    const char *log = klog_buf(&len);
    kwrite(log, len);
}

SH_CMD(ps) {
    (void)argc; (void)argv;
    static const char *states[] = { "ready", "running", "sleeping", "done" };
    struct task_info t[32];
    int n = task_list(t, 32);
    uint64_t total = sched_total_ticks(), hz = pit_hz();
    kprintf(C_DIM "  %4s  %-12s %-10s %10s %6s\n", "ID", "NAME", "STATE", "CPU TIME", "CPU");
    for (int i = 0; i < n; i++) {
        uint64_t pct10 = total ? t[i].ticks * 1000 / total : 0;
        kprintf("  " C_ACCENT "%4d  " C_FG "%-12s %s%-10s " C_FG "%7lu.%02lus %4lu.%lu%%\n",
                t[i].id, t[i].name, t[i].state == TASK_RUNNING ? C_OK : C_DIM, states[t[i].state],
                t[i].ticks / hz, (t[i].ticks % hz) * 100 / hz, pct10 / 10, pct10 % 10);
    }
    kprintf(C_DIM "  %d tasks. the scheduler switches between them every 10 ms.\n", n);
}

SH_CMD(history) { (void)argc; (void)argv; sh_print_history(); }

SH_CMD(sleep) {
    uint64_t secs;
    if (argc < 2 || !sh_parse_num(argv[1], &secs)) { kerrorf("usage: sleep <seconds>\n"); return; }
    uint64_t end = pit_ticks() + secs * pit_hz();
    while (pit_ticks() < end) {
        if (sh_poll_key() == 3) { kerrorf(C_DIM "^C\n"); sh_interrupt(); return; }   /* ctrl+c (keeps the desktop alive too) */
        task_sleep(10);   /* doze: the desktop and others keep running */
    }
}

static void cpuid(uint32_t leaf, uint32_t r[4]) {
    asm volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0));
}

SH_CMD(lscpu) {
    (void)argc; (void)argv;
    uint32_t r[4];
    char vendor[13];
    cpuid(0, r);
    memcpy(vendor, &r[1], 4); memcpy(vendor + 4, &r[3], 4); memcpy(vendor + 8, &r[2], 4);
    vendor[12] = 0;

    char brand[49] = "unknown";
    cpuid(0x80000000, r);
    if (r[0] >= 0x80000004) {
        for (uint32_t i = 0; i < 3; i++) { cpuid(0x80000002 + i, r); memcpy(brand + i * 16, r, 16); }
        brand[48] = 0;
    }
    const char *b = brand;
    while (*b == ' ') b++;

    cpuid(1, r);
    uint32_t family = (r[0] >> 8) & 0xf, model = (r[0] >> 4) & 0xf;
    if (family == 0xf) family += (r[0] >> 20) & 0xff;
    if (family >= 6) model |= ((r[0] >> 16) & 0xf) << 4;

    kprintf(C_ACCENT "model     " C_FG "%s\n", b);
    kprintf(C_ACCENT "vendor    " C_FG "%s\n", vendor);
    kprintf(C_ACCENT "family    " C_FG "%u" C_DIM "  model " C_FG "%u\n", family, model);
    kprintf(C_ACCENT "features  " C_FG);
    static const struct { int reg, bit; const char *name; } feats[] = {
        { 3, 25, "sse" }, { 3, 26, "sse2" }, { 2, 0, "sse3" }, { 2, 19, "sse4.1" },
        { 2, 20, "sse4.2" }, { 2, 28, "avx" }, { 2, 30, "rdrand" }, { 3, 9, "apic" },
    };
    for (unsigned i = 0; i < sizeof feats / sizeof feats[0]; i++)
        if (r[feats[i].reg] & (1u << feats[i].bit)) kprintf("%s ", feats[i].name);
    kprintf("\n");
    if (r[2] & (1u << 31)) kprintf(C_ACCENT "virtual   " C_FG "yes" C_DIM "  (a hypervisor is running us)\n");
}
