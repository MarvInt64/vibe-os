#ifndef VIBEOS_MATH_H
#define VIBEOS_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

/* IEEE 754 special values */
#define HUGE_VAL   __builtin_huge_val()
#define HUGE_VALF  __builtin_huge_valf()
#define INFINITY   __builtin_inff()
#define NAN        __builtin_nanf("")

#define M_E        2.71828182845904523536
#define M_LOG2E    1.44269504088896340736
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_SQRT2    1.41421356237309504880

/* ---- Classification macros (use compiler builtins) -------------------- */
#define isnan(x)      __builtin_isnan(x)
#define isinf(x)      __builtin_isinf(x)
#define isfinite(x)   __builtin_isfinite(x)
#define isnormal(x)   __builtin_isnormal(x)
#define signbit(x)    __builtin_signbit(x)
#define fpclassify(x) __builtin_fpclassify(0,1,4,3,2, (x))

/* FP_* constants for fpclassify */
#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4

/* ---- Simple operations ------------------------------------------------- */
#ifdef __TINYC__
static __inline__ double fabs(double x)   { return x < 0.0 ? -x : x; }
static __inline__ float  fabsf(float x)   { return x < 0.0f ? -x : x; }
static __inline__ double fmin(double a, double b) { return a < b ? a : b; }
static __inline__ double fmax(double a, double b) { return a > b ? a : b; }
#else
static __inline__ double fabs(double x)   { return __builtin_fabs(x);   }
static __inline__ float  fabsf(float x)   { return __builtin_fabsf(x);  }
static __inline__ double fmin(double a, double b) { return __builtin_fmin(a,b); }
static __inline__ double fmax(double a, double b) { return __builtin_fmax(a,b); }
#endif

/* ---- Rounding ---------------------------------------------------------- */
/*
 * SSE4.1 roundsd/roundss give the fastest path when available (clang).
 * TCC doesn't support SSE constraints ("x"), so we use x87 or pure C.
 */
#if defined(__TINYC__) || defined(ARCH_ARM64)

/*
 * TCC / ARM64: pure-C fallback for all math functions (no SSE/x87 asm).
 */

static __inline__ double floor(double x) {
    long long ix = (long long)x;
    if (x >= 0.0 || (double)ix == x) return (double)ix;
    return (double)(ix - 1);
}
static __inline__ float floorf(float x) { return (float)floor((double)x); }

static __inline__ double ceil(double x) {
    long long ix = (long long)x;
    if (x <= 0.0 || (double)ix == x) return (double)ix;
    return (double)(ix + 1);
}

static __inline__ double trunc(double x) {
    return (double)(long long)x;
}

static __inline__ double round(double x) {
    if (x >= 0.0) return floor(x + 0.5);
    return ceil(x - 0.5);
}

/*
 * sqrt via Newton's method — converges quickly (quadratic).
 * For x <= 0: returns 0 (caller should check domain).
 */
static __inline__ double sqrt(double x) {
    if (x <= 0.0) return 0.0;
    double r = x, last;
    do { last = r; r = 0.5 * (r + x / r); } while (r != last);
    return r;
}
static __inline__ float sqrtf(float x) { return (float)sqrt((double)x); }

/*
 * fmod: x - trunc(x/y) * y
 * Use integer truncation via long long cast.
 */
static __inline__ double fmod(double x, double y) {
    if (y == 0.0) return 0.0;
    double q = (double)(long long)(x / y);
    return x - q * y;
}

/*
 * scalbn: x * 2^n via bit-level exponent manipulation.
 * Avoids the slow multiply-in-a-loop approach.
 */
static __inline__ double scalbn(double x, int n) {
    /* IEEE 754 double: 1 sign, 11 exponent, 52 mantissa */
    union { double d; unsigned long long u; } u;
    u.d = x;
    int exp = (int)((u.u >> 52) & 0x7FFuLL);
    /* Subnormal or zero: multiply in a loop as a simple fallback. */
    if (exp == 0) {
        double r = x;
        if (n > 0) while (n-- > 0) r *= 2.0;
        else       while (n++ < 0) r *= 0.5;
        return r;
    }
    exp += n;
    if (exp <= 0) return 0.0;  /* underflow */
    if (exp >= 0x7FF) {        /* overflow to inf */
        u.u = (u.u & 0x8000000000000000uLL) | 0x7FF0000000000000uLL;
        return u.d;
    }
    u.u = (u.u & 0x800FFFFFFFFFFFFFuLL) | ((unsigned long long)exp << 52);
    return u.d;
}

