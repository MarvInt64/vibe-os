/* Scaling test for the freestanding VibeOS C++ STL subset: a self-contained
 * recursive-descent JSON parser + serializer that stresses <vector>, <string>,
 * <map>, <memory> (unique_ptr) and <functional> under real load — deep
 * recursion, many heap allocations, and move-only values stored in containers
 * (vector<unique_ptr> / map<string, unique_ptr>). Prints PASS/FAIL + "ALL OK".
 *
 * Build: `make test-json-arm64`, then run `/bin/test_json` at the arm64:/$
 * prompt. */

#include <vector>
#include <string>
#include <map>
#include <memory>
#include <functional>
#include <optional>
#include <utility>
#include <stdio.h>

static int g_fail = 0;
#define CHECK(cond) do { \
    if (cond) { /* quiet on success to keep the log readable under load */ } \
    else      { printf("FAIL %s\n", #cond); g_fail++; } \
} while (0)

/* ---- JSON value model (no <variant>, so a tagged struct) ---------------- */
struct JValue;
using JPtr = std::unique_ptr<JValue>;

struct JValue {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool         b = false;
    long         num = 0;                 /* integers only, keeps it self-contained */
    std::string  str;
    std::vector<JPtr>            arr;
    std::map<std::string, JPtr>  obj;

    static JPtr make(Type t) { auto p = std::make_unique<JValue>(); p->type = t; return p; }
};

/* ---- recursive-descent parser ------------------------------------------- */
struct Parser {
    const char *p;
    bool ok = true;

    void skip_ws() { while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') ++p; }

    JPtr parse() { skip_ws(); return value(); }

    JPtr value() {
        skip_ws();
        switch (*p) {
            case '{': return object();
            case '[': return array();
            case '"': { auto v = JValue::make(JValue::Str); v->str = string_lit(); return v; }
            case 't': case 'f': return boolean();
            case 'n': p += 4; return JValue::make(JValue::Null);   /* "null" */
            default:  return number();
        }
    }

    std::string string_lit() {
        std::string s;
        ++p;                                /* opening quote */
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) { ++p; s.push_back(*p); }
            else                    s.push_back(*p);
            ++p;
        }
        if (*p == '"') ++p;
        return s;
    }

    JPtr boolean() {
        auto v = JValue::make(JValue::Bool);
        if (*p == 't') { v->b = true;  p += 4; }   /* "true"  */
        else           { v->b = false; p += 5; }   /* "false" */
        return v;
    }

    JPtr number() {
        auto v = JValue::make(JValue::Num);
        long sign = 1;
        if (*p == '-') { sign = -1; ++p; }
        long n = 0;
        while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); ++p; }
        v->num = sign * n;
        return v;
    }

    JPtr array() {
        auto v = JValue::make(JValue::Arr);
        ++p;                                /* '[' */
        skip_ws();
        if (*p == ']') { ++p; return v; }
        for (;;) {
            v->arr.push_back(value());
            skip_ws();
            if (*p == ',') { ++p; continue; }
            if (*p == ']') { ++p; break; }
            ok = false; break;
        }
        return v;
    }

    JPtr object() {
        auto v = JValue::make(JValue::Obj);
        ++p;                                /* '{' */
        skip_ws();
        if (*p == '}') { ++p; return v; }
        for (;;) {
            skip_ws();
            std::string key = string_lit();
            skip_ws();
            if (*p == ':') ++p; else ok = false;
            v->obj.emplace(key, value());
            skip_ws();
            if (*p == ',') { ++p; continue; }
            if (*p == '}') { ++p; break; }
            ok = false; break;
        }
        return v;
    }
};

/* ---- serializer (compact, sorted keys via map order) -------------------- */
static void num_to_str(long n, std::string &out) {
    if (n == 0) { out.push_back('0'); return; }
    if (n < 0)  { out.push_back('-'); n = -n; }
    char tmp[24]; int i = 0;
    while (n > 0) { tmp[i++] = (char)('0' + n % 10); n /= 10; }
    while (i > 0) out.push_back(tmp[--i]);
}

static std::string serialize(const JValue &v) {
    std::string out;
    switch (v.type) {
        case JValue::Null: out += "null"; break;
        case JValue::Bool: out += v.b ? "true" : "false"; break;
        case JValue::Num:  num_to_str(v.num, out); break;
        case JValue::Str:  out.push_back('"'); out += v.str; out.push_back('"'); break;
        case JValue::Arr: {
            out.push_back('[');
            for (size_t i = 0; i < v.arr.size(); ++i) {
                if (i) out.push_back(',');
                out += serialize(*v.arr[i]);
            }
            out.push_back(']');
            break;
        }
        case JValue::Obj: {
            out.push_back('{');
            bool first = true;
            for (auto &kv : v.obj) {                 /* map iterates sorted */
                if (!first) out.push_back(',');
                first = false;
                out.push_back('"'); out += kv.first; out.push_back('"');
                out.push_back(':');
                out += serialize(*kv.second);
            }
            out.push_back('}');
            break;
        }
    }
    return out;
}

