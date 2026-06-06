#ifndef VIBEOS_FLOAT_H
#define VIBEOS_FLOAT_H

/*
 * float.h — characteristics of floating-point types (IEEE 754).
 *
 * Values for x86_64 with SSE2 (standard on VibeOS userspace).
 */

/* ---- float (32-bit IEEE 754) ------------------------------------------ */
#define FLT_RADIX        2
#define FLT_MANT_DIG     24
#define FLT_DIG          6
#define FLT_MIN_EXP      (-125)
#define FLT_MIN_10_EXP   (-37)
#define FLT_MAX_EXP      128
#define FLT_MAX_10_EXP   38
#define FLT_MIN          1.17549435e-38F
#define FLT_MAX          3.40282347e+38F
#define FLT_EPSILON      1.19209290e-07F
#define FLT_ROUNDS       1       /* round to nearest */
#define FLT_EVAL_METHOD  0       /* operations evaluated in type's range/precision */

/* ---- double (64-bit IEEE 754) ----------------------------------------- */
#define DBL_MANT_DIG     53
#define DBL_DIG          15
#define DBL_MIN_EXP      (-1021)
#define DBL_MIN_10_EXP   (-307)
#define DBL_MAX_EXP      1024
#define DBL_MAX_10_EXP   308
#define DBL_MIN          2.2250738585072014e-308
#define DBL_MAX          1.7976931348623157e+308
#define DBL_EPSILON      2.2204460492503131e-16

/* ---- long double (80-bit x86 extended precision) ---------------------- */
#define LDBL_MANT_DIG    64
#define LDBL_DIG         18
#define LDBL_MIN_EXP     (-16381)
#define LDBL_MIN_10_EXP  (-4931)
#define LDBL_MAX_EXP     16384
#define LDBL_MAX_10_EXP  4932
#define LDBL_MIN         3.36210314311209350626e-4932L
#define LDBL_MAX         1.18973149535723176502e+4932L
#define LDBL_EPSILON     1.08420217248550443401e-19L

#endif /* VIBEOS_FLOAT_H */
