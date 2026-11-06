#pragma once
#include "kernel.h"

struct regs;
struct task;

/* A user program: its own address space, running in ring 3 on its own task. */
struct process {
    int pid;
    char name[16];
    uint64_t *pml4;                  /* its page tables */
    struct task *task;
    volatile bool done, killed;
    int exit_code;
    char fault[96];                  /* why it was killed, if it crashed */

    /* input: piped data, or keys the shell passes along */
    const char *in_data;
    size_t in_len, in_pos;
    bool piped;
    volatile uint8_t keys[64];
    volatile unsigned kh, kt;

    uint64_t entry, user_rsp, argv;
    int argc;

    bool detached;                   /* runs in the background: nobody waits for it */
    struct process *next_bg;         /* the list of background programs */
};

#include <dream/abi.h>     /* system call numbers */

/* Load an ELF program and start it. NULL (with a message printed) on failure. */
struct process *process_spawn(const char *name, const void *elf, size_t size,
                              int argc, char **argv, const char *in, size_t in_len, bool piped);
void process_key(struct process *p, int key);

/* Start /bin/<name> (or a path) in the background: it runs on its own, and is
   tidied up after it ends (see process_reap). NULL if there's no such program. */
struct process *program_start(const char *name, int argc, char **argv);
void process_reap(void);
bool user_string(struct process *p, uint64_t from, char *out, size_t n);   /* copy a string in from user space */                 /* free background programs that have ended */
bool program_running(const char *name);  /* is a background program called this running? */
void process_free(struct process *p);

void syscall_dispatch(struct regs *r);
__attribute__((noreturn)) void process_fault(struct regs *r);
void process_check_killed(void);
