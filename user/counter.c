/* counter: counts up twice a second until you press Ctrl+C.
   While it sleeps, other tasks (like the desktop) keep running. */
#include "dream.h"

int main(int argc, char **argv) {
    int limit = argc > 1 ? atoi(argv[1]) : 0;
    printf(DIM "counting%s. Ctrl+C stops me.\n", limit ? "" : " forever");
    for (int i = 1; !limit || i <= limit; i++) {
        printf(ACCENT "%d" DIM "  (%lu ms since boot)\n", i, uptime_ms());
        sleep_ms(500);
    }
    return 0;
}