#else /* clang — SSE4.1 + x87 */

static __inline__ double floor(double x) {
    double r;
    __asm__("roundsd $1, %1, %0" : "=x"(r) : "xm"(x));
    return r;
}
static __inline__ float floorf(float x) {
    float r;
    __asm__("roundss $1, %1, %0" : "=x"(r) : "xm"(x));
    return r;
}
static __inline__ double ceil(double x) {
    double r;
    __asm__("roundsd $2, %1, %0" : "=x"(r) : "xm"(x));
    return r;
}
static __inline__ double trunc(double x) {
    double r;
    __asm__("roundsd $3, %1, %0" : "=x"(r) : "xm"(x));
    return r;
}
static __inline__ double round(double x) {
    /*
     * Use floor(x + 0.5) for x >= 0 and ceil(x - 0.5) for x < 0.
     * This implements "round half away from zero" (C99 §7.12.9.6).
     * SSE4.1 roundsd $0 is banker's rounding (round ties to even),
     * which gives the wrong answer for .5 (e.g. round(2.5) == 2).
     */
    double r;
    if (x >= 0.0)
        __asm__("roundsd $1, %1, %0" : "=x"(r) : "xm"(x + 0.5));  /* floor */
    else
        __asm__("roundsd $2, %1, %0" : "=x"(r) : "xm"(x - 0.5));  /* ceil  */
    return r;
}

/* ---- Square root (SSE2) ----------------------------------------------- */
static __inline__ double sqrt(double x) {
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "xm"(x));
    return r;
}
static __inline__ float sqrtf(float x) {
    float r;
    __asm__("sqrtss %1, %0" : "=x"(r) : "xm"(x));
    return r;
}

/* ---- fmod (x87 fprem1) ------------------------------------------------ */
static __inline__ double fmod(double x, double y) {
    double r;
    __asm__(
        "1: fprem1\n\t"
        "fnstsw %%ax\n\t"
        "testb  $4, %%ah\n\t"
        "jnz    1b\n\t"
        "fstp   %%st(1)"
        : "=t"(r) : "0"(x), "u"(y) : "ax", "st(1)"
    );
    return r;
}

#endif /* __TINYC__ */

static __inline__ long   lround(double x)    { return (long)round(x); }
static __inline__ long long llround(double x) { return (long long)round(x); }

/* ---- Transcendentals: declared here, implemented in math.S (x87) ------ */
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double exp(double x);
double exp2(double x);
double expm1(double x);
double log(double x);
double log2(double x);
double log10(double x);
double log1p(double x);
double pow(double x, double y);
double hypot(double x, double y);
double cbrt(double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double asinh(double x);
double acosh(double x);
double atanh(double x);

float  sinf(float x);
float  cosf(float x);
float  tanf(float x);
float  powf(float x, float y);
float  expf(float x);
float  logf(float x);

/* ---- Misc -------------------------------------------------------------- */
static __inline__ double copysign(double x, double y) {
    return __builtin_copysign(x, y);
}
#if !defined(__TINYC__) && !defined(ARCH_ARM64)
static __inline__ double scalbn(double x, int n) {
    /* x * 2^n via ldexp */
    double r = x;
    __asm__("fildl %1\n\t fscale\n\t fstp %%st(1)" : "+t"(r) : "m"(n));
    return r;
}
#endif
static __inline__ double ldexp(double x, int exp) { return scalbn(x, exp); }
static __inline__ long double ldexpl(long double x, int exp) { return (long double)ldexp((double)x, exp); }
static __inline__ double nearbyint(double x) { return round(x); }
static __inline__ double rint(double x) { return round(x); }
static __inline__ long lrint(double x) { return (long)round(x); }

#ifdef __cplusplus
}
#endif

#endif
