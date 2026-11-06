/* Wallpapers, painted from maths at startup (and when the theme changes).
 * They use the theme's colours, so every style works with every theme.
 *
 *   glow     soft blobs of colour on a gradient (the original)
 *   aurora   curtains of light over a night sky
 *   dunes    layered hills under a big moon
 *   nebula   a starfield with clouds of colour
 *   plain    just the gradient */
#include "gui/wm.h"
#include "drivers/theme.h"

const char *const wallpaper_names[] = { "aurora", "dunes", "glow", "nebula", "plain" };
const int wallpaper_count = 5;

/* sin of one turn split into 60 steps, x1000; isin interpolates between them.
   phase: 0..1023 is one full turn. */
static const int16_t sin60[61] = {
    0, 105, 208, 309, 407, 500, 588, 669, 743, 809, 866, 914, 951, 978, 995,
    1000, 995, 978, 951, 914, 866, 809, 743, 669, 588, 500, 407, 309, 208, 105,
    0, -105, -208, -309, -407, -500, -588, -669, -743, -809, -866, -914, -951, -978, -995,
    -1000, -995, -978, -951, -914, -866, -809, -743, -669, -588, -500, -407, -309, -208, -105, 0,
};

static int isin(int phase) {
    phase &= 1023;
    int pos = phase * 60, i = pos >> 10, f = pos & 1023;
    return sin60[i] + (sin60[i + 1] - sin60[i]) * f / 1024;
}

static uint32_t seed;
static uint32_t rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }

/* film grain: a whisper of noise hides the stripes ("banding") in smooth gradients */
static uint32_t grain(uint32_t c) {
    uint32_t r = rnd() & 7;
    if (r == 0) return gfx_mix(c, 0xffffff, 5);
    if (r == 1) return gfx_mix(c, 0x000000, 5);
    return c;
}

static void base(struct surface *s) {
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++)
            *gfx_at(s, x, y) = gfx_mix(theme->bg_top, theme->bg_bottom, x * 96 / s->w + y * 160 / s->h);
}

/* add a soft round glow of colour c, strength a (0..256) */
static void glow(struct surface *s, int cx, int cy, int r, uint32_t c, int a) {
    int64_t r2 = (int64_t)r * r;
    int y0 = cy - r < 0 ? 0 : cy - r, y1 = cy + r > s->h ? s->h : cy + r;
    int x0 = cx - r < 0 ? 0 : cx - r, x1 = cx + r > s->w ? s->w : cx + r;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            int64_t dx = x - cx, dy = y - cy, d2 = dx * dx + dy * dy;
            if (d2 >= r2) continue;
            int64_t f = 256 - d2 * 256 / r2;
            uint32_t *p = gfx_at(s, x, y);
            *p = gfx_mix(*p, c, (int)(f * f / 256 * a / 256));
        }
}

static void stars(struct surface *s, int n, int max_y) {
    for (int i = 0; i < n; i++) {
        int x = rnd() % s->w, y = rnd() % max_y, b = 60 + rnd() % 160;
        uint32_t *p = gfx_at(s, x, y);
        *p = gfx_mix(*p, 0xffffff, b);
        if (b > 200) {                               /* a few bright ones get a halo */
            if (x > 0) gfx_blend_px(s, x - 1, y, 0xffffff, b / 3);
            if (x < s->w - 1) gfx_blend_px(s, x + 1, y, 0xffffff, b / 3);
            if (y > 0) gfx_blend_px(s, x, y - 1, 0xffffff, b / 3);
            if (y < s->h - 1) gfx_blend_px(s, x, y + 1, 0xffffff, b / 3);
        }
    }
}

