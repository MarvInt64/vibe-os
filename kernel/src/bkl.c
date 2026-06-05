/*
 * MIT License
 *
 * Copyright (c) 2026 Marvin Kicha
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "bkl.h"
#include "cpu.h"
#include "spinlock.h"

static spinlock_t       g_bkl_lock = SPINLOCK_INIT;
static volatile int     g_bkl_owner = -1;   /* CPU index that holds it, -1 = free */
static volatile int     g_bkl_depth = 0;    /* recursion depth of the owner       */

void bkl_acquire(void) {
    int me = (int)this_cpu()->index;

    /* Fast path: we already own it (a nested kernel entry, e.g. a timer IRQ in
     * kernel context) — just deepen. owner is only ever set to `me` by this CPU
     * while holding the lock, so reading it lock-free is safe for that test. */
    if (g_bkl_owner == me) {
        g_bkl_depth++;
        return;
    }

    spin_lock(&g_bkl_lock);
    g_bkl_owner = me;
    g_bkl_depth = 1;
}

void bkl_release(void) {
    if (--g_bkl_depth == 0) {
        g_bkl_owner = -1;
        spin_unlock(&g_bkl_lock);
    }
}
