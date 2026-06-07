
#include "bkl.h"
#include "cpu.h"
#include "spinlock.h"
#include "serial.h"

static spinlock_t       g_bkl_lock = SPINLOCK_INIT;
static volatile int     g_bkl_owner = -1;   /* CPU index that holds it, -1 = free */
static volatile int     g_bkl_depth = 0;    /* recursion depth of the owner       */

void bkl_acquire(void) {
    struct cpu *c = this_cpu();
    if (!c) return;
    int me = (int)c->index;

    if (g_bkl_owner == me) {
        g_bkl_depth++;
        return;
    }

    spin_lock(&g_bkl_lock);
    g_bkl_owner = me;
    g_bkl_depth = 1;
}

void bkl_release(void) {
    struct cpu *c = this_cpu();
    if (!c) return;
    if (--g_bkl_depth == 0) {
        g_bkl_owner = -1;
        spin_unlock(&g_bkl_lock);
    }
}