static void aurora(struct surface *s) {
    int W = s->w, H = s->h;
    base(s);
    stars(s, W * H / 2500, H * 2 / 3);
    uint32_t c1 = theme->pal[PAL_OK], c2 = theme->pal[PAL_ACCENT];
    for (int band = 0; band < 2; band++) {
        for (int x = 0; x < W; x++) {
            /* the curtain's lower edge wanders up and down along the screen */
            int edge = H * (band ? 44 : 36) / 100
                     + isin(x * 1024 / W + band * 300) * H / 9000
                     + isin(x * 2600 / W + 150 + band * 90) * H / 26000;
            int height = H * (band ? 22 : 30) / 100 + isin(x * 1700 / W + 400) * H / 14000;
            uint32_t col = gfx_mix(c1, c2, x * 256 / W);
            if (band) col = gfx_mix(c2, theme->pal[PAL_ERR], 80 + x * 100 / W);
            /* vertical rays: some columns brighter than others */
            int ray = 160 + isin(x * 9000 / W) / 12 + isin(x * 23000 / W + 77) / 20;
            for (int y = edge - height; y < edge + 30; y++) {
                if (y < 0 || y >= H) continue;
                int a;
                if (y >= edge) a = (edge + 30 - y) * 256 / 30;            /* sharp-ish bottom */
                else a = (y - (edge - height)) * 256 / height;            /* fades out upwards */
                a = a * a / 256 * ray / 256 * (band ? 60 : 110) / 256;
                uint32_t *p = gfx_at(s, x, y);
                *p = gfx_mix(*p, col, a);
            }
        }
    }
    /* a dark horizon at the bottom */
    for (int y = H * 80 / 100; y < H; y++)
        for (int x = 0; x < W; x++) {
            int ridge = H * 84 / 100 + isin(x * 3000 / W) * H / 40000 + isin(x * 7000 / W + 50) * H / 90000;
            if (y >= ridge) *gfx_at(s, x, y) = gfx_mix(theme->bg_bottom, 0x000000, 140);
        }
}

static void dunes(struct surface *s) {
    int W = s->w, H = s->h;
    base(s);
    glow(s, W * 70 / 100, H * 30 / 100, H * 45 / 100, theme->pal[PAL_WARN], 70);
    stars(s, W * H / 6000, H / 2);
    /* the moon */
    int mx = W * 70 / 100, my = H * 30 / 100, mr = H / 11;
    glow(s, mx, my, mr * 3, theme->pal[PAL_FG], 60);
    gfx_circle(s, mx, my, mr, gfx_mix(theme->pal[PAL_FG], 0xffffff, 120));
    /* hills, back to front, each a little darker */
    for (int layer = 0; layer < 4; layer++) {
        uint32_t c = gfx_mix(gfx_mix(theme->bg_top, theme->pal[PAL_ACCENT], 90 - layer * 20), 0x000000, 40 + layer * 50);
        int baseline = H * (58 + layer * 10) / 100;
        for (int x = 0; x < W; x++) {
            int top = baseline + isin(x * (700 + layer * 400) / W + layer * 211) * H / (11000 + layer * 2000)
                    + isin(x * (1900 + layer * 300) / W + layer * 97) * H / 40000;
            for (int y = top < 0 ? 0 : top; y < H; y++) {
                uint32_t col = gfx_mix(c, 0x000000, (y - top) * 120 / H);
                *gfx_at(s, x, y) = y == top ? gfx_mix(*gfx_at(s, x, y), col, 128) : col;
            }
        }
    }
}

static void glows(struct surface *s) {
    int W = s->w, H = s->h;
    base(s);
    glow(s, W * 4 / 5, H / 4, H * 3 / 5, theme->pal[PAL_ACCENT], 80);
    glow(s, W / 6, H * 5 / 6, H / 2, theme->pal[PAL_OK], 50);
    glow(s, W * 3 / 5, H + H / 10, H / 2, theme->pal[PAL_ERR], 40);
}

static void nebula(struct surface *s) {
    int W = s->w, H = s->h;
    base(s);
    for (int i = 0; i < 9; i++) {
        int x = rnd() % W, y = rnd() % H, r = H / 5 + rnd() % (H / 3);
        uint32_t c = theme->pal[i % 3 == 0 ? PAL_ACCENT : i % 3 == 1 ? PAL_ERR : PAL_OK];
        glow(s, x, y, r, c, 40 + rnd() % 40);
    }
    stars(s, W * H / 900, H);
}

void wallpaper_draw(struct surface *s, const char *style) {
    seed = 0x9e3779b9;
    if (!strcmp(style, "aurora")) aurora(s);
    else if (!strcmp(style, "dunes")) dunes(s);
    else if (!strcmp(style, "nebula")) nebula(s);
    else if (!strcmp(style, "plain")) base(s);
    else glows(s);
    for (int i = 0; i < s->w * s->h; i++) s->px[i] = grain(s->px[i]);
    s->damage = (struct rect){ 0, 0, s->w, s->h };
}
