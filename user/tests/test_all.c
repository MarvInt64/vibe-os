/*
 * test_all.c — TCC comprehensive test runner.
 * Compile inside VibeOS:
 *   /bin/tcc test_all.c -o test_all && ./test_all
 *
 * Tests: arithmetic, floats, structs, malloc, strings, file I/O.
 * Returns 0 (all pass) or 1 (at least one failure).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

static int failures = 0;
static int tests   = 0;

#define CHECK(expr, msg) do { \
    tests++; \
    if (!(expr)) { printf("  FAIL #%d: %s\n", tests, msg); failures++; } \
    else { printf("  ok #%d: %s\n", tests, msg); } \
} while(0)

#define CHECK_FEQ(a, b, eps, msg) do { \
    tests++; \
    double da = (a), db = (b); \
    double diff = da > db ? da - db : db - da; \
    if (diff > (eps)) { \
        printf("  FAIL #%d: %s (%.6f vs %.6f, diff=%.6f)\n", tests, msg, da, db, diff); \
        failures++; \
    } else { printf("  ok #%d: %s\n", tests, msg); } \
} while(0)

/* ── helpers ─────────────────────────────────────────────────────── */
static int factorial(int n) {
    return (n <= 1) ? 1 : n * factorial(n - 1);
}

/* ── function pointers ───────────────────────────────────────────── */
typedef int (*cmp_fn)(int, int);
static int add(int a, int b) { return a + b; }
static int mul(int a, int b) { return a * b; }
static int apply(cmp_fn fn, int a, int b) { return fn(a, b); }

/* ── structs ─────────────────────────────────────────────────────── */
struct point { int x, y; };
struct rect  { struct point origin; int w, h; };
struct node  { int value; struct node *next; };

/* ══════════════════════════════════════════════════════════════════ */

static void test_arithmetic(void) {
    printf("── arithmetic ──\n");
    CHECK(1 + 2 == 3, "1+2==3");
    CHECK(7 * 8 == 56, "7*8==56");
    CHECK(100 / 3 == 33, "100/3==33");
    CHECK(100 % 3 == 1, "100%3==1");
    CHECK((-5) + 3 == -2, "negative add");
    CHECK((-10) * (-3) == 30, "negative*negative");
}

static void test_bitwise(void) {
    printf("── bitwise / shifts ──\n");
    CHECK((1 << 10) == 1024, "1<<10==1024");
    CHECK((1024 >> 3) == 128, "1024>>3==128");
    CHECK((0xFF & 0x0F) == 0x0F, "0xFF & 0x0F");
    CHECK((0xF0 | 0x0F) == 0xFF, "0xF0 | 0x0F");
    CHECK((0xFF ^ 0x0F) == 0xF0, "0xFF ^ 0x0F");
    CHECK(~0 == -1, "~0==-1");
}

static void test_64bit(void) {
    printf("── 64-bit integers ──\n");
    long long big = 10000000000LL;
    CHECK(big == 10000000000LL, "64-bit literal");
    CHECK(big * 2 == 20000000000LL, "64-bit multiply");
    CHECK(big / 3 == 3333333333LL, "64-bit division");
    unsigned int u = 0xFFFFFFFFu;
    CHECK(u + 1 == 0, "unsigned overflow wrap");
}

static void test_loops(void) {
    printf("── loops ──\n");
    int sum = 0;
    for (int i = 0; i < 10; i++) sum += i;
    CHECK(sum == 45, "for sum 0..9==45");

    int j = 0; sum = 0;
    while (j < 10) { sum += j; j++; }
    CHECK(sum == 45, "while sum 0..9==45");

    j = 0; sum = 0;
    do { sum += j; j++; } while (j < 10);
    CHECK(sum == 45, "do-while sum 0..9==45");
}

static void test_conditionals(void) {
    printf("── conditionals ──\n");
    int a = 5, b = 10;
    CHECK((a < b), "5<10");
    CHECK(!(a > b), "!(5>10)");
    CHECK((a != b), "5!=10");
    CHECK(((a > b ? 1 : 2) == 2), "ternary");
}

