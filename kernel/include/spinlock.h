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
 * SMP spinlocks (milestone 3).
 *
 * A plain test-and-set lock with acquire/release ordering — enough to make the
 * kernel safe once more than one CPU runs kernel code. The irqsave variants
 * additionally disable interrupts while held so a lock taken in process context
 * cannot deadlock against the same lock taken from an interrupt handler on the
 * same CPU.
 */
#ifndef VIBEOS_SPINLOCK_H
#define VIBEOS_SPINLOCK_H

typedef struct spinlock {
    volatile unsigned char locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spin_lock(spinlock_t *lock) {
    while (__atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE))
#if defined(ARCH_ARM64)
        __asm__ volatile("yield");
#else
        __asm__ volatile("pause");
#endif
}

static inline int spin_trylock(spinlock_t *lock) {
    return __atomic_test_and_set(&lock->locked, __ATOMIC_ACQUIRE) == 0;
}

static inline void spin_unlock(spinlock_t *lock) {
    __atomic_clear(&lock->locked, __ATOMIC_RELEASE);
}

/* Disable interrupts, then take the lock; returns the prior interrupt-flag state. */
static inline unsigned long spin_lock_irqsave(spinlock_t *lock) {
    unsigned long flags;
#if defined(ARCH_ARM64)
    __asm__ volatile("mrs %0, daif" : "=r"(flags));
    __asm__ volatile("msr daifset, #3" ::: "memory");
#else
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
#endif
    spin_lock(lock);
    return flags;
}

/* Release the lock, then restore the interrupt state saved above. */
static inline void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags) {
    spin_unlock(lock);
#if defined(ARCH_ARM64)
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
#else
    __asm__ volatile("push %0; popfq" : : "r"(flags) : "memory", "cc");
#endif
}

#endif
