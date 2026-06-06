/*
 * test_malloc.c — TCC dynamic memory test: malloc, free, realloc, calloc.
 * Compile inside VibeOS:  /bin/tcc test_malloc.c -o test_malloc && ./test_malloc
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(expr, msg) do { \
    if (!(expr)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("  ok: %s\n", msg); } \
} while(0)

int main(void) {
    printf("=== test_malloc: dynamic memory ===\n");

    /* malloc basics */
    int *ia = malloc(sizeof(int));
    CHECK(ia != NULL, "malloc(int) != NULL");
    *ia = 42;
    CHECK(*ia == 42, "write through malloc'd pointer");
    free(ia);

    /* malloc zero should work or return NULL (both are valid) */
    void *zero = malloc(0);
    free(zero);
    printf("  ok: malloc(0) handled\n");

    /* malloc array */
    int n = 100;
    int *arr = malloc(n * sizeof(int));
    CHECK(arr != NULL, "malloc(100*int) != NULL");
    for (int i = 0; i < n; i++) arr[i] = i * i;
    int sum = 0;
    for (int i = 0; i < n; i++) sum += arr[i];
    CHECK(sum > 0, "large array computation");
    free(arr);

    /* calloc */
    int *ca = calloc(50, sizeof(int));
    CHECK(ca != NULL, "calloc(50, int) != NULL");
    int all_zero = 1;
    for (int i = 0; i < 50; i++)
        if (ca[i] != 0) { all_zero = 0; break; }
    CHECK(all_zero, "calloc zero-initialized");
    free(ca);

    /* realloc: grow */
    char *s = malloc(8);
    CHECK(s != NULL, "malloc(8) != NULL");
    strcpy(s, "hello");
    CHECK(strcmp(s, "hello") == 0, "string in malloc'd buffer");

    char *s2 = realloc(s, 32);
    CHECK(s2 != NULL, "realloc grow != NULL");
    CHECK(strcmp(s2, "hello") == 0, "realloc preserves data");
    strcat(s2, " world");
    CHECK(strcmp(s2, "hello world") == 0, "realloc extended buffer writable");
    free(s2);

    /* Many small allocations */
    #define N_SMALL 200
    void *small[N_SMALL];
    int ok = 1;
    for (int i = 0; i < N_SMALL; i++) {
        small[i] = malloc(64);
        if (!small[i]) { ok = 0; break; }
        memset(small[i], 0xAB, 64);
    }
    CHECK(ok, "200 × malloc(64) all succeed");
    for (int i = 0; i < N_SMALL; i++) free(small[i]);

    /* Large allocation */
    char *big = malloc(1024 * 1024);  /* 1 MB */
    CHECK(big != NULL, "malloc(1MB) != NULL");
    if (big) {
        /* Touch every page */
        for (int i = 0; i < 1024 * 1024; i += 4096)
            big[i] = (char)(i & 0xFF);
        free(big);
    }

    /* Alloc/free interleave */
    void *a = malloc(128);
    void *b = malloc(256);
    void *c = malloc(512);
    CHECK(a != NULL && b != NULL && c != NULL, "3 interleaved allocs");
    free(b);
    void *d = malloc(128);
    CHECK(d != NULL, "recycle freed 256→128");
    free(a); free(c); free(d);

    printf("=== test_malloc: %d failure(s) ===\n", failures);
    return failures ? 1 : 0;
}
