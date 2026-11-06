/* The shell: read a line, split it into words, run the matching command.
   Add your own by writing a function and adding a row to `commands`.

   It also understands a little shell syntax:
     cmd1 | cmd2      pipe: cmd1's output becomes cmd2's input
     cmd > file       write output to a file    (>> appends)
     cmd1 ; cmd2      run one after the other
     cmd1 && cmd2     run cmd2 only if cmd1 didn't print an error
     "quoted words"   keep spaces inside one argument */
#include "shell/shell.h"
#include "arch/cpu.h"
#include "arch/io.h"
#include "drivers/console.h"
#include "drivers/fb.h"
#include "drivers/keyboard.h"
#include "drivers/theme.h"
#include "fs/ramfs.h"
#include "fs/diskfs.h"
#include "mm/heap.h"
#include "gui/wm.h"
#include "sched/task.h"
#include "proc/process.h"

#define LINE_MAX 256
#define HIST     32
#define TOK_MAX  48

static char history[HIST][LINE_MAX];
static int hist_len;
static int cwd;

/* ---- reading a line --------------------------------------------------------- */

/* Sleep until a key arrives; meanwhile blink the cursor and keep the clock fresh. */
/* Where keys come from: the keyboard in text mode, the terminal window on the
   desktop (the desktop task reads the keyboard and passes keys along). */
int sh_poll_key(void) {
    if (wm_active()) return term_key();
    con_flush();
    return kbd_poll();
}

int sh_wait_key(void) {
    uint64_t last_sec = -1, last_blink = -1;
    bool on = false;
    for (;;) {
        int k = sh_poll_key();
        if (k >= 0) { if (on) con_cursor(false); return k; }
        uint64_t t = pit_ticks();
        if (t / 100 != last_sec)  { last_sec = t / 100; con_bar(); }
        if (t / 50 != last_blink) { last_blink = t / 50; on = !on; con_cursor(on); }
        task_sleep(10);   /* nap: other tasks get the CPU meanwhile */
    }
}

static void replace_line(char *buf, int *len, const char *with) {
    while (*len > 0) { (*len)--; con_putc('\b'); }
    for (; *with && *len < LINE_MAX - 1; with++) { buf[(*len)++] = *with; con_putc(*with); }
    buf[*len] = 0;
}

static bool at_prompt;
bool sh_at_prompt(void) { return at_prompt; }

static void read_line(char *buf) {
    int len = 0, h = hist_len;
    buf[0] = 0;
    at_prompt = true;
    for (;;) {
        int k = sh_wait_key();
        if (k == '\n') { con_putc('\n'); buf[len] = 0; at_prompt = false; return; }
        if (k == '\b') { if (len) { len--; con_putc('\b'); } continue; }
        if (k == 12)   { con_clear(); buf[0] = 0; at_prompt = false; return; }       /* ctrl+l */
        if (k == 3)    { kprintf(C_DIM "^C\n"); buf[0] = 0; at_prompt = false; return; }   /* ctrl+c */
        if (k == KEY_UP && h > 0)          { replace_line(buf, &len, history[--h]); continue; }
        if (k == KEY_DOWN && h < hist_len) { h++; replace_line(buf, &len, h < hist_len ? history[h] : ""); continue; }
        if (k >= ' ' && k < 127 && len < LINE_MAX - 1) { buf[len++] = (char)k; con_putc((char)k); }
    }
}

/* ---- helpers commands can use ------------------------------------------------ */

static const char *stdin_buf;
static size_t stdin_len;
static bool tty = true;

int  sh_cwd(void)          { return cwd; }
void sh_set_cwd(int node)  { cwd = node; }
bool sh_is_tty(void)       { return tty; }

const char *sh_hostname(void) {
    static char name[32];
    int n = fs_lookup(0, "/etc/hostname");
    if (n < 0 || fs_get(n)->dir || fs_get(n)->size == 0) return "dream";
    size_t i = 0, size = fs_get(n)->size;
    const char *d = fs_data(n);
    while (i < size && i < sizeof name - 1 && d[i] != '\n') { name[i] = d[i]; i++; }
    name[i] = 0;
    return i ? name : "dream";
}

void sh_print_history(void) {
    for (int i = 0; i < hist_len; i++) kprintf(C_DIM "%4d  " C_FG "%s\n", i + 1, history[i]);
}

bool sh_parse_num(const char *s, uint64_t *out) {
    uint64_t v = 0;
    if (!*s) return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + (*s - '0');
    }
    *out = v;
    return true;
}

