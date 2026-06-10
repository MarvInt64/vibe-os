/* STL-subset smoke test for the freestanding VibeOS C++ headers.
 * Build (arm64, on-host cross): see Makefile `tcc-vexui`/manual; or run on the
 * VM. Prints PASS/FAIL lines to stdout. */

#include <vector>
#include <string>
#include <memory>
#include <utility>
#include <stdio.h>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (cond) { printf("PASS %s\n", #cond); } \
    else      { printf("FAIL %s\n", #cond); g_fail++; } \
} while (0)

struct Counted {
    static int live;
    int v;
    Counted(int x = 0) : v(x) { ++live; }
    Counted(const Counted &o) : v(o.v) { ++live; }
    Counted(Counted &&o) noexcept : v(o.v) { o.v = -1; ++live; }
    Counted &operator=(const Counted &o) { v = o.v; return *this; }
    Counted &operator=(Counted &&o) noexcept { v = o.v; o.v = -1; return *this; }
    ~Counted() { --live; }
};
int Counted::live = 0;

int main(void) {
    /* ---- vector basics ---- */
    std::vector<int> v;
    for (int i = 0; i < 10; ++i) v.push_back(i * i);
    CHECK(v.size() == 10);
    CHECK(v[3] == 9);
    CHECK(v.back() == 81);
    int sum = 0;
    for (int x : v) sum += x;
    CHECK(sum == 285);

    v.erase(v.begin() + 0);     /* drop the 0 */
    CHECK(v.front() == 1);
    CHECK(v.size() == 9);
    v.insert(v.begin(), 99);
    CHECK(v.front() == 99);

    std::vector<int> w = v;     /* copy */
    CHECK(w == v);
    std::vector<int> m = std::move(w);
    CHECK(m == v);
    CHECK(w.size() == 0);

    /* ---- vector of non-trivial type: no leaks ---- */
    {
        std::vector<Counted> cv;
        for (int i = 0; i < 5; ++i) cv.emplace_back(i);
        cv.resize(3);
        CHECK(cv.size() == 3);
        CHECK(cv[2].v == 2);
    }
    CHECK(Counted::live == 0);

    /* ---- string ---- */
    std::string s = "hello";
    s += ", ";
    s.append("world");
    CHECK(s == "hello, world");
    CHECK(s.size() == 12);
    CHECK(s.substr(7) == "world");
    CHECK(s.find("world") == 7);
    CHECK(s.find('z') == std::string::npos);
    std::string t = s + "!";
    CHECK(t == "hello, world!");
    std::string big(100, 'x');
    CHECK(big.size() == 100);
    CHECK(big[99] == 'x');

    /* ---- vector<string> ---- */
    std::vector<std::string> names;
    names.push_back("alice");
    names.push_back("bob");
    names.emplace_back("carol");
    CHECK(names.size() == 3);
    CHECK(names[1] == "bob");

    /* ---- unique_ptr ---- */
    {
        auto p = std::make_unique<Counted>(42);
        CHECK(p->v == 42);
        CHECK(Counted::live == 1);
        auto q = std::move(p);
        CHECK(!p);
        CHECK(q->v == 42);
    }
    CHECK(Counted::live == 0);

    auto arr = std::make_unique<int[]>(8);
    arr[0] = 7; arr[7] = 11;
    CHECK(arr[0] + arr[7] == 18);

    /* ---- pair ---- */
    auto pr = std::make_pair(1, std::string("one"));
    CHECK(pr.first == 1);
    CHECK(pr.second == "one");

    printf(g_fail == 0 ? "ALL OK\n" : "FAILURES: %d\n", g_fail);
    return g_fail;
}
