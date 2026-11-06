/* The drawing that makes the desktop look modern: see-through rounded shapes,
 * soft shadows, frosted-glass blur and smooth text.
 *
 * Everything here is integer maths. Distances and coverage are kept in 1/256ths
 * of a pixel, so "256" means "one whole pixel" or "fully covered". The kernel is
 * built without floating point (it doesn't save FPU state when switching tasks). */
#include "gfx/gfx.h"

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

static uint32_t isqrt(uint64_t n) {
    if (n < 2) return (uint32_t)n;
    uint64_t x = 1ull << ((64 - __builtin_clzll(n) + 1) / 2);
    for (;;) {
        uint64_t y = (x + n / x) / 2;
        if (y >= x) return (uint32_t)x;
        x = y;
    }
}

/* Clip to the surface's clip rectangle and remember the damage. */
static bool clip(struct surface *s, struct rect in, struct rect *out) {
    if (!rect_intersect(in, s->clip, out)) return false;
    s->damage = rect_union(s->damage, *out);
    return true;
}

static inline void put(uint32_t *p, uint32_t c, int a) {
    if (a >= 256) *p = c;
    else if (a > 0) *p = gfx_mix(*p, c, a);
}

/* How much of pixel (i, j) is inside a w x h rectangle with corners of radius r,
   all relative to the rectangle's top-left. 0 = outside ... 256 = inside. */
static int rr_cover(int i, int j, int w, int h, int r) {
    if (i < 0 || j < 0 || i >= w || j >= h) return 0;
    int dx = i < r ? r * 256 - (i * 256 + 128) : (i >= w - r ? (i * 256 + 128) - (w - r) * 256 : 0);
    int dy = j < r ? r * 256 - (j * 256 + 128) : (j >= h - r ? (j * 256 + 128) - (h - r) * 256 : 0);
    if (dx <= 0 || dy <= 0) return 256;              /* along a straight edge: fully in */
    int dist = (int)isqrt((uint64_t)dx * dx + (uint64_t)dy * dy);
    int cov = r * 256 - dist + 128;
    return cov < 0 ? 0 : cov > 256 ? 256 : cov;
}

static int fix_radius(int r, int w, int h) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    return r < 0 ? 0 : r;
}

void gfx_round_ex(struct surface *s, int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom, int alpha) {
    struct rect c;
    if (w <= 0 || h <= 0 || !clip(s, (struct rect){ x, y, w, h }, &c)) return;
    r = fix_radius(r, w, h);
    for (int py = c.y; py < c.y + c.h; py++) {
        int j = py - y;
        uint32_t col = top == bottom ? top : gfx_mix(top, bottom, h > 1 ? j * 256 / (h - 1) : 0);
        bool band = j < r || j >= h - r;              /* a row that goes through a corner */
        uint32_t *row = gfx_at(s, 0, py);
        for (int px = c.x; px < c.x + c.w; px++) {
            int i = px - x;
            int cov = (band && (i < r || i >= w - r)) ? rr_cover(i, j, w, h, r) : 256;
            put(&row[px], col, cov * alpha / 256);
        }
    }
}

void gfx_round_a(struct surface *s, int x, int y, int w, int h, int r, uint32_t c, int alpha) {
    gfx_round_ex(s, x, y, w, h, r, c, c, alpha);
}

void gfx_ring(struct surface *s, int x, int y, int w, int h, int r, uint32_t c, int alpha) {
    struct rect cl;
    if (w <= 2 || h <= 2 || !clip(s, (struct rect){ x, y, w, h }, &cl)) return;
    r = fix_radius(r, w, h);
    if (r < 1) r = 1;
    for (int py = cl.y; py < cl.y + cl.h; py++) {
        int j = py - y;
        bool band = j < r || j >= h - r;
        uint32_t *row = gfx_at(s, 0, py);
        for (int px = cl.x; px < cl.x + cl.w; px++) {
            int i = px - x;
            if (!band && i > 0 && i < w - 1) { px = x + w - 2; continue; }   /* jump across the middle */
            int cov = rr_cover(i, j, w, h, r) - rr_cover(i - 1, j - 1, w - 2, h - 2, r - 1);
            if (cov > 0) put(&row[px], c, cov * alpha / 256);
        }
    }
}

void gfx_shadow(struct surface *s, struct rect box, int r, int spread, int dy, int alpha) {
    struct rect area = { box.x - spread, box.y - spread + dy, box.w + 2 * spread, box.h + 2 * spread }, c;
    if (spread <= 0 || !clip(s, area, &c)) return;
    r = fix_radius(r, box.w, box.h);
    /* the shadow's shape: the box, nudged down by dy (light comes from above) */
    int64_t cx2 = box.x * 2 + box.w, cy2 = (box.y + dy) * 2 + box.h;      /* centre, in half pixels */
    int64_t hx = (box.w / 2 - r) * 256, hy = (box.h / 2 - r) * 256;
    for (int py = c.y; py < c.y + c.h; py++) {
        uint32_t *row = gfx_at(s, 0, py);
        bool inside_rows = py >= box.y + r && py < box.y + box.h - r;
        for (int px = c.x; px < c.x + c.w; px++) {
            /* under the box itself: it gets painted over anyway, skip the maths */
            if (inside_rows && px >= box.x && px < box.x + box.w) { px = box.x + box.w - 1; continue; }
            int64_t qx = ((int64_t)(px * 2 + 1) - cx2) * 128, qy = ((int64_t)(py * 2 + 1) - cy2) * 128;
            if (qx < 0) qx = -qx;
            if (qy < 0) qy = -qy;
            qx -= hx; qy -= hy;
            if (qx < 0) qx = 0;
            if (qy < 0) qy = 0;
            int64_t d = (qx && qy ? (int64_t)isqrt((uint64_t)(qx * qx + qy * qy)) : qx + qy) - r * 256;
            if (d >= spread * 256) continue;
            int t = d <= 0 ? 256 : (int)(256 - d / spread);              /* 256 at the edge ... 0 far out */
            int a = t * t / 256 * alpha / 256;
            if (a > 0) row[px] = gfx_mix(row[px], 0x000000, a);
        }
    }
}

