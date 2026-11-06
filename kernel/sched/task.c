#include "sched/task.h"
#include "arch/cpu.h"
#include "arch/io.h"
#include "mm/heap.h"

#define STACK_SIZE TASK_STACK_SIZE

extern void switch_to(uint64_t *save_rsp, uint64_t load_rsp);

static struct task boot_task;
static struct task *current, *idle;
static int ntasks, next_id;
static uint64_t total_ticks;
static uint64_t kcr3;

uint64_t kernel_cr3(void) { return kcr3; }

struct task *task_current(void) { return current; }
uint64_t sched_total_ticks(void) { return total_ticks; }

/* ---- choosing who runs next ----------------------------------------------------- */

/* Round robin: start looking just after the current task, go around the ring once,
   take the first one that's ready (waking sleepers whose time has come). */
static struct task *pick(void) {
    uint64_t now = pit_ticks();
    struct task *t = current->next;
    for (int i = 0; i < ntasks; i++, t = t->next) {
        if (t == idle) continue;
        if (t->state == TASK_SLEEPING && now >= t->wake) t->state = TASK_READY;
        if (t->state == TASK_READY || t->state == TASK_RUNNING) return t;
    }
    return idle;                         /* nobody wants the CPU */
}

/* Free tasks that have finished. (A task can't free its own stack while it's
   standing on it, so whoever runs next does the cleaning up.) */
static void reap(void) {
    struct task *prev = current;
    for (int i = 0; i < ntasks; i++) {
        struct task *t = prev->next;
        if (t->state == TASK_DEAD && t != current) {
            prev->next = t->next;
            ntasks--;
            kfree(t->stack);
            kfree(t);
        } else {
            prev = t;
        }
    }
}

/* Switch to whoever should run now. Interrupts must be off. */
static void schedule(void) {
    reap();
    struct task *prev = current, *next = pick();
    if (next == prev) return;
    if (prev->state == TASK_RUNNING) prev->state = TASK_READY;
    next->state = TASK_RUNNING;
    current = next;
    /* if it's a user program, interrupts from ring 3 must land on its own kernel stack */
    if (next->stack) tss_set_rsp0((uint64_t)next->stack + STACK_SIZE);
    /* ...and it sees its own address space */
    uint64_t want = next->cr3 ? next->cr3 : kcr3;
    if (read_cr3() != want) write_cr3(want);
    switch_to(&prev->rsp, next->rsp);
    /* ...and some time later, another task switches back to us, and we return here. */
}

void sched_tick(void) {
    if (!current) return;
    current->ticks++;
    total_ticks++;
    schedule();                          /* time's up: next! */
}

/* ---- tasks ---------------------------------------------------------------------- */

/* Every new task starts here (switch_to "returns" into it). */
static void task_start(void) {
    sti();                               /* we arrived with interrupts off */
    current->fn(current->arg);
    task_exit();
}

static void idle_loop(void *arg) {
    (void)arg;
    for (;;) asm volatile("sti; hlt");   /* sleep until the next interrupt */
}

void sched_init(const char *name) {
    /* whatever is running right now (kmain) becomes the first task */
    boot_task = (struct task){ .id = next_id++, .state = TASK_RUNNING, .next = &boot_task };
    strlcpy(boot_task.name, name, sizeof boot_task.name);
    current = &boot_task;
    ntasks = 1;
    kcr3 = read_cr3();                   /* the page tables Limine built: the kernel's */
    idle = task_spawn("idle", idle_loop, NULL);
}

struct task *task_spawn(const char *name, void (*fn)(void *), void *arg) {
    struct task *t = kzalloc(sizeof *t);
    uint8_t *stack = t ? kmalloc(STACK_SIZE) : NULL;
    if (!stack) { kfree(t); return NULL; }
    strlcpy(t->name, name, sizeof t->name);
    t->stack = stack;
    t->fn = fn;
    t->arg = arg;
    t->state = TASK_READY;

    /* Build a stack that looks like switch_to was called from task_start:
       the registers it will pop, then the address its `ret` will jump to. */
    uint64_t *sp = (uint64_t *)(stack + STACK_SIZE);
    *--sp = 0;                           /* keeps the stack 16-byte aligned for C */
    *--sp = (uint64_t)task_start;        /* where `ret` goes */
    *--sp = 0x2;                         /* rflags: interrupts off for now */
    for (int i = 0; i < 6; i++) *--sp = 0;   /* rbp rbx r12 r13 r14 r15 */
    t->rsp = (uint64_t)sp;

    uint64_t f = irq_save();
    t->id = next_id++;
    t->next = current->next;             /* join the ring right after us */
    current->next = t;
    ntasks++;
    irq_restore(f);
    return t;
}

void task_yield(void) {
    uint64_t f = irq_save();
    schedule();
    irq_restore(f);
}

void task_sleep(uint64_t ms) {
    uint64_t ticks = (ms * pit_hz() + 999) / 1000;
    uint64_t f = irq_save();
    current->wake = pit_ticks() + (ticks ? ticks : 1);
    current->state = TASK_SLEEPING;
    schedule();
    irq_restore(f);
}

void task_exit(void) {
    cli();
    current->state = TASK_DEAD;
    schedule();
    for (;;) hlt();                      /* never reached */
}

int task_list(struct task_info *out, int max) {
    uint64_t f = irq_save();
    int n = 0;
    struct task *t = &boot_task;
    do {
        if (n < max && t->state != TASK_DEAD) {
            out[n] = (struct task_info){ t->id, "", t->state, t->ticks };
            strlcpy(out[n].name, t->name, sizeof out[n].name);
            n++;
        }
        t = t->next;
    } while (t != &boot_task);
    irq_restore(f);
    return n;
}

/* ---- locks ---------------------------------------------------------------------- */

void mutex_lock(struct mutex *m) {
    for (;;) {
        uint64_t f = irq_save();
        if (!m->owner || m->owner == current) {   /* free, or already ours */
            m->owner = current;
            m->depth++;
            irq_restore(f);
            return;
        }
        irq_restore(f);
        task_yield();                             /* someone else has it: let them finish */
    }
}

void mutex_unlock(struct mutex *m) {
    uint64_t f = irq_save();
    if (m->owner == current && --m->depth == 0) m->owner = NULL;
    irq_restore(f);
}
