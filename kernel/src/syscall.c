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

#include "syscall.h"

int64_t syscall_dispatch(struct syscall_context *context, uint64_t number, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    (void)arg3;
    (void)arg4;
    (void)arg5;

    if (context == 0) {
        return -SYSCALL_EINVAL;
    }

    switch (number) {
        case SYS_READ:
            return fd_read(context->fd_table, (int)arg0, (void *)(uintptr_t)arg1, (size_t)arg2);
        case SYS_WRITE:
            return fd_write(context->fd_table, (int)arg0, (const void *)(uintptr_t)arg1, (size_t)arg2);
        case SYS_IOCTL:
            return fd_ioctl(context->fd_table, (int)arg0, (uint32_t)arg1, (uintptr_t)arg2);
        case SYS_WINDOW_CREATE:
        case SYS_WINDOW_CREATE_EX:
        case SYS_WINDOW_PRESENT:
        case SYS_EVENT_POLL:
        case SYS_WINDOW_SET_MENU:
        case SYS_JOURNAL_READ:
        case SYS_LOG:
        case SYS_TIMER_SLEEP:
            return -SYSCALL_ENOSYS;
        default:
            return -SYSCALL_ENOSYS;
    }
}
