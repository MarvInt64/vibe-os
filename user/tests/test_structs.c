/*
 * test_structs.c — TCC struct, pointer, and function pointer test.
 * Compile inside VibeOS:  /bin/tcc test_structs.c -o test_structs && ./test_structs
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(expr, msg) do { \
    if (!(expr)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("  ok: %s\n", msg); } \
} while(0)

/* Various struct layouts */
struct point { int x, y; };
struct rect  { struct point origin; int w, h; };
struct node  { int value; struct node *next; };
struct mixed { char c; int i; short s; long long ll; };
struct vec3  { float x, y, z; };

/* Function pointer types */
typedef int (*cmp_fn)(int, int);
typedef void (*callback)(const char *);

static int add(int a, int b) { return a + b; }
static int mul(int a, int b) { return a * b; }
static int apply(cmp_fn fn, int a, int b) { return fn(a, b); }

int main(void) {
    printf("=== test_structs: structs, pointers, function ptrs ===\n");

    /* Basic struct */
    struct point p = { 10, 20 };
    CHECK(p.x == 10, "struct point.x==10");
    CHECK(p.y == 20, "struct point.y==20");
    CHECK(sizeof(struct point) == 8, "sizeof(point)==8");

    /* Struct assignment */
    struct point q = p;
    q.x = 99;
    CHECK(p.x == 10, "struct copy independent (p.x unchanged)");
    CHECK(q.x == 99, "struct copy (q.x==99)");

    /* Nested struct */
    struct rect r = { {5, 5}, 100, 200 };
    CHECK(r.origin.x == 5, "nested struct origin.x");
    CHECK(r.origin.y == 5, "nested struct origin.y");
    CHECK(r.w == 100, "nested struct w");
    CHECK(r.h == 200, "nested struct h");

    /* Struct with mixed types */
    struct mixed m = { 'A', 42, (short)7, 1234567890123LL };
    CHECK(m.c == 'A', "mixed char");
    CHECK(m.i == 42, "mixed int");
    CHECK(m.s == 7, "mixed short");
    CHECK(m.ll == 1234567890123LL, "mixed long long");
    CHECK(sizeof(struct mixed) >= 16, "mixed struct size ≥ 16 (alignment)");

    /* Pointer to struct */
    struct point *pp = &p;
    CHECK(pp->x == 10, "ptr->x");
    CHECK(pp->y == 20, "ptr->y");
    pp->x = 30;
    CHECK(p.x == 30, "write through pointer");

    /* NULL pointer */
    void *np = NULL;
    CHECK(np == NULL, "NULL pointer");
    CHECK(!np, "!NULL is true");

    /* Pointer arithmetic */
    int arr[] = { 10, 20, 30, 40, 50 };
    int *pa = arr;
    CHECK(*pa == 10, "*ptr to arr[0]");
    CHECK(*(pa + 2) == 30, "*(ptr+2) == arr[2]");
    pa++;
    CHECK(*pa == 20, "ptr++ → arr[1]");

    /* Array access via pointer */
    CHECK(pa[0] == 20, "ptr[0] after increment");
    CHECK(pa[2] == 40, "ptr[2] after increment");

    /* LinkedList basic */
    struct node n1 = { 1, NULL }, n2 = { 2, NULL }, n3 = { 3, NULL };
    n1.next = &n2; n2.next = &n3;
    int sum = 0;
    for (struct node *n = &n1; n; n = n->next) sum += n->value;
    CHECK(sum == 6, "linked-list traversal sum==6");

    /* Function pointers */
    cmp_fn fp = add;
    CHECK(fp(3, 4) == 7, "function ptr add(3,4)==7");
    fp = mul;
    CHECK(fp(3, 4) == 12, "function ptr mul(3,4)==12");

    /* Function pointer as argument */
    CHECK(apply(add, 10, 20) == 30, "higher-order apply(add,...)");
    CHECK(apply(mul, 5, 6) == 30, "higher-order apply(mul,...)");

    /* Pointer to pointer */
    int val = 77;
    int *p1 = &val;
    int **p2 = &p1;
    CHECK(**p2 == 77, "double-pointer deref");

    /* Void pointer cast */
    void *vp = &val;
    CHECK(*(int*)vp == 77, "void* cast to int*");

    /* Struct size vs offset */
    CHECK(sizeof(struct vec3) == 12, "sizeof(vec3)==12 (3 floats)");

    printf("=== test_structs: %d failure(s) ===\n", failures);
    return failures ? 1 : 0;
}