void sh_perror(const char *cmd, const char *what, int err) {
    kerrorf(C_ERR "%s" C_DIM ": %s: " C_FG "%s\n", cmd, what, fs_strerror(err));
}

const char *sh_input(int argc, char **argv, int i, size_t *len) {
    if (argc > i) {
        int n = fs_lookup(cwd, argv[i]);
        if (n < 0) { sh_perror(argv[0], argv[i], n); return NULL; }
        if (fs_get(n)->dir) { sh_perror(argv[0], argv[i], FS_EISDIR); return NULL; }
        *len = fs_get(n)->size;
        return fs_data(n);
    }
    if (stdin_buf) { *len = stdin_len; return stdin_buf; }
    kerrorf(C_ERR "%s" C_DIM ": no input. give it a file, or pipe something in: "
            C_FG "cat notes.txt | %s\n", argv[0], argv[0]);
    return NULL;
}

/* ---- built-in commands --------------------------------------------------------- */

static void cmd_help(int, char **);

static void cmd_clear(int argc, char **argv) { (void)argc; (void)argv; con_clear(); }

static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) kprintf("%s%s", argv[i], i + 1 < argc ? " " : "");
    kprintf("\n");
}

static void cmd_uptime(int argc, char **argv) {
    (void)argc; (void)argv;
    uint64_t t = pit_ticks(), s = t / pit_hz();
    kprintf("up " C_ACCENT "%lu" C_FG "h " C_ACCENT "%lu" C_FG "m " C_ACCENT "%lu" C_FG "s"
            C_DIM "   (%lu timer ticks)\n", s / 3600, (s / 60) % 60, s % 60, t);
}

static void cmd_mem(int argc, char **argv) { (void)argc; (void)argv; mem_print_map(); }

static void cmd_theme(int argc, char **argv) {
    if (argc > 1 && theme_set(argv[1]) == 0) return;
    if (argc > 1) kerrorf(C_ERR "no theme called '%s'\n", argv[1]);
    for (int i = 0; i < theme_count; i++) {
        const struct theme *t = &themes[i];
        kprintf("  %s %-10s ", t == theme ? C_ACCENT ">" : " ", t->name);
        kprintf(C_DIM "%s\n", t == theme ? "(active)" : "");
    }
    kprintf(C_DIM "  usage: theme <name>\n");
}

static void cmd_fetch(int argc, char **argv) {
    (void)argc; (void)argv;
    static const char *logo[] = {
        C_ACCENT "     \\  |  /    ",
        C_ACCENT "   -- (" C_FG "*" C_ACCENT ") --    ",
        C_ACCENT "     /  |  \\    ",
        "                ",
        C_FG     "     d r e a m  ",
        "                ",
        "                ",
        "                ",
    };
    uint64_t s = pit_ticks() / pit_hz();
    char info[8][96];
    snprintf(info[0], 96, C_ACCENT "root" C_DIM "@" C_ACCENT "%s", sh_hostname());
    snprintf(info[1], 96, C_DIM "----------");
    snprintf(info[2], 96, C_ACCENT "os      " C_FG "dream " DREAM_VERSION " x86_64");
    snprintf(info[3], 96, C_ACCENT "boot    " C_FG "limine");
    snprintf(info[4], 96, C_ACCENT "uptime  " C_FG "%lum %lus", s / 60, s % 60);
    snprintf(info[5], 96, C_ACCENT "display " C_FG "%ux%u", fb.width, fb.height);
    snprintf(info[6], 96, C_ACCENT "memory  " C_FG "%lu MiB", mem_usable_bytes() >> 20);
    snprintf(info[7], 96, C_ACCENT "theme   " C_FG "%s", theme->name);
    kprintf("\n");
    for (int i = 0; i < 8; i++) kprintf("  %s  %s\n", logo[i], info[i]);
    kprintf("  %18s", "");
    for (int p = 0; p < PAL_COUNT; p++) kprintf("\033%c" BLOCK BLOCK BLOCK " ", '0' + p);
    kprintf("\n\n");
}

