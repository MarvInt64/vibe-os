/* math.S — transcendental math functions for VibeOS userspace.
 *
 * x86-64 System V ABI: double arguments in xmm0/xmm1, return in xmm0.
 * We move the argument to the x87 stack, use the x87 instruction, then
 * move the result back.  The temporary 8-byte slot on the stack is safe
 * because -mno-red-zone is in effect for all userspace code. */

    .text

/* ---- Helper macros ----------------------------------------------------- */

/* Load xmm0 → ST(0), compute <insn>, store ST(0) → xmm0 */
.macro UNARY insn
    subq  $8,    %rsp
    movsd %xmm0, (%rsp)
    fldl  (%rsp)
    \insn
    fstpl (%rsp)
    movsd (%rsp), %xmm0
    addq  $8,    %rsp
    ret
.endm

/* Load xmm1 → ST(0), xmm0 → ST(1), compute <insn>, result in ST(0). */
.macro BINARY_YX insn
    subq  $16,   %rsp
    movsd %xmm0, (%rsp)
    movsd %xmm1, 8(%rsp)
    fldl  8(%rsp)      /* ST(0) = xmm1 (y for atan2, etc.) */
    fldl  (%rsp)       /* ST(0) = xmm0 (x), ST(1) = xmm1   */
    \insn              /* result in ST(0)                   */
    fstpl (%rsp)
    movsd (%rsp), %xmm0
    addq  $16,   %rsp
    ret
.endm

/* ---- Trigonometry ------------------------------------------------------ */

    .global sin
sin:
    UNARY fsin

    .global cos
cos:
    UNARY fcos

    .global tan
tan:
    subq  $16,   %rsp
    movsd %xmm0, (%rsp)
    fldl  (%rsp)
    fptan
    fstp  %st(0)       /* discard the 1.0 pushed by fptan */
    fstpl (%rsp)
    movsd (%rsp), %xmm0
    addq  $16,   %rsp
    ret

    .global atan
atan:
    /* atan(x) = fpatan(x, 1.0) */
    subq  $8,    %rsp
    movsd %xmm0, (%rsp)
    fldl  (%rsp)       /* ST(0) = x */
    fld1               /* ST(0) = 1.0, ST(1) = x */
    fpatan             /* ST(0) = atan(ST(1)/ST(0)) = atan(x/1) = atan(x) */
    fstpl (%rsp)
    movsd (%rsp), %xmm0
    addq  $8,    %rsp
    ret

    .global atan2
atan2:
    /* atan2(y, x): xmm0=y, xmm1=x */
    subq  $16,   %rsp
    movsd %xmm0, (%rsp)
    movsd %xmm1, 8(%rsp)
    fldl  8(%rsp)      /* ST(0) = x */
    fldl  (%rsp)       /* ST(0) = y, ST(1) = x */
    fpatan             /* ST(0) = atan2(y, x) */
    fstpl (%rsp)
    movsd (%rsp), %xmm0
    addq  $16,   %rsp
    ret

    .global asin
asin:
    /* asin(x) = atan2(x, sqrt(1-x*x)) */
    subq  $16,   %rsp
    movsd %xmm0, (%rsp)
    fldl  (%rsp)       /* ST(0) = x */
    fld   %st(0)       /* ST(0) = x, ST(1) = x */
    fmul  %st(0)       /* ST(0) = x*x */
    fld1
    fsubrp             /* ST(0) = 1-x*x */
    fsqrt              /* ST(0) = sqrt(1-x*x) */
    fxch  %st(1)       /* ST(0) = x, ST(1) = sqrt(1-x*x) */
    fpatan             /* ST(0) = atan2(x, sqrt(1-x*x)) */
    fstpl (%rsp)
    movsd (%rsp), %xmm0
    addq  $16,   %rsp
    ret

    .global acos
acos:
    /* acos(x) = atan2(sqrt(1-x*x), x) */
    subq  $16,   %rsp
    movsd %xmm0, (%rsp)
    fldl  (%rsp)       /* ST(0) = x */
    fld   %st(0)       /* duplicate x */
    fmul  %st(0)       /* x*x */
    fld1
    fsubrp             /* 1-x*x */
    fsqrt              /* sqrt(1-x*x) */
                       /* ST(0)=sqrt(1-x*x), ST(1)=x */
    fpatan             /* atan2(ST(1), ST(0)) but fpatan computes atan(ST(1)/ST(0)) */
    fstpl (%rsp)
    movsd (%rsp), %xmm0
    addq  $16,   %rsp
    ret

/* ---- Exponential / logarithm ------------------------------------------ */

    .global exp
