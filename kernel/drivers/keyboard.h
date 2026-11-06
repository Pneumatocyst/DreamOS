#pragma once
#include <stdbool.h>

/* Keys that aren't characters get codes from 0x80 up (ASCII stops at 0x7f). */
enum { KEY_UP = 0x80, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
       KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_DEL,
       KEY_SUPER,                    /* the "windows" key: opens the launcher */
       KEY_ALTTAB,                   /* alt+tab: next window */
       KEY_ALTF4,                    /* alt+f4: close the window */
       KEY_F1 = 0x90 };              /* F1..F12 = KEY_F1 + 0..11 */

void kbd_init(void);
int  kbd_poll(void);
bool kbd_alt(void);     /* is alt held down right now? */    /* next key, or -1 if none waiting */
