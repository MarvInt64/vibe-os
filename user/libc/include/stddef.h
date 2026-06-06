#ifndef VIBEOS_STDDEF_H
#define VIBEOS_STDDEF_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Standard type definitions ----------------------------------------- */

/* Signed integer type of the result of subtracting two pointers. */
#ifndef __PTRDIFF_TYPE__
#define __PTRDIFF_TYPE__ long
#endif
typedef __PTRDIFF_TYPE__ ptrdiff_t;

/* Unsigned integer type of the result of the sizeof operator. */
#ifndef __SIZE_TYPE__
#define __SIZE_TYPE__ unsigned long
#endif
typedef __SIZE_TYPE__ size_t;

/* Wide character type. In C++ wchar_t is a built-in keyword, so it must not
 * be typedef'd there; only C needs the definition. */
#ifndef __cplusplus
#ifndef __WCHAR_TYPE__
#define __WCHAR_TYPE__ int
#endif
typedef __WCHAR_TYPE__ wchar_t;
#endif

/* NULL pointer constant. */
#undef NULL
#if defined(__cplusplus) && __cplusplus >= 201103L
# define NULL nullptr
#elif defined(__cplusplus)
# define NULL 0L
#else
# define NULL ((void *)0)
#endif

/* Offset of a member in a struct. */
#if defined(__clang__) || defined(__GNUC__) || defined(__TINYC__)
# define offsetof(type, member) __builtin_offsetof(type, member)
#else
# define offsetof(type, member) ((size_t)((char *)&(((type *)0)->member) - (char *)0))
#endif

/* Maximum alignment required by the implementation. */
#ifndef __BIGGEST_ALIGNMENT__
#define __BIGGEST_ALIGNMENT__ 16
#endif

#ifdef __cplusplus
}
#endif

#endif /* VIBEOS_STDDEF_H */
