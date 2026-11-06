/* mandel: the Mandelbrot set, in colour, with real floating-point maths.
 *
 * For each spot c on the screen, keep doing z = z*z + c starting from z = 0.
 * If z flies off (gets bigger than 2), c is outside the set; how quickly it
 * flew decides the character and colour. Points that never escape are in the set.
 *
 *   mandel            the whole set
 *   mandel <zoom>     zoom in on the "seahorse valley" (try 4, 20, 100) */
#include "dream.h"

int main(int argc, char **argv) {
    int cols = 88, rows = 30, max = 120;
    double zoom = argc > 1 ? (double)atoi(argv[1]) : 1.0;
    if (zoom < 1) zoom = 1;
    double cx = zoom > 1 ? -0.7436 : -0.6, cy = zoom > 1 ? 0.1318 : 0.0;
    double w = 3.2 / zoom, h = 2.4 / zoom;
    static const char shade[] = " .:-=+*#%@";
    static const char *const colour[] = { DIM, ACCENT, OK, WARN, ERR, FG };

    unsigned long t0 = uptime_ms();
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            double x0 = cx - w / 2 + w * c / cols, y0 = cy - h / 2 + h * r / rows;
            double x = 0, y = 0;
            int i = 0;
            while (x * x + y * y <= 4.0 && i < max) {
                double t = x * x - y * y + x0;
                y = 2 * x * y + y0;
                x = t;
                i++;
            }
            if (i == max) { putchar(' '); continue; }
            printf("%s%c", colour[i % 6], shade[i % 9 + 1]);
        }
        printf(FG "\n");
    }
    printf(DIM "%d x %d points, up to %d steps each, in %.2f seconds (zoom %.1f)\n" FG,
           cols, rows, max, (double)(uptime_ms() - t0) / 1000.0, zoom);
    return 0;
}
