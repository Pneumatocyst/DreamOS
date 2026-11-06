#include "drivers/theme.h"
#include "drivers/console.h"
#include "kernel.h"
#include "gui/wm.h"

const struct theme themes[] = {
    {   /* cool blue night */
        "midnight", 0x1a1b26, 0x0f1017, 0x16161e,
        { 0xc0caf5, 0x565f89, 0x7aa2f7, 0x9ece6a, 0xe0af68, 0xf7768e },
    },
    {   /* green phosphor terminal */
        "nostromo", 0x041a0c, 0x000803, 0x021006,
        { 0x39ff7a, 0x1d7a3c, 0x9dffbf, 0x39ff7a, 0xffd23f, 0xff5a5a },
    },
    {   /* deep-space violet */
        "cosmos", 0x1e1238, 0x07040f, 0x140c26,
        { 0xe6dcff, 0x7a6a9e, 0xc792ea, 0x89ddff, 0xffcb6b, 0xff5370 },
    },
    {   /* warm amber CRT */
        "ember", 0x1c1209, 0x0a0603, 0x140d06,
        { 0xffb347, 0x8a5a24, 0xffd28a, 0xc3e88d, 0xffcb6b, 0xff6e5a },
    },
};
const int theme_count = sizeof(themes) / sizeof(themes[0]);
const struct theme *theme = &themes[0];

int theme_set(const char *name) {
    for (int i = 0; i < theme_count; i++) {
        if (strcmp(themes[i].name, name) == 0) {
            theme = &themes[i];
            con_redraw();
            wm_theme_changed();
            return 0;
        }
    }
    return -1;
}
