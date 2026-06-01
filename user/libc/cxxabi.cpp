/* Minimal freestanding C++ ABI/runtime for VibeOS userspace.
 *
 * Supports ordinary C++ code with classes, virtual functions, templates,
 * global/static constructors, local static guards, RAII destructors registered
 * through __cxa_atexit, and heap allocation through malloc/free.
 *
 * Not implemented here yet: exception unwinding and the full libc++ STL.
 */
#include <new>
#include <stddef.h>
#include <stdlib.h>
#include <typeinfo>

extern "C" void *__dso_handle = &__dso_handle;

extern "C" void __cxa_pure_virtual(void) {
    abort();
}

extern "C" void __cxa_deleted_virtual(void) {
    abort();
}

namespace std {
type_info::~type_info() {}

const nothrow_t nothrow;
} /* namespace std */

namespace __cxxabiv1 {
class __class_type_info : public std::type_info {
public:
    explicit __class_type_info(const char *name) : std::type_info(name) {}
    ~__class_type_info() override;
};

class __si_class_type_info : public __class_type_info {
public:
    __si_class_type_info(const char *name, const __class_type_info *base)
        : __class_type_info(name), __base_type(base) {}
    ~__si_class_type_info() override;

private:
    const __class_type_info *__base_type __attribute__((unused));
};

class __vmi_class_type_info : public __class_type_info {
public:
    explicit __vmi_class_type_info(const char *name) : __class_type_info(name) {}
    ~__vmi_class_type_info() override;
};

class __fundamental_type_info : public std::type_info {
public:
    explicit __fundamental_type_info(const char *name) : std::type_info(name) {}
    ~__fundamental_type_info() override;
};

class __array_type_info : public std::type_info {
public:
    explicit __array_type_info(const char *name) : std::type_info(name) {}
    ~__array_type_info() override;
};

class __function_type_info : public std::type_info {
public:
    explicit __function_type_info(const char *name) : std::type_info(name) {}
    ~__function_type_info() override;
};

class __enum_type_info : public std::type_info {
public:
    explicit __enum_type_info(const char *name) : std::type_info(name) {}
    ~__enum_type_info() override;
};

class __pbase_type_info : public std::type_info {
public:
    explicit __pbase_type_info(const char *name) : std::type_info(name) {}
    ~__pbase_type_info() override;
};

class __pointer_type_info : public __pbase_type_info {
public:
    explicit __pointer_type_info(const char *name) : __pbase_type_info(name) {}
    ~__pointer_type_info() override;
};

class __pointer_to_member_type_info : public __pbase_type_info {
public:
    explicit __pointer_to_member_type_info(const char *name) : __pbase_type_info(name) {}
    ~__pointer_to_member_type_info() override;
};

__class_type_info::~__class_type_info() {}
__si_class_type_info::~__si_class_type_info() {}
__vmi_class_type_info::~__vmi_class_type_info() {}
__fundamental_type_info::~__fundamental_type_info() {}
__array_type_info::~__array_type_info() {}
__function_type_info::~__function_type_info() {}
__enum_type_info::~__enum_type_info() {}
__pbase_type_info::~__pbase_type_info() {}
__pointer_type_info::~__pointer_type_info() {}
__pointer_to_member_type_info::~__pointer_to_member_type_info() {}
}

extern "C" void *__dynamic_cast(const void *, const void *, const void *, long) {
    abort();
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

struct atexit_entry {
    void (*fn)(void *);
    void *arg;
    void *dso;
    int active;
};

static atexit_entry g_atexit[128];
static int g_atexit_count;

extern "C" int __cxa_atexit(void (*fn)(void *), void *arg, void *dso) {
    if (!fn || g_atexit_count >= (int)(sizeof(g_atexit) / sizeof(g_atexit[0]))) {
        return -1;
    }
    g_atexit[g_atexit_count].fn = fn;
    g_atexit[g_atexit_count].arg = arg;
    g_atexit[g_atexit_count].dso = dso;
    g_atexit[g_atexit_count].active = 1;
    ++g_atexit_count;
    return 0;
}

extern "C" void __cxa_finalize(void *dso) {
    int i;
    for (i = g_atexit_count - 1; i >= 0; --i) {
        if (g_atexit[i].active && (dso == 0 || dso == g_atexit[i].dso)) {
            void (*fn)(void *) = g_atexit[i].fn;
            void *arg = g_atexit[i].arg;
            g_atexit[i].active = 0;
            fn(arg);
        }
    }
}

void *operator new(size_t size) {
    void *p = malloc(size ? size : 1);
    if (!p) abort();
    return p;
}

void *operator new[](size_t size) {
    void *p = malloc(size ? size : 1);
    if (!p) abort();
    return p;
}

void operator delete(void *ptr) noexcept {
    free(ptr);
}

void operator delete[](void *ptr) noexcept {
    free(ptr);
}

void operator delete(void *ptr, size_t) noexcept {
    free(ptr);
}

void operator delete[](void *ptr, size_t) noexcept {
    free(ptr);
}

void *operator new(size_t size, const std::nothrow_t &) noexcept {
    return malloc(size ? size : 1);
}

void *operator new[](size_t size, const std::nothrow_t &) noexcept {
    return malloc(size ? size : 1);
}

void operator delete(void *ptr, const std::nothrow_t &) noexcept {
    free(ptr);
}

void operator delete[](void *ptr, const std::nothrow_t &) noexcept {
    free(ptr);
}
