/* threadtest — smoke test for the VibeOS C++ threading library.
 *
 * Exercises the freestanding <thread>, <atomic> and <mutex> implementations:
 *   - std::thread with capturing lambdas (shared address space)
 *   - std::atomic<int> for a lock-free shared counter
 *   - std::mutex + std::lock_guard for a guarded critical section
 *   - join()
 *
 * Run from the shell: `threadtest`. Expect "atomic=10" and "guarded=10".
 * While running, `taskmgr` briefly shows this process with thread_count > 1. */
#include <atomic>
#include <mutex>
#include <thread>
#include <cstdio>
#include <vibeos.h>

int main() {
    std::atomic<int> atomic_counter{0};
    int              guarded_counter = 0;
    std::mutex       mtx;

    std::printf("threadtest: launching 2 std::thread workers\n");

    auto work = [&](int id) {
        for (int i = 0; i < 5; ++i) {
            atomic_counter.fetch_add(1);          /* lock-free */
            {
                std::lock_guard<std::mutex> lock(mtx);  /* guarded */
                ++guarded_counter;
            }
            char msg[48];
            std::snprintf(msg, sizeof msg, "thread %d tick %d", id, i);
            vos_log(VOS_LOG_APP, msg);
            std::this_thread::yield();
        }
    };

    std::thread t1([&] { work(1); });
    std::thread t2([&] { work(2); });

    t1.join();
    t2.join();

    std::printf("threadtest: atomic=%d guarded=%d (expected 10 / 10)\n",
                atomic_counter.load(), guarded_counter);
    return 0;
}
