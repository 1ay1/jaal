// tests/kernel/delay_test.cpp — jaal::kernel::delay_for.
//
// Three properties, and the middle one is the whole point of the function:
//
//   1. an elapsed delay returns false (it did NOT stop) and really waited
//   2. a stop DURING the wait cuts it short and returns true — promptly,
//      not at the end of the duration. This is what the hand-rolled
//      mutex+cv+flag version gets wrong when someone forgets the notify:
//      the job still sleeps out its full delay and only then notices.
//   3. an already-stopped token returns true without waiting at all
//
// Check 2 is timed with a generous bound. The assertion is not "it woke in
// under 20ms" (that is a scheduler bet); it is "it woke in far less than the
// delay it was asked for", which is the behavioural claim and is robust on a
// loaded machine.

#include <jaal/kernel/delay.hpp>

#include <chrono>
#include <cstdio>
#include <stop_token>
#include <thread>

using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

namespace {

int failures = 0;

void ok(bool cond, const char* what) {
    if (!cond) {
        std::fprintf(stderr, "FAIL %s\n", what);
        ++failures;
    }
}

long long ms_since(clk::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0)
        .count();
}

}  // namespace

int main() {
    // 1. A delay that is allowed to elapse reports "not stopped", and did
    //    actually block. Kept short: this one is on the critical path of the
    //    test's own runtime.
    {
        std::stop_source src;
        const auto       t0      = clk::now();
        const bool       stopped = jaal::kernel::delay_for(src.get_token(), 60ms);
        const auto       waited  = ms_since(t0);
        ok(!stopped, "1. an elapsed delay returns false");
        ok(waited >= 50, "1. and it really waited for the duration");
    }

    // 2. A stop mid-wait wins, and wins EARLY. Asked for 10s; a stop at
    //    ~50ms must return in nothing like 10s.
    {
        std::stop_source src;
        std::jthread     stopper([&src] {
            std::this_thread::sleep_for(50ms);
            src.request_stop();
        });
        const auto t0      = clk::now();
        const bool stopped = jaal::kernel::delay_for(src.get_token(), 10s);
        const auto waited  = ms_since(t0);
        ok(stopped, "2. a stop during the wait returns true");
        ok(waited < 5000,
           "2. and cuts the wait short rather than sleeping it out");
        std::printf("   (woke after %lld ms of a 10000 ms delay)\n", waited);
    }

    // 3. Already stopped: answer immediately, touch no primitives.
    {
        std::stop_source src;
        src.request_stop();
        const auto t0      = clk::now();
        const bool stopped = jaal::kernel::delay_for(src.get_token(), 10s);
        ok(stopped, "3. an already-stopped token returns true");
        ok(ms_since(t0) < 1000, "3. and does not wait at all");
    }

    // A non-positive duration is not a stop: it elapsed, instantly.
    {
        std::stop_source src;
        ok(!jaal::kernel::delay_for(src.get_token(), 0ms),
           "a zero duration elapses rather than reporting a stop");
    }

    if (failures == 0) std::puts("delay: all checks OK");
    return failures == 0 ? 0 : 1;
}