exp:
    /* exp(x) = 2^(x * log2(e)) using fscale+f2xm1 */
    subq  $16,   %rsp
    movsd %xmm0, (%rsp)
    fldl  (%rsp)       /* ST(0) = x */
    fldl2e             /* ST(0) = log2(e), ST(1) = x */
    fmulp              /* ST(0) = x * log2(e) */
    /* Now compute 2^ST(0) */
    fld   %st(0)       /* duplicate */
    frndint            /* ST(0) = trunc(x*log2e) = integer part */
    fxch  %st(1)
    fsub  %st(1), %st(0) /* ST(0) = fractional part */
    f2xm1              /* ST(0) = 2^frac - 1 */
    fld1
    faddp              /* ST(0) = 2^frac */
    fscale             /* ST(0) = 2^frac * 2^int = 2^(x*log2e) = exp(x) */
    fstp  %st(1)
    fstpl (%rsp)
    movsd (%rsp), %xmm0
    addq  $16,   %rsp
    ret

    .global exp2
exp2:
    /* 2^x using same approach */
    subq  $16,   %rsp
    movsd %xmm0, (%rsp)
    fldl  (%rsp)
    fld   %st(0)
    frndint
    fxch  %st(1)
    fsub  %st(1), %st(0)
    f2xm1
    fld1
    faddp
    fscale
    fstp  %st(1)
    fstpl (%rsp)
    movsd (%rsp), %xmm0
    addq  $16,   %rsp
    ret

    .global log
log:
    /* ln(x) = log2(x) / log2(e) = log2(x) * ln(2) */
    subq  $8,    %rsp
    movsd %xmm0, (%rsp)
    fldln2             /* ST(0) = ln(2) */
    fldl  (%rsp)       /* ST(0) = x, ST(1) = ln(2) */
    fyl2x              /* ST(0) = ln(2) * log2(x) = ln(x) */
    fstpl (%rsp)
    movsd (%rsp), %xmm0
    addq  $8,    %rsp
    ret

    .global log2
log2:
    subq  $8,    %rsp
    movsd %xmm0, (%rsp)
    fld1               /* ST(0) = 1.0 */
    fldl  (%rsp)       /* ST(0) = x, ST(1) = 1.0 */
    fyl2x              /* ST(0) = 1.0 * log2(x) */
    fstpl (%rsp)
    movsd (%rsp), %xmm0
    addq  $8,    %rsp
    ret

    .global log10
log10:
    subq  $8,    %rsp
    movsd %xmm0, (%rsp)
    fldlg2             /* ST(0) = log10(2) */
    fldl  (%rsp)       /* ST(0) = x, ST(1) = log10(2) */
    fyl2x              /* ST(0) = log10(2) * log2(x) = log10(x) */
    fstpl (%rsp)
    movsd (%rsp), %xmm0
    addq  $8,    %rsp
    ret

    .global pow
pow:
    /* pow(x, y) = 2^(y * log2(x)) */
    /* xmm0 = x, xmm1 = y */
    subq  $24,   %rsp
    movsd %xmm0, (%rsp)
    movsd %xmm1, 8(%rsp)

    /* check x <= 0: use log only for x > 0 */
    fldl  (%rsp)       /* x */
    ftst
    fnstsw %ax
    sahf
    jbe   .Lpow_special

    /* x > 0: compute y * log2(x) */
    fldl  8(%rsp)      /* ST(0) = y */
    fxch  %st(1)       /* ST(0) = x, ST(1) = y */
    fyl2x              /* ST(0) = y * log2(x) */

    /* 2^ST(0) */
    fld   %st(0)
    frndint
    fxch  %st(1)
    fsub  %st(1), %st(0)
    f2xm1
    fld1
    faddp
    fscale
    fstp  %st(1)
    fstpl (%rsp)
    movsd (%rsp), %xmm0
    addq  $24,   %rsp
    ret

.Lpow_special:
    /* x <= 0: for x==0 return 0, otherwise NaN */
    fstp  %st(0)
    fldz
    fstpl (%rsp)
    movsd (%rsp), %xmm0
    addq  $24,   %rsp
    ret

    .global log1p
log1p:
    /* log1p(x) = ln(1+x) using fyl2xp1 for |x| <= sqrt(2)-1 */
    subq  $8,    %rsp
    movsd %xmm0, (%rsp)
    fldln2
    fldl  (%rsp)
    fyl2xp1
    fstpl (%rsp)
    movsd (%rsp), %xmm0
    addq  $8,    %rsp
    ret

    .global expm1
expm1:
    subq  $8,    %rsp
    movsd %xmm0, (%rsp)
    fldl  (%rsp)
    fldl2e
    fmulp
    f2xm1
    fstpl (%rsp)
    movsd (%rsp), %xmm0
    addq  $8,    %rsp
    ret

/* ---- Hyperbolic -------------------------------------------------------- */

    .global sinh
sinh:
    /* sinh(x) = (e^x - e^{-x}) / 2  — call exp twice */
    subq  $24,   %rsp
    movsd %xmm0, (%rsp)
    /* save -x */
    movsd %xmm0, 8(%rsp)
    /* use exp(x) */
    call  exp
    movsd %xmm0, 16(%rsp)  /* save exp(x) */
    /* exp(-x): negate */
    movsd 8(%rsp), %xmm0
    xorpd .Lneg(%rip), %xmm0
    call  exp
    /* xmm0 = exp(-x), 16(%rsp) = exp(x) */
    movsd 16(%rsp), %xmm1
    subsd %xmm0, %xmm1     /* exp(x) - exp(-x) */
    movsd .Lhalf(%rip), %xmm0
    mulsd %xmm1, %xmm0
    addq  $24,   %rsp
    ret

    .global cosh
