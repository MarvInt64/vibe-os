/* STL-subset smoke test for the freestanding VibeOS C++ headers.
 * Build (arm64, on-host cross): see Makefile `tcc-vexui`/manual; or run on the
 * VM. Prints PASS/FAIL lines to stdout. */

#include <vector>
#include <string>
#include <memory>
#include <utility>
#include <optional>
#include <functional>
#include <map>
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

    /* ---- optional ---- */
    std::optional<int> oi;
    CHECK(!oi.has_value());
    CHECK(oi.value_or(-1) == -1);
    oi = 7;
    CHECK(oi.has_value());
    CHECK(*oi == 7);
    oi.reset();
    CHECK(!oi);
    {
        std::optional<Counted> oc(std::in_place, 5);
        CHECK(oc->v == 5);
        CHECK(Counted::live == 1);
    }
    CHECK(Counted::live == 0);

    /* ---- function ---- */
    std::function<int(int, int)> add = [](int a, int b) { return a + b; };
    CHECK(add(3, 4) == 7);
    int base = 100;
    std::function<int(int)> addbase = [base](int x) { return x + base; };
    CHECK(addbase(5) == 105);
    std::function<int(int)> copy = addbase;   /* clone */
    CHECK(copy(1) == 101);
    add = nullptr;
    CHECK(!add);

    /* ---- map ---- */
    std::map<std::string, int> ages;
    ages["alice"] = 30;
    ages["bob"]   = 25;
    ages["carol"] = 40;
    CHECK(ages.size() == 3);
    CHECK(ages["bob"] == 25);
    CHECK(ages.at("carol") == 40);
    CHECK(ages.count("dave") == 0);
    CHECK(ages.contains("alice"));
    ages["bob"] += 1;
    CHECK(ages["bob"] == 26);
    /* iteration is in sorted key order */
    std::string order;
    for (auto &kv : ages) { order += kv.first; order += ' '; }
    CHECK(order == "alice bob carol ");
    CHECK(ages.erase("alice") == 1);
    CHECK(ages.size() == 2);
    CHECK(ages.find("alice") == ages.end());

    std::map<int, std::string> im = { {3, "three"}, {1, "one"}, {2, "two"} };
    CHECK(im.size() == 3);
    CHECK(im.begin()->first == 1);     /* sorted */
    CHECK(im[2] == "two");

    printf(g_fail == 0 ? "ALL OK\n" : "FAILURES: %d\n", g_fail);
    return g_fail;
}
