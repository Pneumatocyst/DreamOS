#pragma once
#include "kernel.h"

/* Kernel tasks (threads) and a preemptive round-robin scheduler.
 *
 * Each task has its own stack. The timer fires 100 times a second; each time,
 * the scheduler saves the running task's registers and switches to the next
 * task that's ready. So tasks take turns, 10 ms at a time, and a busy one can't
 * hog the CPU. When nobody has anything to do, the idle task halts the CPU. */

struct process;
enum task_state { TASK_READY, TASK_RUNNING, TASK_SLEEPING, TASK_DEAD };

struct task {
    int id;
    char name[16];
    enum task_state state;
    uint64_t rsp;                /* saved stack pointer while not running */
    void *stack;                 /* NULL for the boot task (Limine gave us that stack) */
    uint64_t wake;               /* when sleeping: the tick to wake up at */
    uint64_t ticks;              /* CPU time used, in timer ticks */
    void (*fn)(void *);
    void *arg;
    uint64_t cr3;                /* page tables (physical address); 0 = the kernel's */
    struct process *proc;        /* the user program this task runs, if any */
    struct task *next;           /* all tasks form a ring */
};

struct process;
#define TASK_STACK_SIZE (64 * 1024)
uint64_t kernel_cr3(void);

void sched_init(const char *boot_task_name);
struct task *task_spawn(const char *name, void (*fn)(void *), void *arg);
void task_yield(void);                       /* let someone else run */
void task_sleep(uint64_t ms);                /* give up the CPU for at least this long */
__attribute__((noreturn)) void task_exit(void);
struct task *task_current(void);
void sched_tick(void);                       /* called by the timer interrupt */

/* A lock that tasks wait their turn for. It's "recursive": the task holding it
   may lock it again (and must unlock as many times). Waiting tasks step aside
   with task_yield, so the holder gets to finish. Don't hold one with interrupts off. */
struct mutex { struct task *volatile owner; int depth; };
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);

struct task_info { int id; char name[16]; enum task_state state; uint64_t ticks; };
int  task_list(struct task_info *out, int max);
uint64_t sched_total_ticks(void);