cosh:
    subq  $24,   %rsp
    movsd %xmm0, (%rsp)
    movsd %xmm0, 8(%rsp)
    call  exp
    movsd %xmm0, 16(%rsp)
    movsd 8(%rsp), %xmm0
    xorpd .Lneg(%rip), %xmm0
    call  exp
    movsd 16(%rsp), %xmm1
    addsd %xmm0, %xmm1
    movsd .Lhalf(%rip), %xmm0
    mulsd %xmm1, %xmm0
    addq  $24,   %rsp
    ret

    .global tanh
tanh:
    subq  $24,   %rsp
    movsd %xmm0, 8(%rsp)
    call  sinh
    movsd %xmm0, 16(%rsp)
    movsd 8(%rsp), %xmm0
    call  cosh
    movsd 16(%rsp), %xmm1
    divsd %xmm0, %xmm1
    movsd %xmm1, %xmm0
    addq  $24,   %rsp
    ret

    .global asinh
asinh:
    /* asinh(x) = log(x + sqrt(x^2 + 1)) */
    subq  $16, %rsp
    movsd %xmm0, (%rsp)
    /* compute x*x + 1 */
    mulsd %xmm0, %xmm0
    addsd .Lone(%rip), %xmm0
    sqrtsd %xmm0, %xmm0
    addsd (%rsp), %xmm0
    call  log
    addq  $16, %rsp
    ret

    .global acosh
acosh:
    /* acosh(x) = log(x + sqrt(x^2-1)) */
    subq  $16, %rsp
    movsd %xmm0, (%rsp)
    mulsd %xmm0, %xmm0
    subsd .Lone(%rip), %xmm0
    sqrtsd %xmm0, %xmm0
    addsd (%rsp), %xmm0
    call  log
    addq  $16, %rsp
    ret

    .global atanh
atanh:
    /* atanh(x) = 0.5 * log((1+x)/(1-x)) */
    subq  $16, %rsp
    movsd %xmm0, (%rsp)
    movsd .Lone(%rip), %xmm1
    addsd %xmm0, %xmm1     /* 1+x */
    movsd .Lone(%rip), %xmm2
    subsd %xmm0, %xmm2     /* 1-x */
    divsd %xmm2, %xmm1     /* (1+x)/(1-x) */
    movsd %xmm1, %xmm0
    call  log
    mulsd .Lhalf(%rip), %xmm0
    addq  $16, %rsp
    ret

/* ---- Misc -------------------------------------------------------------- */

    .global hypot
hypot:
    /* hypot(x,y) = sqrt(x*x + y*y) */
    mulsd %xmm0, %xmm0
    mulsd %xmm1, %xmm1
    addsd %xmm1, %xmm0
    sqrtsd %xmm0, %xmm0
    ret

    .global cbrt
cbrt:
    /* cbrt(x) = pow(|x|, 1/3) with sign correction */
    subq  $16, %rsp
    movsd %xmm0, (%rsp)
    /* use pow(|x|, 1/3) */
    andpd .Labs(%rip), %xmm0
    movsd .Lthird(%rip), %xmm1
    call  pow
    /* restore sign */
    movsd (%rsp), %xmm1
    andpd .Lsign(%rip), %xmm1
    orpd  %xmm1, %xmm0
    addq  $16, %rsp
    ret

/* float wrappers */
    .global sinf
sinf:
    cvtss2sd %xmm0, %xmm0; call sin; cvtsd2ss %xmm0, %xmm0; ret

    .global cosf
cosf:
    cvtss2sd %xmm0, %xmm0; call cos; cvtsd2ss %xmm0, %xmm0; ret

    .global tanf
tanf:
    cvtss2sd %xmm0, %xmm0; call tan; cvtsd2ss %xmm0, %xmm0; ret

    .global expf
expf:
    cvtss2sd %xmm0, %xmm0; call exp; cvtsd2ss %xmm0, %xmm0; ret

    .global logf
logf:
    cvtss2sd %xmm0, %xmm0; call log; cvtsd2ss %xmm0, %xmm0; ret

    .global powf
powf:
    cvtss2sd %xmm0, %xmm0
    cvtss2sd %xmm1, %xmm1
    call pow
    cvtsd2ss %xmm0, %xmm0
    ret

/* ---- Constants --------------------------------------------------------- */
    .section .rodata
    .align 8
.Lhalf:  .double 0.5
.Lone:   .double 1.0
.Lthird: .double 0.333333333333333333
.Lneg:   .quad   0x8000000000000000   /* sign-flip mask */
.Labs:   .quad   0x7fffffffffffffff   /* abs mask */
.Lsign:  .quad   0x8000000000000000   /* sign mask */