static void test_recursion(void) {
    printf("── recursion ──\n");
    CHECK(factorial(0) == 1, "factorial(0)==1");
    CHECK(factorial(5) == 120, "factorial(5)==120");
    CHECK(factorial(10) == 3628800, "factorial(10)==3628800");
}

static void test_floats(void) {
    printf("── floats ──\n");
    CHECK_FEQ(1.5 + 2.5, 4.0, 0.0001, "1.5+2.5");
    CHECK_FEQ(2.5 * 4.0, 10.0, 0.0001, "2.5*4.0");
    CHECK_FEQ(10.0 / 4.0, 2.5, 0.0001, "10.0/4.0");
    CHECK_FEQ(0.1 + 0.2, 0.3, 0.0001, "0.1+0.2≈0.3");
    CHECK((int)3.9 == 3, "float→int trunc");
    CHECK((int)(-3.9) == -3, "negative trunc");
}

static void test_math_h(void) {
    printf("── math.h ──\n");
    CHECK_FEQ(sqrt(4.0), 2.0, 0.0001, "sqrt(4)");
    CHECK_FEQ(sqrt(2.0), 1.41421356, 0.0001, "sqrt(2)");
    CHECK_FEQ(pow(2.0, 10.0), 1024.0, 0.0001, "pow(2,10)");
    CHECK_FEQ(fabs(-5.5), 5.5, 0.0001, "fabs");
    CHECK_FEQ(ceil(2.3), 3.0, 0.0001, "ceil");
    CHECK_FEQ(floor(2.7), 2.0, 0.0001, "floor");
    CHECK_FEQ(round(2.5), 3.0, 0.0001, "round");
    CHECK_FEQ(sin(0.0), 0.0, 0.0001, "sin(0)");
    CHECK_FEQ(cos(0.0), 1.0, 0.0001, "cos(0)");
    CHECK_FEQ(log(1.0), 0.0, 0.0001, "log(1)");
    CHECK_FEQ(exp(0.0), 1.0, 0.0001, "exp(0)");
    CHECK_FEQ(log10(1000.0), 3.0, 0.0001, "log10(1000)");
}

static void test_structs(void) {
    printf("── structs ──\n");
    struct point p = { 10, 20 };
    CHECK(p.x == 10 && p.y == 20, "struct init");
    CHECK(sizeof(struct point) == 8, "sizeof(point)");

    struct point q = p;
    q.x = 99;
    CHECK(p.x == 10, "copy independence");

    struct rect r = { {5, 5}, 100, 200 };
    CHECK(r.origin.x == 5 && r.w == 100, "nested struct");

    struct point *pp = &p;
    CHECK(pp->x == 10, "ptr->member");
    pp->x = 30;
    CHECK(p.x == 30, "write through ptr");
}

static void test_pointers(void) {
    printf("── pointers ──\n");
    int arr[] = { 10, 20, 30, 40, 50 };
    int *pa = arr;
    CHECK(*pa == 10, "ptr deref");
    CHECK(*(pa + 2) == 30, "ptr+2");
    pa++;
    CHECK(*pa == 20, "ptr++");
    CHECK(pa[2] == 40, "ptr[2]");

    int val = 77;
    int **p2; int *p1 = &val;
    p2 = &p1;
    CHECK(**p2 == 77, "double ptr");

    void *vp = &val;
    CHECK(*(int*)vp == 77, "void* cast");

    CHECK(NULL == 0, "NULL==0");
}

static void test_function_pointers(void) {
    printf("── function pointers ──\n");
    cmp_fn fp = add;
    CHECK(fp(3, 4) == 7, "fn ptr add");
    fp = mul;
    CHECK(fp(3, 4) == 12, "fn ptr mul");
    CHECK(apply(add, 10, 20) == 30, "higher-order add");
    CHECK(apply(mul, 5, 6) == 30, "higher-order mul");
}

