// tests/kernel/stop_group_test.cpp — jaal::kernel::stop_group.
//
//   1. a member's token is stopped by stop_and_wait, and the wait ends when
//      the member leaves — not at the end of the grace
//   2. join() after stop has begun is REFUSED. This is the check-then-register
//      gap: without it a run starting mid-shutdown is never stopped and never
//      waited for.
//   3. a bounded wait reports the members still running
//   4. a member that outlives the group (abandoned at the grace) leaves
//      safely — it co-owns the state, so its destructor touches live memory
//   5. the token is a REAL std::stop_token: a stop_callback fires on it
//
// Run under ASan in CI; check 4 is the one a sanitizer exists for.

#include <jaal/kernel/stop_group.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <optional>
#include <stop_token>
#include <thread>

using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

namespace {
int failures = 0;
void ok(bool c, const char* w) {
    if (!c) { std::fprintf(stderr, "FAIL %s\n", w); ++failures; }
}
long long ms_since(clk::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count();
}
}  // namespace

int main() {
    // 1. stop reaches a member, and the wait is event-driven.
    {
        jaal::kernel::stop_group g;
        std::atomic<bool> in{false}, saw_stop{false};
        std::thread worker([&] {
            auto m = g.join();
            ok(m.has_value(), "1. join() admits before stop");
            in = true;
            while (!m->stop_requested()) std::this_thread::sleep_for(1ms);
            saw_stop = true;
        });
        while (!in) std::this_thread::yield();
        const auto t0    = clk::now();
        const auto stuck = g.stop_and_wait(10s);
        const auto took  = ms_since(t0);
        worker.join();
        ok(stuck == 0, "1. every member left");
        ok(saw_stop.load(), "1. the member's token was stopped");
        ok(took < 2000, "1. the wait ended when the member left, not at the grace");
    }

    // 2. admission closes with stop.
    {
        jaal::kernel::stop_group g;
        (void)g.stop_and_wait(std::chrono::milliseconds{0});
        ok(!g.join().has_value(), "2. join() after stop is refused");
    }

    // 3 + 4. bounded wait reports a stuck member; that member then outlives
    // the group and must leave without touching freed memory.
    {
        std::atomic<bool> in{false}, release{false};
        std::thread straggler;
        {
            jaal::kernel::stop_group g;
            straggler = std::thread([&] {
                auto m = g.join();   // ignores its token on purpose
                in = true;
                while (!release) std::this_thread::sleep_for(1ms);
                // m leaves HERE, after `g` was destroyed below.
            });
            while (!in) std::this_thread::yield();
            ok(g.stop_and_wait(30ms) == 1, "3. a short grace reports the straggler");
        }   // group destroyed while the member is still in
        release = true;
        straggler.join();
        ok(true, "4. a member that outlived its group left safely");
    }

    // 5. a real stop_token: callbacks fire on it.
    {
        jaal::kernel::stop_group g;
        std::atomic<bool> fired{false};
        auto m = g.join();
        {
            std::stop_callback cb(m->token(), [&] { fired = true; });
            std::thread stopper([&] { (void)g.stop_and_wait(std::chrono::milliseconds{0}); });
            stopper.join();
        }
        ok(fired.load(), "5. a stop_callback registered on the token fires");
        m.reset();
        ok(g.live() == 0, "5. reset() leaves the group");
    }

    if (failures == 0) std::puts("stop_group: all checks OK");
    return failures == 0 ? 0 : 1;
}