/* One box blur pass along a line of n pixels (each `step` apart), edges clamped. */
static void blur_line(uint32_t *p, int n, int step, int radius, uint32_t *tmp) {
    for (int i = 0; i < n; i++) tmp[i] = p[i * step];
    int k = 2 * radius + 1;
    uint32_t inv = 65536 / k;
    uint32_t sr = 0, sg = 0, sb = 0;
    for (int i = -radius; i <= radius; i++) {
        uint32_t c = tmp[imin(imax(i, 0), n - 1)];
        sr += c >> 16 & 255; sg += c >> 8 & 255; sb += c & 255;
    }
    for (int i = 0; i < n; i++) {
        p[i * step] = ((sr * inv >> 16) << 16) | ((sg * inv >> 16) << 8) | (sb * inv >> 16);
        uint32_t out = tmp[imax(i - radius, 0)], in = tmp[imin(i + radius + 1, n - 1)];
        sr += (in >> 16 & 255) - (out >> 16 & 255);
        sg += (in >> 8 & 255) - (out >> 8 & 255);
        sb += (in & 255) - (out & 255);
    }
}

void gfx_blur(struct surface *s, struct rect area, int radius) {
    static uint32_t tmp[4096];
    struct rect c;
    if (radius < 1 || !rect_intersect(area, (struct rect){ 0, 0, s->w, s->h }, &c)) return;
    if (c.w > 4096 || c.h > 4096) return;
    for (int pass = 0; pass < 3; pass++) {
        for (int y = c.y; y < c.y + c.h; y++) blur_line(gfx_at(s, c.x, y), c.w, 1, radius, tmp);
        for (int x = c.x; x < c.x + c.w; x++) blur_line(gfx_at(s, x, c.y), c.h, s->stride, radius, tmp);
    }
    s->damage = rect_union(s->damage, c);
}

/* ---- smooth text ------------------------------------------------------------------------ */

static const struct uiglyph *glyph(const struct uifont *f, unsigned char ch) {
    if (ch < 32 || ch > 126) ch = '?';
    const struct uiglyph *g = &f->g[ch - 32];
    if (!g->adv) g = &f->g[0];                        /* not in this font: a space */
    return g;
}

int gfx_utext(struct surface *s, const struct uifont *f, int x, int y, const char *str, uint32_t c) {
    int pen = x * 16;                                 /* advances are in 1/16 px */
    for (; *str; str++) {
        const struct uiglyph *g = glyph(f, (unsigned char)*str);
        int gx = (pen + 8) / 16 + g->xoff, gy = y + g->yoff;
        struct rect r;
        if (g->w && clip(s, (struct rect){ gx, gy, g->w, g->h }, &r)) {
            const uint8_t *bits = f->bits + g->off;
            for (int py = r.y; py < r.y + r.h; py++) {
                const uint8_t *src = bits + (py - gy) * g->w;
                uint32_t *dst = gfx_at(s, 0, py);
                for (int px = r.x; px < r.x + r.w; px++) {
                    int a = src[px - gx];
                    if (a) put(&dst[px], c, a + (a >> 7));   /* 255 -> 256 */
                }
            }
        }
        pen += g->adv;
    }
    return (pen + 8) / 16;
}

int gfx_utext_width(const struct uifont *f, const char *str) {
    int pen = 0;
    for (; *str; str++) pen += glyph(f, (unsigned char)*str)->adv;
    return (pen + 8) / 16;
}

void gfx_utext_center(struct surface *s, const struct uifont *f, int cx, int y, const char *str, uint32_t c) {
    gfx_utext(s, f, cx - gfx_utext_width(f, str) / 2, y, str, c);
}

int gfx_utext_fit(struct surface *s, const struct uifont *f, int x, int y, const char *str, uint32_t c, int maxw) {
    if (gfx_utext_width(f, str) <= maxw) return gfx_utext(s, f, x, y, str, c);
    char buf[256];
    int n = 0, room = maxw - gfx_utext_width(f, "...");
    int pen = 0;
    while (str[n] && n < (int)sizeof buf - 4) {
        int a = glyph(f, (unsigned char)str[n])->adv;
        if ((pen + a) / 16 > room) break;
        pen += a;
        buf[n] = str[n];
        n++;
    }
    while (n > 0 && buf[n - 1] == ' ') n--;
    memcpy(buf + n, "...", 4);
    return gfx_utext(s, f, x, y, buf, c);
}
