/*
 * test_float.c — TCC floating-point test: float/double arithmetic, math.h.
 * Compile inside VibeOS:  /bin/tcc test_float.c -o test_float && ./test_float
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static int failures = 0;

#define CHECK(expr, msg) do { \
    if (!(expr)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("  ok: %s\n", msg); } \
} while(0)

#define CHECK_FEQ(a, b, eps, msg) do { \
    double da = (a), db = (b); \
    double diff = da > db ? da - db : db - da; \
    if (diff > (eps)) { \
        printf("FAIL: %s (%.6f vs %.6f, diff=%.6f)\n", msg, da, db, diff); \
        failures++; \
    } else { printf("  ok: %s\n", msg); } \
} while(0)

int main(void) {
    printf("=== test_float: floating-point arithmetic ===\n");

    /* Basic float ops */
    CHECK_FEQ(1.5 + 2.5, 4.0, 0.0001, "1.5+2.5==4.0");
    CHECK_FEQ(10.0 - 3.5, 6.5, 0.0001, "10.0-3.5==6.5");
    CHECK_FEQ(2.5 * 4.0, 10.0, 0.0001, "2.5*4.0==10.0");
    CHECK_FEQ(10.0 / 4.0, 2.5, 0.0001, "10.0/4.0==2.5");
    CHECK_FEQ(-3.0 * -2.0, 6.0, 0.0001, "negative * negative");
    CHECK_FEQ(0.1 + 0.2, 0.3, 0.0001, "0.1+0.2≈0.3");

    /* Double precision */
    double pi = 3.141592653589793;
    CHECK_FEQ(pi, 3.141592653589793, 0.000001, "double pi literal");

    /* Float cast */
    float f = 3.14f;
    CHECK_FEQ((double)f, 3.14, 0.01, "float→double cast");

    /* Integer → float */
    CHECK_FEQ((double)42, 42.0, 0.0001, "int→double cast");

    /* Float → integer */
    CHECK((int)3.9 == 3, "float→int trunc 3.9→3");
    CHECK((int)(-3.9) == -3, "float→int trunc -3.9→-3");

    /* math.h functions */
    CHECK_FEQ(sqrt(4.0), 2.0, 0.0001, "sqrt(4)==2");
    CHECK_FEQ(sqrt(2.0), 1.41421356, 0.0001, "sqrt(2)≈1.4142");
    CHECK_FEQ(pow(2.0, 10.0), 1024.0, 0.0001, "pow(2,10)==1024");
    CHECK_FEQ(pow(3.0, 3.0), 27.0, 0.0001, "pow(3,3)==27");
    CHECK_FEQ(fabs(-5.5), 5.5, 0.0001, "fabs(-5.5)==5.5");
    CHECK_FEQ(ceil(2.3), 3.0, 0.0001, "ceil(2.3)==3");
    CHECK_FEQ(floor(2.7), 2.0, 0.0001, "floor(2.7)==2");
    CHECK_FEQ(round(2.5), 3.0, 0.0001, "round(2.5)==3");

    /* Trigonometry */
    CHECK_FEQ(sin(0.0), 0.0, 0.0001, "sin(0)==0");
    CHECK_FEQ(cos(0.0), 1.0, 0.0001, "cos(0)==1");
    CHECK_FEQ(sin(3.1415926535 / 2.0), 1.0, 0.0001, "sin(pi/2)==1");
    CHECK_FEQ(cos(3.1415926535), -1.0, 0.002, "cos(pi)≈-1");

    /* log, exp */
    CHECK_FEQ(log(1.0), 0.0, 0.0001, "log(1)==0");
    CHECK_FEQ(exp(0.0), 1.0, 0.0001, "exp(0)==1");
    CHECK_FEQ(log10(1000.0), 3.0, 0.0001, "log10(1000)==3");

    /* Mixed int/float expressions */
    CHECK_FEQ(10.0 / 3, 3.333333, 0.0001, "int-float mixed div");
    CHECK_FEQ(3 * 2.5, 7.5, 0.0001, "int*float");

    /* Comparison */
    CHECK((1.5 < 2.0), "float < comparison");
    CHECK(!(2.0 < 1.5), "float !( < )");
    CHECK((2.0 >= 1.99999), "float >=");

    printf("=== test_float: %d failure(s) ===\n", failures);
    return failures ? 1 : 0;
}
