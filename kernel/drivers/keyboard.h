#pragma once

enum { KEY_UP = 0x80, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
       KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_DEL };

void kbd_init(void);
int  kbd_poll(void);    /* next key, or -1 if none waiting */