static void test_linked_list(void) {
    printf("── linked list ──\n");
    struct node n1 = { 1, NULL }, n2 = { 2, NULL }, n3 = { 3, NULL };
    n1.next = &n2; n2.next = &n3;
    int sum = 0;
    for (struct node *n = &n1; n; n = n->next) sum += n->value;
    CHECK(sum == 6, "list sum==6");
}

static void test_malloc(void) {
    printf("── malloc ──\n");
    int *ia = malloc(sizeof(int));
    CHECK(ia != NULL, "malloc(int)");
    *ia = 42;
    CHECK(*ia == 42, "*malloc");
    free(ia);

    int n = 100;
    int *arr = malloc(n * sizeof(int));
    CHECK(arr != NULL, "malloc(100*int)");
    for (int i = 0; i < n; i++) arr[i] = i * i;
    int sum = 0;
    for (int i = 0; i < n; i++) sum += arr[i];
    CHECK(sum > 0, "large array sum");
    free(arr);
}

static void test_calloc_realloc(void) {
    printf("── calloc / realloc ──\n");
    int *ca = calloc(50, sizeof(int));
    CHECK(ca != NULL, "calloc");
    int all_zero = 1;
    for (int i = 0; i < 50; i++)
        if (ca[i] != 0) { all_zero = 0; break; }
    CHECK(all_zero, "calloc zeroed");
    free(ca);

    char *s = malloc(8);
    strcpy(s, "hello");
    char *s2 = realloc(s, 32);
    CHECK(s2 != NULL, "realloc");
    CHECK(strcmp(s2, "hello") == 0, "realloc preserve");
    strcat(s2, " world");
    CHECK(strcmp(s2, "hello world") == 0, "realloc extend");
    free(s2);
}

static void test_malloc_many(void) {
    printf("── many small allocs ──\n");
    void *small[200];
    int ok = 1;
    for (int i = 0; i < 200; i++) {
        small[i] = malloc(64);
        if (!small[i]) { ok = 0; break; }
        memset(small[i], 0xAB, 64);
    }
    CHECK(ok, "200×malloc(64)");
    for (int i = 0; i < 200; i++) free(small[i]);
}

static void test_malloc_large(void) {
    printf("── large alloc (1 MB) ──\n");
    char *big = malloc(1024 * 1024);
    CHECK(big != NULL, "malloc(1MB)");
    if (big) {
        for (int i = 0; i < 1024 * 1024; i += 4096)
            big[i] = (char)(i & 0xFF);
        free(big);
    }
}

static void test_malloc_interleave(void) {
    printf("── alloc/free interleave ──\n");
    void *a = malloc(128);
    void *b = malloc(256);
    void *c = malloc(512);
    CHECK(a && b && c, "3 interleaved");
    free(b);
    void *d = malloc(128);
    CHECK(d != NULL, "recycle freed→128");
    free(a); free(c); free(d);
}

static void test_strings(void) {
    printf("── strings ──\n");
    CHECK(strlen("hello") == 5, "strlen");
    CHECK(strcmp("abc", "abc") == 0, "strcmp eq");
    CHECK(strcmp("abc", "abd") < 0, "strcmp lt");
    CHECK(strcmp("abd", "abc") > 0, "strcmp gt");
    CHECK(strncmp("abcdef", "abcxyz", 3) == 0, "strncmp");

    char buf[64];
    strcpy(buf, "hello");
    CHECK(strcmp(buf, "hello") == 0, "strcpy");
    strcat(buf, " world");
    CHECK(strcmp(buf, "hello world") == 0, "strcat");

    CHECK(strchr("hello", 'e') != NULL, "strchr found");
    CHECK(strchr("hello", 'z') == NULL, "strchr not found");
    CHECK(strstr("hello world", "world") != NULL, "strstr");

    char src[] = "test data";
    char dst[16];
    memcpy(dst, src, sizeof(src));
    CHECK(strcmp(dst, "test data") == 0, "memcpy");

    char block[16];
    memset(block, 0x42, 16);
    int all42 = 1;
    for (int i = 0; i < 16; i++) if (block[i] != 0x42) all42 = 0;
    CHECK(all42, "memset");
}

