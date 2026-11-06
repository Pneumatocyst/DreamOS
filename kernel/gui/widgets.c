/* Little pieces of interface every app draws the same way. */
#include "gui/wm.h"

void ui_button(struct surface *s, struct rect r, const char *label, bool hot, bool primary) {
    uint32_t bg = primary ? ui(UI_ACCENT) : (hot ? ui(UI_HOVER) : ui(UI_RAISED));
    if (primary && hot) bg = gfx_mix(bg, 0xffffff, 40);
    gfx_round(s, r.x, r.y, r.w, r.h, r.h / 2 > 9 ? 9 : r.h / 2, bg);
    uint32_t fg = primary ? ui(UI_ON_ACCENT) : ui(UI_TEXT);
    const struct uifont *f = primary ? &font_bold : &font_ui;
    gfx_utext_center(s, f, r.x + r.w / 2, r.y + (r.h - f->height) / 2, label, fg);
}

void ui_icon_button(struct surface *s, struct rect r, int icon, bool hot, bool active) {
    if (active) gfx_round(s, r.x, r.y, r.w, r.h, 8, ui(UI_SEL));
    else if (hot) gfx_round(s, r.x, r.y, r.w, r.h, 8, ui(UI_HOVER));
    int g = r.h - 14 < 18 ? r.h - 14 : 18;
    icon_glyph(s, icon, r.x + (r.w - g) / 2, r.y + (r.h - g) / 2, g, active ? ui(UI_TEXT) : (hot ? ui(UI_TEXT) : ui(UI_DIM)));
}

void ui_field(struct surface *s, struct rect r, const char *text, const char *placeholder, bool focused, bool secret) {
    gfx_round(s, r.x, r.y, r.w, r.h, 8, gfx_mix(ui(UI_BG), 0x000000, 50));
    gfx_ring(s, r.x, r.y, r.w, r.h, 8, focused ? ui(UI_ACCENT) : ui(UI_LINE), focused ? 256 : 200);
    int ty = r.y + (r.h - font_ui.height) / 2, x = r.x + 10;
    if (secret) {
        int n = (int)strlen(text);
        for (int i = 0; i < n && i < 24; i++) gfx_circle(s, x + 4 + i * 12, r.y + r.h / 2, 3, ui(UI_TEXT));
        x += n * 12;
    } else if (text[0]) {
        x = gfx_utext_fit(s, &font_ui, x, ty, text, ui(UI_TEXT), r.w - 24);
    } else if (placeholder && !focused) {
        gfx_utext(s, &font_ui, x, ty, placeholder, ui(UI_DIM));
    }
    if (focused) gfx_fill(s, x + 1, r.y + 7, 2, r.h - 14, ui(UI_ACCENT));
}

void ui_toggle(struct surface *s, int x, int y, bool on) {
    gfx_round(s, x, y, 40, 22, 11, on ? ui(UI_ACCENT) : ui(UI_RAISED));
    gfx_circle(s, on ? x + 29 : x + 11, y + 11, 8, on ? 0xffffff : ui(UI_DIM));
}

void ui_scrollbar(struct surface *s, int x, int y, int h, int total, int visible, int pos) {
    if (total <= visible || h < 20) return;
    int len = h * visible / total;
    if (len < 24) len = 24;
    int off = (h - len) * pos / (total - visible);
    gfx_round_a(s, x, y + off, 5, len, 2, ui(UI_TEXT), 70);
}
