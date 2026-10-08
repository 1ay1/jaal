// tests/kernel/published_test.cpp — published<T>: swap, keep-alive, CAS.
// Built twice by CMake: once as is, once with JAAL_FORCE_PUBLISHED_MUTEX=1 so
// the libc++ fallback runs on a library that has atomic<shared_ptr>.

#include <jaal/kernel/published.hpp>

#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

namespace {
int fails = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL %s\n", what); ++fails; }
}
}  // namespace

int main() {
    using jaal::kernel::published;

    published<const int> p;
    check(!p.current(), "starts empty");

    p.publish(std::make_shared<const int>(1));
    auto held = p.current();
    p.publish(std::make_shared<const int>(2));
    check(held && *held == 1, "a reader's handle survives a swap");
    check(*p.current() == 2, "new readers see the new object");

    // publish_if: succeeds only against the current object
    auto cur = p.current();
    check(!p.publish_if(held, std::make_shared<const int>(3)), "stale CAS refused");
    check(*p.current() == 2, "refused CAS changed nothing");
    check(p.publish_if(cur, std::make_shared<const int>(4)), "fresh CAS lands");
    check(*p.current() == 4, "landed CAS is visible");

    auto taken = p.take();
    check(taken && *taken == 4 && !p.current(), "take leaves it empty");

    // Concurrent CAS increments: every increment lands exactly once.
    published<const int> counter;
    counter.publish(std::make_shared<const int>(0));
    {
        std::vector<std::jthread> ts;
        for (int t = 0; t < 8; ++t)
            ts.emplace_back([&counter] {
                for (int i = 0; i < 500; ++i)
                    for (;;) {
                        auto c = counter.current();
                        if (counter.publish_if(c, std::make_shared<const int>(*c + 1))) break;
                    }
            });
        std::vector<std::jthread> readers;
        for (int t = 0; t < 4; ++t)
            readers.emplace_back([&counter] {
                int last = 0;
                for (int i = 0; i < 2000; ++i) {
                    const int v = *counter.current();
                    if (v < last) std::abort();   // monotone
                    last = v;
                }
            });
    }
    check(*counter.current() == 8 * 500, "concurrent CAS loses nothing");

    std::printf(fails ? "published_test FAILED\n" : "published_test ok (%s)\n",
                JAAL_ATOMIC_SHARED_PTR ? "atomic" : "mutex");
    return fails ? 1 : 0;
}
