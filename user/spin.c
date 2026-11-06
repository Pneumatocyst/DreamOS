/* spin: an infinite loop that never asks the kernel for anything. In a system
   without preemption it would freeze everything. Here the timer still
   interrupts it every 10 ms, so the desktop stays smooth, and Ctrl+C still works. */
#include "dream.h"

int main(void) {
    printf(WARN "spinning at 100%% CPU, forever." DIM " watch the desktop stay smooth. Ctrl+C stops me.\n");
    unsigned long n = 0;
    for (;;) {
        n++;
        asm volatile("" : "+r"(n));   /* stops the compiler from deleting the "useless" loop */
    }
}
