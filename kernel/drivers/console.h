#pragma once
#include <stdbool.h>
#include "gfx/gfx.h"

void con_init(void);                              /* text mode: the whole screen */
void con_attach(struct surface *s, bool with_bar); /* draw into this surface instead */
void con_flush(void);                             /* show what changed (text mode) */
void con_putc(char c);
void con_write(const char *s);
void con_clear(void);
void con_redraw(void);          /* repaint everything, e.g. after a theme change */
void con_cursor(bool visible);
void con_bar(void);             /* repaint the status bar (clock, theme) */
int  con_cols(void);
int  con_rows(void);
int  con_top(void);             /* first pixel row below the status bar */

/* full-screen apps */
#include <stdint.h>
#define CON_PANEL 0x80          /* OR into a colour: draw the cell on a solid panel */
void con_goto(int col, int row);
void con_put(int col, int row, char c, uint8_t color);
void con_save(void);
void con_restore(void);
