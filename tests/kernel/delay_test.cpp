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

#include <jaal/core/co_owned.hpp>
#include <jaal/kernel/delay.hpp>
#include <jaal/kernel/pool.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <stop_token>
#include <thread>

using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

// What a job and the test share. Atomics only, so it is Sync.
struct Flags {
    std::atomic<bool> entered{false};
    std::atomic<bool> finished{false};
};
using flags_t = jaal::co_owned<Flags>;

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

    // pool::no_deadline is a BARRIER: shutdown must not return while a job is
    // still running. The bounded default would return here with the job
    // abandoned, which is exactly the case a host cannot have when the job
    // writes into state the host frees next.
    {
        auto f = flags_t::make();
        {
            jaal::kernel::pool p;
            p.post_isolated([](std::stop_token, flags_t f) {
                f->entered = true;
                // Ignores its stop_token on purpose: a cooperative job would
                // not distinguish a barrier from a short grace.
                std::this_thread::sleep_for(300ms);
                f->finished = true;
            }, f);
            while (!f->entered.load()) std::this_thread::yield();
            const std::size_t stuck = p.shutdown(jaal::kernel::pool::no_deadline);
            ok(stuck == 0, "no_deadline abandons nothing");
            ok(f->finished.load(),
               "no_deadline waited for a job that ignored its stop_token");
        }
    }

    // And the bounded default still bounds — the two must stay distinct, or
    // the option above is decorative.
    {
        auto f = flags_t::make();
        jaal::kernel::pool p;
        p.post_isolated([](std::stop_token, flags_t f) {
            f->entered = true;
            std::this_thread::sleep_for(2s);
        }, f);
        while (!f->entered.load()) std::this_thread::yield();
        const auto        t0    = clk::now();
        const std::size_t stuck = p.shutdown(50ms);
        ok(stuck > 0, "a short grace reports what it abandoned");
        ok(ms_since(t0) < 1500, "and returns without waiting the job out");
    }

    // submit(): a result, without std::async's blocking destructor. This is
    // the property the whole thing exists for, so it is measured: drop the
    // future of a job that is still running and the drop must be instant.
    // std::async would join here.
    {
        jaal::kernel::pool p;
        auto f = flags_t::make();
        const auto t0 = clk::now();
        {
            auto fut = p.submit_isolated([](std::stop_token, flags_t f) {
                f->entered = true;
                std::this_thread::sleep_for(400ms);
                f->finished = true;
                return 7;
            }, f);
            while (!f->entered.load()) std::this_thread::yield();
        }   // future dropped mid-job
        const auto dropped_after = ms_since(t0);
        ok(dropped_after < 200,
           "dropping a submit() future does NOT join the job");
        ok(!f->finished.load(), "and the job really was still running");
        // The job is still owned: shutdown waits for it.
        (void)p.shutdown(jaal::kernel::pool::no_deadline);
        ok(f->finished.load(), "an abandoned result still runs to completion");
    }

    // A value comes back, and a throw comes back as a throw.
    {
        jaal::kernel::pool p;
        auto v = p.submit([](std::stop_token, int x) { return x + 1; }, 41);
        ok(v.get() == 42, "submit returns the body's value");

        auto bad = p.submit([](std::stop_token) -> int { throw std::runtime_error("nope"); });
        bool threw = false;
        try { (void)bad.get(); } catch (const std::runtime_error&) { threw = true; }
        ok(threw, "a throwing body surfaces at get(), not at on_error");

        auto nothing = p.submit([](std::stop_token) {});          // void body
        nothing.get();
        ok(true, "a void body round-trips");
    }

    // Submitting to a stopped pool must not hand back a future that waits
    // forever for a job nobody will run.
    {
        jaal::kernel::pool p;
        (void)p.shutdown();
        auto fut   = p.submit([](std::stop_token) { return 1; });
        bool threw = false;
        try { (void)fut.get(); } catch (const std::future_error&) { threw = true; }
        ok(threw, "a dropped job breaks its promise instead of hanging get()");
    }

    if (failures == 0) std::puts("delay: all checks OK");
    return failures == 0 ? 0 : 1;
}