static void cmd_crash(int argc, char **argv) {
    const char *how = argc > 1 ? argv[1] : "";
    if (!strcmp(how, "pf"))      *(volatile uint64_t *)0x0000700000000000ULL = 1;   /* unmapped */
    else if (!strcmp(how, "ud")) asm volatile("ud2");
    else if (!strcmp(how, "de")) asm volatile("xor %%ecx, %%ecx; div %%ecx" ::: "eax", "ecx", "edx");
    else if (!strcmp(how, "bp")) asm volatile("int3");
    else kprintf("usage: crash <pf|ud|de|bp>   " C_DIM "page fault, bad opcode, divide by zero, breakpoint\n");
}

static void cmd_reboot(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf(C_DIM "rebooting...\n");
    while (inb(0x64) & 2) {}
    outb(0x64, 0xfe);            /* ask the keyboard controller to pulse the reset line */
    halt_forever();
}

static void cmd_shutdown(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf(C_DIM "goodbye.\n");
    diskfs_autosave();
    outw(0x604, 0x2000);         /* QEMU's ACPI power-off port; real hardware needs ACPI */
    outw(0xb004, 0x2000);        /* older QEMU / Bochs */
    halt_forever();
}

static void cmd_hello(int argc, char **argv) {
    kprintf("hello from my own command!\n");
}

/* ---- the command table ------------------------------------------------------- */

static const struct { const char *name, *help; void (*fn)(int, char **); } commands[] = {
    /* files */
    { "ls",       "list a directory (-l long, -a all)",   cmd_ls       },
    { "cd",       "change directory (cd = home)",         cmd_cd       },
    { "pwd",      "print the current directory",          cmd_pwd      },
    { "tree",     "show a directory as a tree",           cmd_tree     },
    { "mkdir",    "make directories (-p for parents)",    cmd_mkdir    },
    { "touch",    "create a file / update its time",      cmd_touch    },
    { "cat",      "print files or piped input",           cmd_cat      },
    { "cp",       "copy a file",                          cmd_cp       },
    { "mv",       "move or rename",                       cmd_mv       },
    { "rm",       "remove files (-r for directories)",    cmd_rm       },
    { "rmdir",    "remove an empty directory",            cmd_rmdir    },
    /* text */
    { "echo",     "print its arguments",                  cmd_echo     },
    { "grep",     "lines containing a pattern",           cmd_grep     },
    { "head",     "first lines (-n N)",                   cmd_head     },
    { "tail",     "last lines (-n N)",                    cmd_tail     },
    { "wc",       "count lines, words, bytes",            cmd_wc       },
    { "hexdump",  "show a file's raw bytes",              cmd_hexdump  },
    { "edit",     "full-screen text editor",              cmd_edit     },
    { "nano",     "same as edit",                         cmd_edit     },
    { "paint",    "draw with the mouse (on the desktop)", cmd_paint    },
    { "startx",   "start the desktop",                    cmd_startx   },
    { "files",    "file browser (on the desktop)",        cmd_files    },
    { "exit",     "leave the desktop",                    cmd_exit     },
    /* disks */
    { "lsblk",    "list disks",                           cmd_lsblk    },
    { "blkread",  "show a raw disk sector",               cmd_blkread  },
    { "blkwrite", "write text into a raw sector",         cmd_blkwrite },
    { "sync",     "save files to disk now",               cmd_sync     },
    { "mkfs",     "format a disk with dreamfs",           cmd_mkfs     },
    /* system */
    { "uname",    "system name (-a for everything)",      cmd_uname    },
    { "hostname", "show or set the machine name",         cmd_hostname },
    { "whoami",   "who you are (spoiler: root)",          cmd_whoami   },
    { "date",     "date and time from the RTC chip",      cmd_date     },
    { "uptime",   "time since boot",                      cmd_uptime   },
    { "free",     "memory and filesystem usage",          cmd_free     },
    { "mem",      "the physical memory map",              cmd_mem      },
    { "memtest",  "stress-test kmalloc/kfree",            cmd_memtest  },
    { "lscpu",    "what CPU this is",                     cmd_lscpu    },
    { "dmesg",    "the kernel boot log",                  cmd_dmesg    },
    { "ps",       "list running tasks",                   cmd_ps       },
    { "history",  "commands you've typed",                cmd_history  },
    { "sleep",    "wait N seconds (ctrl+c stops)",        cmd_sleep    },
    /* dream */
    { "help",     "this list",                            cmd_help     },
    { "fetch",    "system info, with style",              cmd_fetch    },
    { "theme",    "list or switch colour themes",         cmd_theme    },
    { "clear",    "clear the screen (or ctrl+l)",         cmd_clear    },
    { "hello",    "say hi",                               cmd_hello    },
    { "crash",    "trigger a CPU exception on purpose",   cmd_crash    },
    { "reboot",   "restart the machine",                  cmd_reboot   },
    { "shutdown", "power off (QEMU)",                     cmd_shutdown },
};
#define NCOMMANDS (int)(sizeof commands / sizeof commands[0])

