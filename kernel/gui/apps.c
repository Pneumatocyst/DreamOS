/* Every app on the desktop, in the order the dock and the launcher show them. */
#include "gui/wm.h"

void launcher_open(void);

const struct app_entry app_list[] = {
    { "apps",       "everything, searchable",       ICON_APPS,     launcher_open, true  },
    { "terminal",   "the shell, in a window",       ICON_TERMINAL, term_open,     true  },
    { "files",      "browse and manage files",      ICON_FILES,    files_open,    true  },
    { "notes",      "a text editor",                ICON_NOTES,    notes_open,    true  },
    { "calculator", "add, subtract, multiply",      ICON_CALC,     calc_open,     true  },
    { "paint",      "draw with the mouse",          ICON_PAINT,    paint_open,    true  },
    { "monitor",    "cpu, memory and tasks",        ICON_MONITOR,  monitor_open,  true  },
    { "settings",   "themes, wallpaper, system",    ICON_SETTINGS, settings_open, true  },
    { "clock",      "an analog clock",              ICON_CLOCK,    clock_open,    false },
    { "snake",      "a game: eat, grow, don't crash", ICON_SNAKE,  snake_open,    false },
    { "about",      "about dream",                  ICON_ABOUT,    about_open,    false },
};
const int app_count = sizeof app_list / sizeof app_list[0];

const struct app_entry *app_by_name(const char *name) {
    for (int i = 0; i < app_count; i++) if (!strcmp(app_list[i].name, name)) return &app_list[i];
    if (!strcmp(name, "calc")) return app_by_name("calculator");
    if (!strcmp(name, "edit") || !strcmp(name, "editor")) return app_by_name("notes");
    return NULL;
}
