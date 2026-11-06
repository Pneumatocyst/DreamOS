#pragma once
#include "kernel.h"

/* gfx: drawing onto "surfaces" — blocks of memory holding pixels as 0xRRGGBB.
 *
 * The screen's back buffer is a surface, and so is every window. Every drawing
 * call respects the surface's clip rectangle (nothing is drawn outside it) and
 * records the area it touched in `damage`, so whoever shows the surface knows
 * which part changed and only copies that. */

#define FONT_W 8
#define FONT_H 16

struct rect { int x, y, w, h; };

struct surface {
    uint32_t *px;
    int w, h, stride;          /* stride: pixels from one row to the next */
    struct rect clip;
    struct rect damage;
};

bool rect_empty(struct rect r);
bool rect_intersect(struct rect a, struct rect b, struct rect *out);   /* out may be NULL */
struct rect rect_union(struct rect a, struct rect b);
bool rect_contains(struct rect r, int x, int y);

struct surface *surface_new(int w, int h);
void surface_free(struct surface *s);
void gfx_wrap(struct surface *s, uint32_t *px, int w, int h, int stride);
void gfx_clip(struct surface *s, const struct rect *r);   /* NULL = the whole surface */
void gfx_damage(struct surface *s, int x, int y, int w, int h);

static inline uint32_t *gfx_at(struct surface *s, int x, int y) { return &s->px[y * s->stride + x]; }

/* colours */
uint32_t gfx_mix(uint32_t a, uint32_t b, int t);                  /* t: 0 = a ... 256 = b */
uint32_t gfx_gradient_at(int y, int h, uint32_t top, uint32_t bottom);

/* shapes (alpha: 0 = invisible ... 256 = solid) */
void gfx_fill(struct surface *s, int x, int y, int w, int h, uint32_t c);
void gfx_blend(struct surface *s, int x, int y, int w, int h, uint32_t c, int alpha);
void gfx_blend_px(struct surface *s, int x, int y, uint32_t c, int alpha);
void gfx_gradient(struct surface *s, int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void gfx_rect(struct surface *s, int x, int y, int w, int h, uint32_t c);   /* outline */
void gfx_round(struct surface *s, int x, int y, int w, int h, int r, uint32_t c);
void gfx_circle(struct surface *s, int cx, int cy, int r, uint32_t c);
void gfx_line(struct surface *s, int x0, int y0, int x1, int y1, int thick, uint32_t c);
int  gfx_corner_inset(int r, int dy);   /* rounded corners: inset of row dy, in 1/256 px */

/* text (bg < 0 = transparent) */
bool gfx_font_ok(void);
void gfx_glyph(struct surface *s, int x, int y, unsigned char ch, uint32_t fg, int64_t bg);
int  gfx_text(struct surface *s, int x, int y, const char *str, uint32_t fg, int64_t bg);
void gfx_text_scaled(struct surface *s, int x, int y, const char *str, int scale, uint32_t c, int alpha);
int  gfx_text_width(const char *str);

/* ---- the modern bits (gfx/ui.c) ------------------------------------------------------- */

/* Rounded rectangles that can be see-through and shaded top to bottom. */
void gfx_round_ex(struct surface *s, int x, int y, int w, int h, int r, uint32_t top, uint32_t bottom, int alpha);
void gfx_round_a(struct surface *s, int x, int y, int w, int h, int r, uint32_t c, int alpha);
void gfx_ring(struct surface *s, int x, int y, int w, int h, int r, uint32_t c, int alpha);   /* 1px outline */
/* A soft shadow around a rounded rectangle: darkest at the edge, fading over `spread` px. */
void gfx_shadow(struct surface *s, struct rect box, int r, int spread, int dy, int alpha);
/* Blur a part of a surface in place (three box blurs ~ a gaussian). For frosted glass. */
void gfx_blur(struct surface *s, struct rect area, int radius);

/* Smooth (anti-aliased) proportional text, from Inter rendered at fixed sizes by
   tools/mkfont.py. y is the top of the line; the functions return the x where
   the text ended. */
struct uiglyph { uint8_t w, h; int8_t xoff, yoff; uint16_t adv; uint32_t off; };
struct uifont  { int16_t height, ascent; const struct uiglyph *g; const uint8_t *bits; };
extern const struct uifont font_ui, font_bold, font_small, font_title, font_big, font_display;

int  gfx_utext(struct surface *s, const struct uifont *f, int x, int y, const char *str, uint32_t c);
int  gfx_utext_width(const struct uifont *f, const char *str);
void gfx_utext_center(struct surface *s, const struct uifont *f, int cx, int y, const char *str, uint32_t c);
/* Like gfx_utext, but cut short with "..." if it would be wider than maxw. */
int  gfx_utext_fit(struct surface *s, const struct uifont *f, int x, int y, const char *str, uint32_t c, int maxw);

/* copying */
void gfx_blit(struct surface *dst, int dx, int dy, struct surface *src, int sx, int sy, int w, int h);
void gfx_cursor(struct surface *s, int x, int y);   /* the mouse pointer */
#define CURSOR_W 12
#define CURSOR_H 19
