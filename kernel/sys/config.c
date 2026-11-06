/* Settings that survive a reboot, kept as plain text in /etc/dream.conf:
 *
 *     theme=nostromo
 *     wallpaper=aurora
 *     autostart=yes
 *
 * It's an ordinary file, so `cat /etc/dream.conf` shows it and `edit` can change it.
 * dreamfs saves it to disk like everything else. */
#include "gui/wm.h"
#include "fs/ramfs.h"
#include "fs/diskfs.h"
#include "drivers/theme.h"
#include "arch/cpu.h"

#define CONF "/etc/dream.conf"
#define PASSWD "/etc/passwd.dream"

/* Find "key=" at the start of a line; copy its value out. */
static bool lookup(const char *text, size_t len, const char *key, char *out, size_t n) {
    size_t kl = strlen(key), i = 0;
    while (i < len) {
        size_t end = i;
        while (end < len && text[end] != '\n') end++;
        if (end - i > kl && !strncmp(text + i, key, kl) && text[i + kl] == '=') {
            size_t v = i + kl + 1, m = 0;
            while (v < end && m < n - 1) out[m++] = text[v++];
            out[m] = 0;
            return true;
        }
        i = end + 1;
    }
    return false;
}

const char *config_get(const char *key, const char *fallback) {
    static char val[4][64];          /* a few in flight at once is plenty */
    static int which;
    char *out = val[which++ % 4];
    fs_lock();
    int n = fs_lookup(0, CONF);
    bool found = n >= 0 && !fs_get(n)->dir && lookup(fs_data(n), fs_get(n)->size, key, out, 64);
    fs_unlock();
    return found ? out : fallback;
}

void config_set(const char *key, const char *value) {
    static char buf[2048];
    size_t len = 0, kl = strlen(key);
    fs_lock();
    int n = fs_lookup(0, CONF);
    if (n < 0) n = fs_create(0, CONF, false);
    if (n < 0) { fs_unlock(); return; }
    /* copy every other line, then add ours at the end */
    const char *d = fs_data(n);
    size_t size = fs_get(n)->size, i = 0;
    while (i < size) {
        size_t end = i;
        while (end < size && d[end] != '\n') end++;
        bool ours = end - i > kl && !strncmp(d + i, key, kl) && d[i + kl] == '=';
        if (!ours && end > i && len + (end - i) + 1 < sizeof buf) {
            memcpy(buf + len, d + i, end - i);
            len += end - i;
            buf[len++] = '\n';
        }
        i = end + 1;
    }
    len += snprintf(buf + len, sizeof buf - len, "%s=%s\n", key, value);
    fs_write(n, buf, len, false);
    fs_unlock();
    diskfs_autosave();
}

void config_apply(void) {
    const char *t = config_get("theme", NULL);
    if (t) theme_set(t);
}

/* ---- the lock screen password ------------------------------------------------------
 * Stored as a salted FNV-1a hash, not the password itself. (A real OS would use a slow
 * hash like bcrypt or argon2; this keeps the idea without the maths.) */

static uint64_t hash(const char *salt, const char *pw) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (int round = 0; round < 1000; round++) {     /* repeat it to make guessing slower */
        for (const char *p = salt; *p; p++) { h ^= (uint8_t)*p; h *= 0x100000001b3ull; }
        for (const char *p = pw; *p; p++)   { h ^= (uint8_t)*p; h *= 0x100000001b3ull; }
    }
    return h;
}

bool passwd_set(void) {
    int n = fs_lookup(0, PASSWD);
    return n >= 0 && fs_get(n)->size > 0;
}

bool passwd_check(const char *pw) {
    char line[64], salt[17], want[17];
    fs_lock();
    int n = fs_lookup(0, PASSWD);
    size_t len = n >= 0 ? fs_get(n)->size : 0;
    if (len >= sizeof line) len = sizeof line - 1;
    if (n >= 0) memcpy(line, fs_data(n), len);
    fs_unlock();
    line[len] = 0;
    if (len < 33) return true;                       /* no (valid) password: anyone may in */
    memcpy(salt, line, 16); salt[16] = 0;
    memcpy(want, line + 17, 16); want[16] = 0;
    char got[17];
    snprintf(got, sizeof got, "%016lx", hash(salt, pw));
    return !strcmp(got, want);
}

void passwd_change(const char *pw) {
    fs_lock();
    int n = fs_lookup(0, PASSWD);
    if (!pw[0]) { if (n >= 0) fs_remove(n); fs_unlock(); diskfs_autosave(); return; }
    if (n < 0) n = fs_create(0, PASSWD, false);
    char salt[17], line[40];
    snprintf(salt, sizeof salt, "%016lx", pit_ticks() * 0x9e3779b97f4a7c15ull ^ (uint64_t)n);
    int len = snprintf(line, sizeof line, "%s:%016lx\n", salt, hash(salt, pw));
    if (n >= 0) fs_write(n, line, len, false);
    fs_unlock();
    diskfs_autosave();
}
