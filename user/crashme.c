/* crashme: misbehaves on purpose, to show that the kernel is protected.
 *   crashme kernel   write into the kernel's memory    -> page fault
 *   crashme null     follow a NULL pointer             -> page fault
 *   crashme cli      turn off interrupts (not allowed) -> general protection fault
 *   crashme io       talk to hardware directly         -> general protection fault */
#include "dream.h"

int main(int argc, char **argv) {
    const char *how = argc > 1 ? argv[1] : "kernel";
    printf(DIM "about to try: " WARN "%s" DIM ". the CPU should stop me...\n", how);
    if (!strcmp(how, "kernel"))    *(volatile unsigned long *)0xffffffff80000000ul = 0xdead;
    else if (!strcmp(how, "null")) *(volatile int *)0 = 1;
    else if (!strcmp(how, "cli"))  asm volatile("cli");
    else if (!strcmp(how, "io"))   asm volatile("outb %%al, $0x64" :: "a"(0xfe));   /* would reboot! */
    else { printf("usage: crashme kernel|null|cli|io\n"); return 1; }
    printf(ERR "...it didn't stop me. that's bad!\n");
    return 1;
}
