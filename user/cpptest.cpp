/* cpptest - smoke test for the VibeOS freestanding C++ runtime. */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <typeinfo>

template <typename T>
static T add(T a, T b) {
    return a + b;
}

class Counter {
public:
    explicit Counter(int value) : value_(value) {
        ++constructed;
    }

    ~Counter() {
        ++destroyed;
    }

    int value() const {
        return value_;
    }

    static int constructed;
    static int destroyed;

private:
    int value_;
};

int Counter::constructed = 0;
int Counter::destroyed = 0;

class Shape {
public:
    virtual ~Shape() {}
    virtual int area() const = 0;
};

class Rect final : public Shape {
public:
    Rect(int w, int h) : w_(w), h_(h) {}
    int area() const override { return w_ * h_; }

private:
    int w_;
    int h_;
};

static Counter g_global_counter(7);

static Counter &local_static_counter() {
    static Counter counter(11);
    return counter;
}

struct ExitReporter {
    ~ExitReporter() {
        std::printf("c++ dtors: constructed=%d destroyed=%d\n",
                    Counter::constructed, Counter::destroyed);
    }
};

static ExitReporter g_reporter;

int main(void) {
    Counter stack_counter(3);
    Counter *heap_counter = new Counter(5);
    Counter *nothrow_counter = new (std::nothrow) Counter(13);
    Shape *shape = new Rect(6, 7);
    Counter &local = local_static_counter();

    std::uint32_t typed = 1234u;

    std::printf("c++ runtime ok\n");
    std::printf("template=%d global=%d stack=%d heap=%d nothrow=%d local=%d area=%d u32=%u\n",
                add<int>(20, 22),
                g_global_counter.value(),
                stack_counter.value(),
                heap_counter->value(),
                nothrow_counter ? nothrow_counter->value() : -1,
                local.value(),
                shape->area(),
                (unsigned)typed);
    std::printf("type=%s\n", typeid(*shape).name());

    delete shape;
    delete nothrow_counter;
    delete heap_counter;
    return 0;
}
