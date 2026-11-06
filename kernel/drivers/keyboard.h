#pragma once
#include <stdbool.h>

#include <dream/abi.h>   /* KEY_UP ... KEY_F1: keys that aren't characters */

void kbd_init(void);
int  kbd_poll(void);    /* next key, or -1 if none waiting */
bool kbd_alt(void);     /* is alt held down right now? */
