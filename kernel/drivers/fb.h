#pragma once
#include "gfx/gfx.h"

/* The framebuffer Limine handed us: real video memory, where each pixel is a colour.
 *
 * Double buffering: once memory allocation works, everything draws into a copy of
 * the screen in ordinary RAM (the "back buffer"). fb_present() then copies just the
 * changed part to video memory in one go, so you never see a half-drawn frame. */
struct fb_info { uint32_t width, height; };
extern struct fb_info fb;

bool fb_init(void);
void fb_backbuffer(void);          /* switch to double buffering (needs kmalloc) */
struct surface *fb_screen(void);   /* what to draw on */

void fb_present(void);             /* copy the damaged area to the real screen */
void fb_present_rect(struct rect r);
void fb_present_all(void);
