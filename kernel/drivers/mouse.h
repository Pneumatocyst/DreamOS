#pragma once
#include "kernel.h"

enum { MOUSE_LEFT = 1, MOUSE_RIGHT = 2, MOUSE_MIDDLE = 4 };

/* One movement or button change. x/y is where the pointer is now (pixels);
   dx/dy is how far it moved; wheel is +/- clicks of the scroll wheel. */
struct mouse_event {
    int x, y, dx, dy, wheel;
    uint8_t buttons;
};

void mouse_init(void);              /* call with interrupts off */
bool mouse_present(void);
bool mouse_has_wheel(void);
bool mouse_poll(struct mouse_event *e);    /* next event, false if none */
void mouse_where(int *x, int *y);