/* ---- generic visitor using std::function (recursion + callbacks) -------- */
static void walk(const JValue &v, const std::function<void(const JValue &)> &fn) {
    fn(v);
    if (v.type == JValue::Arr) for (auto &e : v.arr) walk(*e, fn);
    if (v.type == JValue::Obj) for (auto &kv : v.obj) walk(*kv.second, fn);
}

int main(void) {
    /* ---- 1. round-trip parse → serialize; map sorts keys ---- */
    const char *src =
        "{ \"name\": \"vibeos\", \"version\": 4, \"arch\": [\"x86_64\", \"arm64\"], "
        "\"smp\": true, \"meta\": { \"year\": 2026, \"ok\": null } }";
    Parser pr{src};
    JPtr root = pr.parse();
    CHECK(pr.ok);
    CHECK(root->type == JValue::Obj);

    std::string out = serialize(*root);
    /* keys come out sorted: arch, meta, name, smp, version */
    const char *expect =
        "{\"arch\":[\"x86_64\",\"arm64\"],\"meta\":{\"ok\":null,\"year\":2026},"
        "\"name\":\"vibeos\",\"smp\":true,\"version\":4}";
    CHECK(out == expect);
    if (out != expect) printf("got: %s\n", out.c_str());

    /* ---- 2. navigate the tree ---- */
    CHECK(root->obj.at("name")->str == "vibeos");
    CHECK(root->obj.at("version")->num == 4);
    CHECK(root->obj.at("arch")->arr.size() == 2);
    CHECK(root->obj.at("arch")->arr[1]->str == "arm64");
    CHECK(root->obj.at("meta")->obj.at("year")->num == 2026);
    CHECK(root->obj.contains("smp"));
    CHECK(!root->obj.contains("nope"));

    /* ---- 3. visitor counts node types via std::function ---- */
    std::map<int, int> counts;
    walk(*root, [&counts](const JValue &n) { counts[(int)n.type] += 1; });
    CHECK(counts[(int)JValue::Obj] == 2);   /* root + meta */
    CHECK(counts[(int)JValue::Arr] == 1);
    CHECK(counts[(int)JValue::Str] == 3);   /* vibeos, x86_64, arm64 */
    CHECK(counts[(int)JValue::Num] == 2);   /* version, year */

    /* ---- 4. heavy load: build a deep + wide tree, round-trip it ---- */
    {
        auto big = JValue::make(JValue::Arr);
        for (int i = 0; i < 500; ++i) {
            auto o = JValue::make(JValue::Obj);
            auto id = JValue::make(JValue::Num); id->num = i;
            auto nm = JValue::make(JValue::Str); nm->str = std::string("item") ;
            num_to_str(i, nm->str);
            o->obj.emplace("id", std::move(id));
            o->obj.emplace("name", std::move(nm));
            big->arr.push_back(std::move(o));
        }
        CHECK(big->arr.size() == 500);
        std::string s = serialize(*big);
        Parser p2{s.c_str()};
        JPtr back = p2.parse();
        CHECK(p2.ok);
        CHECK(back->arr.size() == 500);
        CHECK(back->arr[250]->obj.at("id")->num == 250);
        CHECK(back->arr[499]->obj.at("name")->str == "item499");
        /* re-serialize must be byte-identical to the first pass */
        CHECK(serialize(*back) == s);
    }

    /* ---- 5. word frequency over a blob: map<string,int> sorted ---- */
    {
        const char *text = "the cat sat on the mat the cat ran";
        std::map<std::string, int> freq;
        std::string word;
        auto flush = [&]() { if (!word.empty()) { freq[word] += 1; word.clear(); } };
        for (const char *c = text; ; ++c) {
            if (*c == ' ' || *c == '\0') { flush(); if (*c == '\0') break; }
            else word.push_back(*c);
        }
        CHECK(freq["the"] == 3);
        CHECK(freq["cat"] == 2);
        CHECK(freq["mat"] == 1);
        CHECK(freq.size() == 6);            /* the cat sat on mat ran */
        /* iterate in sorted order */
        std::vector<std::string> keys;
        for (auto &kv : freq) keys.push_back(kv.first);
        CHECK(keys.front() == "cat");
        CHECK(keys.back() == "the");
    }

    printf(g_fail == 0 ? "ALL OK\n" : "FAILURES: %d\n", g_fail);
    return g_fail;
}
