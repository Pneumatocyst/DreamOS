/* How busy is the CPU? The idle task only runs when nobody else wants to, so
 * the share of timer ticks that landed in it is the share of time we were idle. */
#include "gui/wm.h"
#include "sched/task.h"
#include "arch/cpu.h"

static uint64_t last_total, last_idle;
static int usage;

void cpu_sample(void) {
    static uint64_t last;
    if (pit_ticks() - last < pit_hz() / 4) return;   /* several callers: sample at most 4x a second */
    last = pit_ticks();
    struct task_info t[32];
    int n = task_list(t, 32);
    uint64_t idle = 0, total = sched_total_ticks();
    for (int i = 0; i < n; i++) if (!strcmp(t[i].name, "idle")) idle = t[i].ticks;
    uint64_t dt = total - last_total, di = idle - last_idle;
    if (dt) usage = (int)(100 - di * 100 / dt);
    if (usage < 0) usage = 0;
    last_total = total;
    last_idle = idle;
}

int cpu_usage(void) { return usage; }
