#pragma once
#include <stdint.h>

/* Colours are 0xRRGGBB. Text is stored by palette slot, not colour, so
   switching theme recolours everything already on screen. */
enum { PAL_FG, PAL_DIM, PAL_ACCENT, PAL_OK, PAL_WARN, PAL_ERR, PAL_COUNT };

struct theme {
    const char *name;
    uint32_t bg_top, bg_bottom;   /* vertical gradient behind the text */
    uint32_t bar;                 /* status bar */
    uint32_t pal[PAL_COUNT];
};

extern const struct theme themes[];
extern const int theme_count;
extern const struct theme *theme;   /* the active one */

int theme_set(const char *name);    /* 0 on success */
