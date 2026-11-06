#include "proc/process.h"
#include "arch/cpu.h"
#include "arch/io.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/task.h"
#include "fs/ramfs.h"
#include "shell/shell.h"

#define STACK_TOP   0x00007ffffffff000ull
#define STACK_SIZE  (64 * 1024)

extern void enter_user(uint64_t rip, uint64_t rsp, uint64_t argc, uint64_t argv);
static int next_pid = 100;

/* ---- loading an ELF file ----------------------------------------------------------
 * An ELF executable starts with a header, then a table of "program headers".
 * Each PT_LOAD one says: put this many bytes from the file at this address
 * (and zero-fill up to memsz, for uninitialised variables). That's all a loader
 * really has to do. */

struct __attribute__((packed)) elf_header {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct __attribute__((packed)) elf_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

static const char *load_elf(struct process *p, const uint8_t *data, size_t size) {
    const struct elf_header *h = (const void *)data;
    if (size < sizeof *h || memcmp(h->ident, "\x7f" "ELF", 4)) return "not an ELF program";
    if (h->ident[4] != 2 || h->machine != 62 || h->type != 2) return "not a 64-bit x86 executable";
    if (h->phoff + (uint64_t)h->phnum * sizeof(struct elf_phdr) > size) return "broken program headers";

    for (int i = 0; i < h->phnum; i++) {
        const struct elf_phdr *ph = (const void *)(data + h->phoff + i * sizeof *ph);
        if (ph->type != 1) continue;                               /* PT_LOAD */
        if (ph->filesz > ph->memsz || ph->offset + ph->filesz > size) return "broken segment";
        if (ph->vaddr < 0x1000 || ph->vaddr + ph->memsz > STACK_TOP - STACK_SIZE)
            return "segment outside user space";
        if (!vmm_map_user(p->pml4, ph->vaddr, ph->memsz)) return "out of memory";
        if (!vmm_copy_to(p->pml4, ph->vaddr, data + ph->offset, ph->filesz)) return "copy failed";
    }
    p->entry = h->entry;
    return NULL;
}

/* Put argc/argv on the new program's stack, the way C's main() expects. */
static bool push_args(struct process *p, int argc, char **argv) {
    if (!vmm_map_user(p->pml4, STACK_TOP - STACK_SIZE, STACK_SIZE)) return false;
    uint64_t sp = STACK_TOP, ptrs[16];
    if (argc > 15) argc = 15;
    for (int i = argc - 1; i >= 0; i--) {                         /* the strings themselves */
        size_t n = strlen(argv[i]) + 1;
        sp -= n;
        if (!vmm_copy_to(p->pml4, sp, argv[i], n)) return false;
        ptrs[i] = sp;
    }
    ptrs[argc] = 0;
    sp &= ~0xfull;
    sp -= (argc + 1) * sizeof(uint64_t);                          /* then the argv array */
    if (!vmm_copy_to(p->pml4, sp, ptrs, (argc + 1) * sizeof(uint64_t))) return false;
    p->argv = sp;
    p->argc = argc;
    p->user_rsp = (sp - 8) & ~0xfull;
    return true;
}

/* The first thing a program's task does: switch to its address space, then
   step through the door into ring 3. It never comes back here. */
static void user_start(void *arg) {
    struct process *p = arg;
    struct task *t = task_current();
    t->proc = p;
    uint64_t f = irq_save();
    t->cr3 = virt_to_phys(p->pml4);
    write_cr3(t->cr3);
    irq_restore(f);
    enter_user(p->entry, p->user_rsp, p->argc, p->argv);
}

struct process *process_spawn(const char *name, const void *elf, size_t size,
                              int argc, char **argv, const char *in, size_t in_len, bool piped) {
    struct process *p = kzalloc(sizeof *p);
    if (!p) return NULL;
    p->pid = next_pid++;
    strlcpy(p->name, name, sizeof p->name);
    p->in_data = in;
    p->in_len = in_len;
    p->piped = piped;
    p->pml4 = vmm_new_space();
    const char *err = p->pml4 ? load_elf(p, elf, size) : "out of memory";
    if (!err && !push_args(p, argc, argv)) err = "out of memory";
    if (!err && !(p->task = task_spawn(name, user_start, p))) err = "couldn't create a task";
    if (err) {
        kerrorf(C_ERR "%s" C_DIM ": " C_FG "%s\n", name, err);
        if (p->pml4) vmm_free_space(p->pml4);
        kfree(p);
        return NULL;
    }
    return p;
}

void process_key(struct process *p, int key) {
    unsigned next = (p->kh + 1) % 64;
    if (next != p->kt) { p->keys[p->kh] = (uint8_t)key; p->kh = next; }
}

void process_free(struct process *p) { kfree(p); }

/* ---- ending ------------------------------------------------------------------------- */

__attribute__((noreturn)) static void process_exit(int code) {
    struct task *t = task_current();
    struct process *p = t->proc;
    cli();
    write_cr3(kernel_cr3());              /* step off the program's page tables... */
    t->cr3 = 0;
    t->proc = NULL;
    vmm_free_space(p->pml4);              /* ...then give all its memory back */
    p->exit_code = code;
    p->done = true;                       /* the shell is waiting for this */
    task_exit();
}

static const char *fault_names[32] = {
    [0] = "divide by zero", [6] = "invalid instruction", [13] = "general protection fault",
    [14] = "page fault", [3] = "breakpoint",
};

void process_fault(struct regs *r) {
    struct process *p = task_current()->proc;
    if (!p) panic_exception(r);
    const char *what = r->vector < 32 && fault_names[r->vector] ? fault_names[r->vector] : "CPU exception";
    if (r->vector == 14)
        snprintf(p->fault, sizeof p->fault, "%s touching %p (at %p)", what, (void *)read_cr2(), (void *)r->rip);
    else
        snprintf(p->fault, sizeof p->fault, "%s (at %p)", what, (void *)r->rip);
    process_exit(139);
}

void process_check_killed(void) {
    struct process *p = task_current()->proc;
    if (p && p->killed) process_exit(130);
}

/* ---- system calls ---------------------------------------------------------------------
 * A program puts a number in rax and arguments in rdi/rsi/rdx, then runs `int 0x80`.
 * We land in isr_dispatch on the program's kernel stack, with its registers saved in r.
 * Rule one: never trust a pointer from user space. Check it first. */

#define BAD ((uint64_t)-1)

static uint64_t sys_write(struct process *p, uint64_t fd, uint64_t buf, uint64_t len) {
    if (fd != 1 && fd != 2) return BAD;
    if (len > 65536) len = 65536;
    if (!vmm_user_ok(p->pml4, buf, len, false)) return BAD;
    kwrite((const char *)buf, len);
    return len;
}

static uint64_t sys_read(struct process *p, uint64_t fd, uint64_t buf, uint64_t len) {
    if (fd != 0 || !vmm_user_ok(p->pml4, buf, len, true)) return BAD;
    uint8_t *out = (uint8_t *)buf;
    if (p->piped) {                                   /* reading from a pipe: then end-of-file */
        size_t n = p->in_len - p->in_pos;
        if (n > len) n = len;
        memcpy(out, p->in_data + p->in_pos, n);
        p->in_pos += n;
        return n;
    }
    for (;;) {                                        /* the keyboard: wait for at least one key */
        if (p->killed) process_exit(130);
        size_t n = 0;
        while (n < len && p->kt != p->kh) {
            out[n++] = p->keys[p->kt];
            p->kt = (p->kt + 1) % 64;
        }
        if (n) return n;
        task_sleep(10);
    }
}

static uint64_t sys_readfile(struct process *p, uint64_t path, uint64_t buf, uint64_t len) {
    char name[256];
    size_t i = 0;
    for (; i < sizeof name - 1; i++) {                /* copy the path in, byte by checked byte */
        if (!vmm_user_ok(p->pml4, path + i, 1, false)) return BAD;
        name[i] = *(const char *)(path + i);
        if (!name[i]) break;
    }
    name[i] = 0;
    int n = fs_lookup(sh_cwd(), name);
    if (n < 0 || fs_get(n)->dir) return BAD;
    uint64_t size = fs_get(n)->size;
    if (size > len) size = len;
    if (!vmm_user_ok(p->pml4, buf, size, true)) return BAD;
    memcpy((void *)buf, fs_data(n), size);
    return size;
}

void syscall_dispatch(struct regs *r) {
    struct process *p = task_current()->proc;
    if (!p) return;
    sti();                                            /* system calls can be interrupted */
    uint64_t a = r->rdi, b = r->rsi, c = r->rdx;
    switch (r->rax) {
    case SYS_EXIT:     process_exit((int)a);
    case SYS_WRITE:    r->rax = sys_write(p, a, b, c); break;
    case SYS_READ:     r->rax = sys_read(p, a, b, c); break;
    case SYS_SLEEP:
        for (uint64_t left = a; left; ) {             /* in slices, so Ctrl+C can get in */
            uint64_t step = left > 50 ? 50 : left;
            task_sleep(step);
            left -= step;
            if (p->killed) process_exit(130);
        }
        r->rax = 0;
        break;
    case SYS_GETPID:   r->rax = p->pid; break;
    case SYS_UPTIME:   r->rax = pit_ticks() * 1000 / pit_hz(); break;
    case SYS_READFILE: r->rax = sys_readfile(p, a, b, c); break;
    default:           r->rax = BAD;
    }
    if (p->killed) process_exit(130);
}