static void cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    int half = (NCOMMANDS + 1) / 2;
    for (int i = 0; i < half; i++) {
        kprintf("  " C_ACCENT "%-10s" C_DIM "%-38s", commands[i].name, commands[i].help);
        if (i + half < NCOMMANDS)
            kprintf(C_ACCENT "%-10s" C_DIM "%s", commands[i + half].name, commands[i + half].help);
        kprintf("\n");
    }
    kprintf(C_DIM "\n  programs in " C_FG "/bin" C_DIM " run in user mode: " C_FG "ls /bin" C_DIM " to see them\n");
    kprintf(C_DIM "  combine them:  " C_FG "ls | wc" C_DIM "   " C_FG "echo hi > f.txt"
            C_DIM "   " C_FG "mkdir x && cd x" C_DIM "   up/down = history\n");
}

/* ---- parsing a line ------------------------------------------------------------ */

static char tokbuf[LINE_MAX * 2];

/* Break a line into words and operators ( | > >> ; && ). Quotes group words. */
static int tokenize(const char *s, char **tok, bool *op) {
    char *out = tokbuf;
    int n = 0;
    while (*s && n < TOK_MAX) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        tok[n] = out;
        if (*s == '|' || *s == '>' || *s == ';' || (s[0] == '&' && s[1] == '&')) {
            op[n] = true;
            *out++ = *s;
            if ((s[0] == '>' && s[1] == '>') || s[0] == '&') *out++ = *++s;
            s++;
        } else {
            op[n] = false;
            while (*s && *s != ' ' && *s != '\t' && *s != '|' && *s != '>' && *s != ';'
                   && !(s[0] == '&' && s[1] == '&')) {
                if (*s == '"' || *s == '\'') {
                    char q = *s++;
                    while (*s && *s != q) *out++ = *s++;
                    if (*s) s++;
                } else *out++ = *s++;
            }
        }
        *out++ = 0;
        n++;
    }
    return n;
}

/* ---- running commands ---------------------------------------------------------- */

/* Pipes and > need somewhere to collect a command's output instead of the screen. */
static char *capbuf[2];
static size_t caplen[2], capcap[2];
static int cap;
static bool cap_esc;

static void capture(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (cap_esc) { cap_esc = false; continue; }   /* colours don't belong in files */
        if (c == '\033') { cap_esc = true; continue; }
        if (c == BLOCK[0]) c = '#';
        if (caplen[cap] == capcap[cap]) {             /* full: grow it (up to 1 MiB) */
            size_t want = capcap[cap] ? capcap[cap] * 2 : 4096;
            if (want > FS_FILE_MAX) return;
            char *p = krealloc(capbuf[cap], want);
            if (!p) return;
            capbuf[cap] = p;
            capcap[cap] = want;
        }
        capbuf[cap][caplen[cap]++] = c;
    }
}

/* Not a built-in? Look for a program file (in /bin, or a path) and run it in
   user mode. The shell waits for it to finish, passing your keys along, and
   Ctrl+C stops it. */
static bool run_program(int argc, char **argv) {
    char path[128];
    if (strchr(argv[0], '/')) strlcpy(path, argv[0], sizeof path);
    else snprintf(path, sizeof path, "/bin/%s", argv[0]);
    int n = fs_lookup(cwd, path);
    if (n < 0 || fs_get(n)->dir) return false;

    const char *name = strrchr(path, '/') + 1;
    struct process *p = process_spawn(name, fs_data(n), fs_get(n)->size, argc, argv,
                                      stdin_buf, stdin_len, stdin_buf != NULL);
    if (!p) return true;
    while (!p->done) {
        int k = wm_active() ? term_key() : kbd_poll();
        if (k == 3) p->killed = true;                     /* ctrl+c */
        else if (k >= 0 && !p->piped) process_key(p, k);
        task_sleep(10);
    }
    if (p->fault[0])
        kerrorf(C_ERR "segmentation fault" C_DIM ": %s: %s\n" C_DIM
                "the program was stopped. dream is fine.\n", p->name, p->fault);
    else if (p->killed) kerrorf(C_DIM "^C\n");
    else if (p->exit_code) kerrorf(C_DIM "[%s exited with %d]\n", p->name, p->exit_code);
    process_free(p);
    return true;
}

