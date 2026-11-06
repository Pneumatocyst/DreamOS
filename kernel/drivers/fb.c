#include "drivers/fb.h"
#include "mm/heap.h"
#include <limine.h>

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request fb_req = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0,
};

struct fb_info fb;
static uint32_t *hw;              /* video memory */
static uint64_t pitch;            /* bytes per row of video memory */
static uint8_t rs, gs, bs;        /* where R, G and B live inside a pixel */
static bool standard;             /* pixels are 0x00RRGGBB, same as our surfaces */
static struct surface screen;

bool fb_init(void) {
    if (!fb_req.response || fb_req.response->framebuffer_count < 1) return false;
    struct limine_framebuffer *f = fb_req.response->framebuffers[0];
    if (f->bpp != 32 || !gfx_font_ok()) return false;
    hw = f->address;
    pitch = f->pitch;
    fb.width = f->width;
    fb.height = f->height;
    rs = f->red_mask_shift; gs = f->green_mask_shift; bs = f->blue_mask_shift;
    standard = rs == 16 && gs == 8 && bs == 0;
    gfx_wrap(&screen, hw, fb.width, fb.height, pitch / 4);   /* until we have a back buffer */
    return true;
}

void fb_backbuffer(void) {
    uint32_t *bb = kmalloc((size_t)fb.width * fb.height * 4);
    if (!bb) return;                                          /* keep drawing straight to the screen */
    for (uint32_t y = 0; y < fb.height; y++)
        memcpy(bb + y * fb.width, (uint8_t *)hw + y * pitch, fb.width * 4);
    gfx_wrap(&screen, bb, fb.width, fb.height, fb.width);
}

struct surface *fb_screen(void) { return &screen; }

static inline uint32_t native(uint32_t c) {
    return ((c >> 16) & 0xff) << rs | ((c >> 8) & 0xff) << gs | (c & 0xff) << bs;
}

void fb_present_rect(struct rect r) {
    if (screen.px == hw) return;                              /* no back buffer: already on screen */
    if (!rect_intersect(r, (struct rect){ 0, 0, fb.width, fb.height }, &r)) return;
    for (int j = 0; j < r.h; j++) {
        uint32_t *dst = (uint32_t *)((uint8_t *)hw + (uint64_t)(r.y + j) * pitch) + r.x;
        uint32_t *src = gfx_at(&screen, r.x, r.y + j);
        if (standard) memcpy(dst, src, (size_t)r.w * 4);
        else for (int i = 0; i < r.w; i++) dst[i] = native(src[i]);
    }
}

void fb_present(void) {
    if (!rect_empty(screen.damage)) fb_present_rect(screen.damage);
    screen.damage = (struct rect){ 0, 0, 0, 0 };
}

void fb_present_all(void) {
    fb_present_rect((struct rect){ 0, 0, fb.width, fb.height });
    screen.damage = (struct rect){ 0, 0, 0, 0 };
}
