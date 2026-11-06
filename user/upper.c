/* upper: reads its input and SHOUTS it back. Try:  cat readme.txt | upper */
#include "dream.h"

int main(void) {
    char buf[256];
    long n;
    while ((n = read(0, buf, sizeof buf)) > 0) {
        for (long i = 0; i < n; i++) if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 32;
        write(1, buf, n);
    }
    return 0;
}
