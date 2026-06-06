/*
 * test_basic.c — TCC smoke test: integer arithmetic, conditionals, loops.
 * Compile inside VibeOS:  /bin/tcc test_basic.c -o test_basic && ./test_basic
 * Returns 0 on success, 1 on failure (prints FAIL line).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(expr, msg) do { \
    if (!(expr)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("  ok: %s\n", msg); } \
} while(0)

static int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

int main(void) {
    printf("=== test_basic: arithmetic, conditionals, loops ===\n");

    /* Integer arithmetic */
    CHECK(1 + 2 == 3, "1+2==3");
    CHECK(100 - 50 == 50, "100-50==50");
    CHECK(7 * 8 == 56, "7*8==56");
    CHECK(100 / 3 == 33, "integer division 100/3==33");
    CHECK(100 % 3 == 1, "modulo 100%3==1");
    CHECK((-5) + 3 == -2, "negative addition");
    CHECK((-10) * (-3) == 30, "negative * negative");
    CHECK((1 << 10) == 1024, "left shift 1<<10");
    CHECK((1024 >> 3) == 128, "right shift 1024>>3");
    CHECK((0xFF & 0x0F) == 0x0F, "bitwise AND");
    CHECK((0xF0 | 0x0F) == 0xFF, "bitwise OR");
    CHECK((0xFF ^ 0x0F) == 0xF0, "bitwise XOR");
    CHECK(~0 == -1, "bitwise NOT ~0");

    /* Conditionals */
    int a = 5, b = 10;
    CHECK((a < b), "5 < 10");
    CHECK(!(a > b), "!(5 > 10)");
    CHECK((a == 5), "equality");
    CHECK((a != b), "inequality");
    CHECK((a <= 5), "<= true");
    CHECK((b >= 10), ">= true");

    /* Ternary */
    CHECK(((a > b ? 1 : 2) == 2), "ternary");

    /* Loops */
    int sum = 0;
    for (int i = 0; i < 10; i++) sum += i;
    CHECK(sum == 45, "for-loop sum 0..9 == 45");

    int j = 0; sum = 0;
    while (j < 10) { sum += j; j++; }
    CHECK(sum == 45, "while-loop sum 0..9 == 45");

    j = 0; sum = 0;
    do { sum += j; j++; } while (j < 10);
    CHECK(sum == 45, "do-while sum 0..9 == 45");

    /* Recursion */
    CHECK(factorial(0) == 1, "factorial(0)==1");
    CHECK(factorial(1) == 1, "factorial(1)==1");
    CHECK(factorial(5) == 120, "factorial(5)==120");
    CHECK(factorial(10) == 3628800, "factorial(10)==3628800");

    /* 64-bit integers */
    long long big = 10000000000LL;
    CHECK(big == 10000000000LL, "64-bit literal");
    CHECK(big * 2 == 20000000000LL, "64-bit multiply");
    CHECK(big / 3 == 3333333333LL, "64-bit division");

    /* Unsigned wrap-around */
    unsigned int u = 0xFFFFFFFFu;
    CHECK(u + 1 == 0, "unsigned overflow wrap");

    printf("=== test_basic: %d failure(s) ===\n", failures);
    return failures ? 1 : 0;
}