static void run_one(int argc, char **argv) {
    for (int i = 0; i < NCOMMANDS; i++) {
        if (!strcmp(commands[i].name, argv[0])) { commands[i].fn(argc, argv); return; }
    }
    if (run_program(argc, argv)) return;
    kerrorf(C_ERR "%s" C_DIM ": not a command. try " C_FG "help" C_DIM " or " C_FG "ls /bin\n", argv[0]);
}

/* Run one pipeline: cmd | cmd | cmd [> file]. Returns false if anything failed. */
static bool run_pipeline(char **tok, bool *op, int n) {
    unsigned errors_before = kerror_count;
    const char *redir = NULL;
    bool append = false;

    for (int i = 0; i < n; i++) {
        if (!(op[i] && tok[i][0] == '>')) continue;
        if (i + 1 >= n || op[i + 1]) { kerrorf(C_ERR "syntax" C_DIM ": > needs a file name\n"); return false; }
        redir = tok[i + 1];
        append = tok[i][1] == '>';
        for (int j = i; j + 2 < n; j++) { tok[j] = tok[j + 2]; op[j] = op[j + 2]; }
        n -= 2;
        i--;
    }

    int start = 0, seg = 0;
    for (;;) {
        int end = start;
        while (end < n && !(op[end] && tok[end][0] == '|')) end++;
        bool last = end >= n;
        if (end == start) { kerrorf(C_ERR "syntax" C_DIM ": empty command around |\n"); break; }

        if (!last || redir) {        /* send this command's output into a buffer */
            cap = seg % 2;
            caplen[cap] = 0;
            cap_esc = false;
            kout_redirect(capture);
            tty = false;
        }
        run_one(end - start, &tok[start]);
        kout_redirect(NULL);
        tty = true;

        if (last) break;
        stdin_buf = capbuf[cap];     /* ...which the next command reads as its input */
        stdin_len = caplen[cap];
        start = end + 1;
        seg++;
    }
    stdin_buf = NULL;

    if (redir && kerror_count == errors_before) {
        int f = fs_lookup(cwd, redir);
        if (f == FS_ENOENT) f = fs_create(cwd, redir, false);
        if (f >= 0 && fs_get(f)->dir) f = FS_EISDIR;
        if (f >= 0) f = fs_write(f, capbuf[cap], caplen[cap], append);
        if (f < 0) sh_perror("shell", redir, f);
    }
    return kerror_count == errors_before;
}

/* Split the line at ; and && and run each pipeline in turn. */
static void run_line(char *line) {
    char *tok[TOK_MAX];
    bool op[TOK_MAX];
    int n = tokenize(line, tok, op);
    int start = 0;
    bool ok = true, skip = false;
    while (start < n) {
        int end = start;
        while (end < n && !(op[end] && (tok[end][0] == ';' || tok[end][0] == '&'))) end++;
        if (end > start && !skip) ok = run_pipeline(&tok[start], &op[start], end - start);
        if (end >= n) break;
        skip = tok[end][0] == '&' && (!ok || skip);
        start = end + 1;
    }
}

static void prompt(void) {
    char path[256];
    fs_path(cwd, path, sizeof path);
    const char *shown = path;
    char pretty[256];
    if (!strncmp(path, "/root", 5) && (path[5] == 0 || path[5] == '/')) {
        snprintf(pretty, sizeof pretty, "~%s", path + 5);
        shown = pretty;
    }
    kprintf(C_ACCENT "%s " C_FG "%s " C_DIM "> " C_FG, sh_hostname(), shown);
}

void shell_run(void) {
    char line[LINE_MAX];
    cwd = fs_home();
    for (;;) {
        prompt();
        read_line(line);
        if (!line[0]) continue;

        if (hist_len == HIST) { memmove(history[0], history[1], sizeof history[0] * (HIST - 1)); hist_len--; }
        memcpy(history[hist_len++], line, LINE_MAX);

        run_line(line);
        if (!fs_get(cwd)->used || !fs_get(cwd)->dir) cwd = 0;   /* our directory vanished */

        /* if that changed any files, write them to disk so they survive a reboot */
        int r = diskfs_autosave();
        if (r < 0) kerrorf(C_WARN "warning" C_DIM ": couldn't save to disk: %s\n", diskfs_strerror(r));
    }
}
