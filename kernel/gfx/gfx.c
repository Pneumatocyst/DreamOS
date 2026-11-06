#include "gfx/gfx.h"
#include "mm/heap.h"

/* ---- rectangles ------------------------------------------------------------------ */

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

bool rect_empty(struct rect r) { return r.w <= 0 || r.h <= 0; }

bool rect_intersect(struct rect a, struct rect b, struct rect *out) {
    int x0 = imax(a.x, b.x), y0 = imax(a.y, b.y);
    int x1 = imin(a.x + a.w, b.x + b.w), y1 = imin(a.y + a.h, b.y + b.h);
    bool hit = x1 > x0 && y1 > y0;
    if (out) *out = hit ? (struct rect){ x0, y0, x1 - x0, y1 - y0 } : (struct rect){ 0, 0, 0, 0 };
    return hit;
}

struct rect rect_union(struct rect a, struct rect b) {
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    int x0 = imin(a.x, b.x), y0 = imin(a.y, b.y);
    int x1 = imax(a.x + a.w, b.x + b.w), y1 = imax(a.y + a.h, b.y + b.h);
    return (struct rect){ x0, y0, x1 - x0, y1 - y0 };
}

bool rect_contains(struct rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

/* ---- surfaces -------------------------------------------------------------------- */

void gfx_wrap(struct surface *s, uint32_t *px, int w, int h, int stride) {
    s->px = px;
    s->w = w;
    s->h = h;
    s->stride = stride;
    s->clip = (struct rect){ 0, 0, w, h };
    s->damage = (struct rect){ 0, 0, 0, 0 };
}

struct surface *surface_new(int w, int h) {
    struct surface *s = kzalloc(sizeof *s);
    uint32_t *px = s ? kmalloc((size_t)w * h * 4) : NULL;
    if (!px) { kfree(s); return NULL; }
    gfx_wrap(s, px, w, h, w);
    return s;
}

void surface_free(struct surface *s) {
    if (!s) return;
    kfree(s->px);
    kfree(s);
}

void gfx_clip(struct surface *s, const struct rect *r) {
    struct rect full = { 0, 0, s->w, s->h };
    if (!r) s->clip = full;
    else rect_intersect(*r, full, &s->clip);
}

void gfx_damage(struct surface *s, int x, int y, int w, int h) {
    struct rect r;
    if (rect_intersect((struct rect){ x, y, w, h }, (struct rect){ 0, 0, s->w, s->h }, &r))
        s->damage = rect_union(s->damage, r);
}

/* Clip a rectangle to the surface's clip; record damage. false = nothing visible. */
static bool clip_to(struct surface *s, int *x, int *y, int *w, int *h) {
    struct rect r;
    if (!rect_intersect((struct rect){ *x, *y, *w, *h }, s->clip, &r)) return false;
    *x = r.x; *y = r.y; *w = r.w; *h = r.h;
    s->damage = rect_union(s->damage, r);
    return true;
}

static inline bool in_clip(struct surface *s, int x, int y) { return rect_contains(s->clip, x, y); }

/* Fill n pixels with one value, using the CPU's "repeat store" instruction. */
static inline void fill32(uint32_t *p, uint32_t v, size_t n) {
    asm volatile("rep stosl" : "+D"(p), "+c"(n) : "a"(v) : "memory");
}

/* ---- colours --------------------------------------------------------------------- */

uint32_t gfx_mix(uint32_t a, uint32_t b, int t) {
    if (t <= 0) return a;
    if (t >= 256) return b;
    uint32_t rb = (((a & 0xff00ff) * (256 - t) + (b & 0xff00ff) * t) >> 8) & 0xff00ff;
    uint32_t g  = (((a & 0x00ff00) * (256 - t) + (b & 0x00ff00) * t) >> 8) & 0x00ff00;
    return rb | g;
}

uint32_t gfx_gradient_at(int y, int h, uint32_t top, uint32_t bottom) {
    return gfx_mix(top, bottom, h > 1 ? y * 256 / (h - 1) : 0);
}

/* ---- shapes ---------------------------------------------------------------------- */

void gfx_fill(struct surface *s, int x, int y, int w, int h, uint32_t c) {
    if (!clip_to(s, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) fill32(gfx_at(s, x, y + j), c, w);
}

void gfx_blend(struct surface *s, int x, int y, int w, int h, uint32_t c, int alpha) {
    if (!clip_to(s, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) {
        uint32_t *p = gfx_at(s, x, y + j);
        for (int i = 0; i < w; i++) p[i] = gfx_mix(p[i], c, alpha);
    }
}

void gfx_blend_px(struct surface *s, int x, int y, uint32_t c, int alpha) {
    if (!in_clip(s, x, y)) return;
    uint32_t *p = gfx_at(s, x, y);
    *p = gfx_mix(*p, c, alpha);
}

/* Vertical gradient. Colours follow the surface's full height, so filling
   any part of it gives the same colour at the same row. */
void gfx_gradient(struct surface *s, int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    if (!clip_to(s, &x, &y, &w, &h)) return;
    for (int j = 0; j < h; j++) fill32(gfx_at(s, x, y + j), gfx_gradient_at(y + j, s->h, top, bottom), w);
}

void gfx_rect(struct surface *s, int x, int y, int w, int h, uint32_t c) {
    gfx_fill(s, x, y, w, 1, c);
    gfx_fill(s, x, y + h - 1, w, 1, c);
    gfx_fill(s, x, y, 1, h, c);
    gfx_fill(s, x + w - 1, y, 1, h, c);
}

static uint64_t isqrt(uint64_t n) {
    if (n < 2) return n;
    uint64_t x = 1ull << ((64 - __builtin_clzll(n) + 1) / 2);   /* a guess above the root */
    for (;;) {
        uint64_t y = (x + n / x) / 2;
        if (y >= x) return x;
        x = y;
    }
}

static void span(struct surface *s, int x0, int x1, int y, uint32_t c) {   /* [x0, x1) */
    if (y < s->clip.y || y >= s->clip.y + s->clip.h) return;
    x0 = imax(x0, s->clip.x);
    x1 = imin(x1, s->clip.x + s->clip.w);
    if (x1 > x0) fill32(gfx_at(s, x0, y), c, x1 - x0);
}

/* How far in from the edge a rounded corner's row starts (1/256 px), dy = rows from the edge. */
int gfx_corner_inset(int r, int dy) {
    int64_t R = (int64_t)r * 256, Y = R - (int64_t)dy * 256 - 128;
    if (Y < 0) Y = 0;
    return (int)(R - (int64_t)isqrt(R * R - Y * Y));
}

void gfx_round(struct surface *s, int x, int y, int w, int h, int r, uint32_t c) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    int cx = x, cy = y, cw = w, ch = h;
    if (!clip_to(s, &cx, &cy, &cw, &ch)) return;
    for (int j = 0; j < h; j++) {
        int dy = j < r ? j : (j >= h - r ? h - 1 - j : -1);
        int inset = dy >= 0 ? gfx_corner_inset(r, dy) : 0;
        int full = inset >> 8, frac = inset & 255;
        span(s, x + full + (frac ? 1 : 0), x + w - full - (frac ? 1 : 0), y + j, c);
        if (frac) {   /* soften the edge pixel by how much of it is covered */
            gfx_blend_px(s, x + full, y + j, c, 256 - frac);
            gfx_blend_px(s, x + w - 1 - full, y + j, c, 256 - frac);
        }
    }
}

void gfx_circle(struct surface *s, int cx, int cy, int r, uint32_t c) {
    int bx = cx - r - 1, by = cy - r - 1, bw = 2 * r + 3, bh = 2 * r + 3;
    if (!clip_to(s, &bx, &by, &bw, &bh)) return;
    int64_t R = (int64_t)r * 256;
    for (int dy = -r; dy <= r; dy++) {
        int64_t Y = (int64_t)dy * 256;
        if (Y * Y > R * R) continue;
        int64_t hw = isqrt(R * R - Y * Y);
        int64_t xl = (int64_t)cx * 256 + 128 - hw, xr = (int64_t)cx * 256 + 128 + hw;
        int l = xl >> 8, rr = xr >> 8, y = cy + dy;
        if (l == rr) { gfx_blend_px(s, l, y, c, (int)(xr - xl)); continue; }
        gfx_blend_px(s, l, y, c, 256 - (xl & 255));
        span(s, l + 1, rr, y, c);
        gfx_blend_px(s, rr, y, c, xr & 255);
    }
}

/* A thick, smooth line: every pixel near the segment is shaded by how close
   its centre is to the line (all in 1/256-pixel fixed point; no floats in here). */
void gfx_line(struct surface *s, int x0, int y0, int x1, int y1, int thick, uint32_t c) {
    if (thick < 1) thick = 1;
    int pad = thick / 2 + 2;
    int bx = imin(x0, x1) - pad, by = imin(y0, y1) - pad;
    int bw = (x0 > x1 ? x0 - x1 : x1 - x0) + 2 * pad + 1, bh = (y0 > y1 ? y0 - y1 : y1 - y0) + 2 * pad + 1;
    if (!clip_to(s, &bx, &by, &bw, &bh)) return;

    int64_t ax = (int64_t)x0 * 256 + 128, ay = (int64_t)y0 * 256 + 128;
    int64_t dx = (int64_t)(x1 - x0) * 256, dy = (int64_t)(y1 - y0) * 256;
    int64_t len2 = dx * dx + dy * dy, half = (int64_t)thick * 128;
    for (int y = by; y < by + bh; y++) {
        for (int x = bx; x < bx + bw; x++) {
            int64_t px = (int64_t)x * 256 + 128 - ax, py = (int64_t)y * 256 + 128 - ay;
            int64_t t = px * dx + py * dy, qx = px, qy = py;
            if (len2 && t >= len2) { qx = px - dx; qy = py - dy; }
            else if (len2 && t > 0) { qx = px - dx * t / len2; qy = py - dy * t / len2; }
            int64_t cov = half + 128 - (int64_t)isqrt(qx * qx + qy * qy);
            if (cov <= 0) continue;
            uint32_t *p = gfx_at(s, x, y);
            *p = gfx_mix(*p, c, cov > 256 ? 256 : (int)cov);
        }
    }
}

/* ---- text ------------------------------------------------------------------------ */

struct psf2 {
    uint32_t magic, version, header_size, flags;
    uint32_t glyphs, bytes_per_glyph, height, width;
};
extern const uint8_t font_psf[];
static const struct psf2 *const font = (const struct psf2 *)font_psf;

bool gfx_font_ok(void) { return font->magic == 0x864ab572; }

static const uint8_t *glyph_bits(unsigned char ch) {
    if (ch >= font->glyphs) ch = '?';
    return font_psf + font->header_size + ch * font->bytes_per_glyph;
}

void gfx_glyph(struct surface *s, int x, int y, unsigned char ch, uint32_t fg, int64_t bg) {
    int cx = x, cy = y, cw = FONT_W, chh = FONT_H;
    if (!clip_to(s, &cx, &cy, &cw, &chh)) return;
    const uint8_t *g = glyph_bits(ch);
    for (int row = 0; row < FONT_H; row++) {
        int py = y + row;
        if (py < cy || py >= cy + chh) continue;
        for (int col = 0; col < FONT_W; col++) {
            int px = x + col;
            if (px < cx || px >= cx + cw) continue;
            if (g[row] & (0x80 >> col)) *gfx_at(s, px, py) = fg;
            else if (bg >= 0)           *gfx_at(s, px, py) = (uint32_t)bg;
        }
    }
}

int gfx_text(struct surface *s, int x, int y, const char *str, uint32_t fg, int64_t bg) {
    for (; *str; str++, x += FONT_W) gfx_glyph(s, x, y, *str, fg, bg);
    return x;
}

void gfx_text_scaled(struct surface *s, int x, int y, const char *str, int scale, uint32_t c, int alpha) {
    for (; *str; str++, x += FONT_W * scale) {
        const uint8_t *g = glyph_bits(*str);
        for (int row = 0; row < FONT_H; row++)
            for (int col = 0; col < FONT_W; col++)
                if (g[row] & (0x80 >> col))
                    gfx_blend(s, x + col * scale, y + row * scale, scale, scale, c, alpha);
    }
}

int gfx_text_width(const char *str) { return (int)strlen(str) * FONT_W; }

/* ---- copying --------------------------------------------------------------------- */

void gfx_blit(struct surface *dst, int dx, int dy, struct surface *src, int sx, int sy, int w, int h) {
    if (sx < 0) { dx -= sx; w += sx; sx = 0; }
    if (sy < 0) { dy -= sy; h += sy; sy = 0; }
    if (sx + w > src->w) w = src->w - sx;
    if (sy + h > src->h) h = src->h - sy;
    int x = dx, y = dy;
    if (w <= 0 || h <= 0 || !clip_to(dst, &x, &y, &w, &h)) return;
    sx += x - dx;
    sy += y - dy;
    for (int j = 0; j < h; j++) memcpy(gfx_at(dst, x, y + j), gfx_at(src, sx, sy + j), (size_t)w * 4);
}

static const char *arrow[CURSOR_H] = {     /* X = outline, . = fill, space = see-through */
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ",
    "X....X      ", "X.....X     ", "X......X    ", "X.......X   ", "X........X  ",
    "X.........X ", "X..........X", "X......XXXXX", "X...X..X    ", "X..XX..X    ",
    "X.X  X..X   ", "XX   X..X   ", "X     X..X  ", "      XXXX  ",
};

void gfx_cursor(struct surface *s, int x, int y) {
    for (int j = 0; j < CURSOR_H; j++)
        for (int i = 0; i < CURSOR_W; i++) {
            char c = arrow[j][i];
            if (c == ' ' || !in_clip(s, x + i, y + j)) continue;
            *gfx_at(s, x + i, y + j) = c == 'X' ? 0x101014 : 0xf4f4f8;
        }
    gfx_damage(s, x, y, CURSOR_W, CURSOR_H);
}
