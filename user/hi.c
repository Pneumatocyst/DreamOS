/* hi: the first user program. It runs in ring 3, in its own address space,
   and talks to the kernel only through system calls. */
#include "dream.h"

int main(int argc, char **argv) {
    printf(ACCENT "hello from user mode!" FG "\n");
    printf(DIM "  I'm process " FG "%d" DIM ", running in ring 3 at address " FG "%p\n", getpid(), (unsigned long)main);
    printf(DIM "  my stack is at " FG "%p" DIM " (my own private memory)\n", (unsigned long)&argc);
    printf(DIM "  you gave me " FG "%d" DIM " argument(s):", argc);
    for (int i = 0; i < argc; i++) printf(" " FG "%s" DIM, argv[i]);
    printf("\n");
    return 0;
}