static void test_ctype(void) {
    printf("── ctype ──\n");
    CHECK(isalpha('A') && isalpha('z'), "isalpha");
    CHECK(!isalpha('5'), "!isalpha(digit)");
    CHECK(isdigit('0') && isdigit('9'), "isdigit");
    CHECK(!isdigit('A'), "!isdigit(alpha)");
    CHECK(isspace(' ') && isspace('\n'), "isspace");
    CHECK(toupper('a') == 'A', "toupper");
    CHECK(tolower('Z') == 'z', "tolower");
}

static void test_printf_formatting(void) {
    printf("── printf formatting ──\n");
    char fmt[64];

    snprintf(fmt, sizeof(fmt), "%d + %d = %d", 2, 3, 5);
    CHECK(strcmp(fmt, "2 + 3 = 5") == 0, "snprintf ints");

    snprintf(fmt, sizeof(fmt), "pi ≈ %.2f", 3.14159);
    CHECK(strcmp(fmt, "pi ≈ 3.14") == 0, "snprintf float");

    snprintf(fmt, sizeof(fmt), "%#x", 255);
    CHECK(strcmp(fmt, "0xff") == 0, "snprintf hex");

    char buf[8];
    snprintf(buf, 8, "hello world");
    CHECK(strlen(buf) < 8, "snprintf truncation");
}

static void test_sscanf(void) {
    printf("── sscanf ──\n");
    int a, b;
    int n = sscanf("42 -7", "%d %d", &a, &b);
    CHECK(n == 2 && a == 42 && b == -7, "sscanf ints");

    float pf;
    n = sscanf("3.14", "%f", &pf);
    CHECK(n == 1 && pf > 3.13 && pf < 3.15, "sscanf float");
}

static void test_file_io(void) {
    printf("── file I/O ──\n");
    const char *path = "/tmp/tcc_all_test.txt";
    const char *data = "Hello VibeOS TCC!\nLine two.\n";

    FILE *fp = fopen(path, "w");
    CHECK(fp != NULL, "fopen w");
    size_t wr = fwrite(data, 1, strlen(data), fp);
    CHECK(wr == strlen(data), "fwrite");
    fclose(fp);

    fp = fopen(path, "r");
    CHECK(fp != NULL, "fopen r");
    char buf[128];
    size_t rd = fread(buf, 1, sizeof(buf)-1, fp);
    buf[rd] = '\0';
    CHECK(rd == strlen(data), "fread size");
    CHECK(strcmp(buf, data) == 0, "fread content");
    fclose(fp);

    /* fseek / ftell */
    fp = fopen(path, "r");
    CHECK(fseek(fp, 6, SEEK_SET) == 0, "fseek");
    CHECK(ftell(fp) == 6, "ftell");
    char x[8];
    rd = fread(x, 1, 6, fp); x[rd] = '\0';
    CHECK(strcmp(x, "VibeOS") == 0, "seek read");
    fclose(fp);

    remove(path);
}

/* ══════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("========================================\n");
    printf("  TCC Comprehensive Test Suite\n");
    printf("========================================\n\n");

    test_arithmetic();
    test_bitwise();
    test_64bit();
    test_loops();
    test_conditionals();
    test_recursion();
    test_floats();
    test_math_h();
    test_structs();
    test_pointers();
    test_function_pointers();
    test_linked_list();
    test_malloc();
    test_calloc_realloc();
    test_malloc_many();
    test_malloc_large();
    test_malloc_interleave();
    test_strings();
    test_ctype();
    test_printf_formatting();
    test_sscanf();
    test_file_io();

    printf("\n========================================\n");
    printf("  %d tests, %d failures\n", tests, failures);
    printf("========================================\n");

    return failures ? 1 : 0;
}
