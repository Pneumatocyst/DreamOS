/* guess: the classic number guessing game. Interactive: it reads your keys. */
#include "dream.h"

int main(void) {
    int secret = rand() % 100 + 1, tries = 0;
    char line[16];
    printf(ACCENT "I'm thinking of a number from 1 to 100.\n");
    for (;;) {
        printf(DIM "your guess: " FG);
        if (readline(line, sizeof line) < 0) return 1;
        if (!line[0]) continue;
        int g = atoi(line);
        tries++;
        if (g < secret)      printf(WARN "  higher.\n");
        else if (g > secret) printf(WARN "  lower.\n");
        else { printf(OK "  yes! %d it is. you got it in %d tries.\n", secret, tries); return 0; }
    }
}
