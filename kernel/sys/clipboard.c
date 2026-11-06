/* The clipboard: one piece of text that every app and program can put things in
 * and take them out of. That's all a clipboard is; the rest (selecting, ctrl+c,
 * ctrl+v) is up to each app. It lives in memory, so it's empty after a reboot. */
#include "gui/wm.h"
#include "sched/task.h"
#include "mm/heap.h"

static char *clip;
static size_t clip_len;
static struct mutex lock;

void clipboard_set(const char *text, size_t len) {
    if (len > CLIP_MAX) len = CLIP_MAX;
    mutex_lock(&lock);
    if (!clip) clip = kmalloc(CLIP_MAX);
    if (clip) { memcpy(clip, text, len); clip_len = len; }
    mutex_unlock(&lock);
}

size_t clipboard_get(char *out, size_t max) {
    mutex_lock(&lock);
    size_t n = clip_len < max ? clip_len : max;
    if (n) memcpy(out, clip, n);
    mutex_unlock(&lock);
    return n;
}

size_t clipboard_len(void) { return clip_len; }
