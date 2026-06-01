/* Minimal freestanding C++ runtime for kernel code.
 *
 * Kernel C++ is intentionally smaller than userspace C++:
 * - no exceptions
 * - no RTTI
 * - no global destructor pass at shutdown
 * - new/delete route to kmalloc/kfree, so constructors that allocate must run
 *   after kmalloc_init()
 */
typedef unsigned long size_t;

extern "C" void *kmalloc(size_t size);
extern "C" void kfree(void *ptr);
extern "C" void serial_write(const char *message);

typedef void (*ctor_fn)(void);

extern "C" ctor_fn __kernel_init_array_start[];
extern "C" ctor_fn __kernel_init_array_end[];

extern "C" void kernel_cxx_init(void) {
    ctor_fn *fn;
    for (fn = __kernel_init_array_start; fn != __kernel_init_array_end; ++fn) {
        if (*fn) (*fn)();
    }
}

static void kernel_cxx_panic(const char *message) {
    serial_write("KERNEL C++ PANIC: ");
    serial_write(message);
    serial_write("\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

extern "C" void __cxa_pure_virtual(void) {
    kernel_cxx_panic("pure virtual call");
}

extern "C" void __cxa_deleted_virtual(void) {
    kernel_cxx_panic("deleted virtual call");
}

extern "C" int __cxa_guard_acquire(unsigned long long *guard) {
    return ((*guard & 1u) == 0u) ? 1 : 0;
}

extern "C" void __cxa_guard_release(unsigned long long *guard) {
    *guard |= 1u;
}

extern "C" void __cxa_guard_abort(unsigned long long *guard) {
    (void)guard;
}

extern "C" int __cxa_atexit(void (*)(void *), void *, void *) {
    return 0;
}

extern "C" void __cxa_finalize(void *) {}

void *operator new(size_t size) {
    void *p = kmalloc(size ? size : 1);
    if (!p) kernel_cxx_panic("kernel operator new failed");
    return p;
}

void *operator new[](size_t size) {
    void *p = kmalloc(size ? size : 1);
    if (!p) kernel_cxx_panic("kernel operator new[] failed");
    return p;
}

void operator delete(void *ptr) noexcept {
    kfree(ptr);
}

void operator delete[](void *ptr) noexcept {
    kfree(ptr);
}

void operator delete(void *ptr, size_t) noexcept {
    kfree(ptr);
}

void operator delete[](void *ptr, size_t) noexcept {
    kfree(ptr);
}
