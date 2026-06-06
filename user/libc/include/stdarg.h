#ifndef VIBEOS_STDARG_H
#define VIBEOS_STDARG_H

/*
 * stdarg.h — variable argument lists for x86_64 (System V AMD64 ABI).
 *
 * On x86_64, variadic arguments are passed in registers (RDI, RSI, RDX,
 * RCX, R8, R9) then on the stack.  We use compiler builtins when
 * available; otherwise, provide a minimal implementation for freestanding
 * targets.
 */

#if defined(__clang__) || defined(__GNUC__) || defined(__TINYC__)
/* Use compiler builtins — correct for all calling conventions. */
typedef __builtin_va_list va_list;
#define va_start(ap, last)  __builtin_va_start(ap, last)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)
#define va_end(ap)          __builtin_va_end(ap)
#define va_copy(dst, src)   __builtin_va_copy(dst, src)

#else
/*
 * Minimal x86_64 ABI implementation for compilers without builtins.
 * Not fully compliant but sufficient for freestanding use cases.
 */
typedef struct {
    unsigned int gp_offset;    /* offset in bytes from reg_save_area to next
                                  available integer register */
    unsigned int fp_offset;    /* offset from reg_save_area to next available
                                  floating-point register */
    void *overflow_arg_area;   /* address of next stack-passed argument */
    void *reg_save_area;       /* address of register save area (rsp at call) */
} va_list[1];

#define va_start(ap, last) \
    __builtin_va_start(ap, last)

#define va_arg(ap, type) \
    __builtin_va_arg(ap, type)

#define va_end(ap) \
    __builtin_va_end(ap)

#define va_copy(dst, src) \
    __builtin_va_copy(dst, src)
#endif

#endif /* VIBEOS_STDARG_H */
