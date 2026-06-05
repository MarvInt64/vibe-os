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

/*
 * Big Kernel Lock (milestone 4, step 3b-2).
 *
 * One global lock that serialises all in-kernel execution so multiple CPUs can
 * run userspace in parallel while the (still single-threaded-unsafe) kernel
 * core stays correct. A CPU holds it whenever it runs kernel code and releases
 * it on the way out to userspace; the assembly entry/exit stubs call
 * bkl_acquire / bkl_release around the ring transition.
 *
 * It is RECURSIVE per owning CPU: a kernel-context interrupt (e.g. the timer
 * firing while the boot CPU is already in the kernel) re-acquires the lock the
 * CPU already owns by bumping a depth counter instead of self-deadlocking. The
 * lock is only truly released when the depth returns to zero.
 */
#ifndef VIBEOS_BKL_H
#define VIBEOS_BKL_H

/* Acquire the big kernel lock (recursively if this CPU already owns it). These
 * are called from the assembly trap entry/exit paths, so they are plain extern
 * C functions with the default calling convention. */
void bkl_acquire(void);
void bkl_release(void);

#endif
