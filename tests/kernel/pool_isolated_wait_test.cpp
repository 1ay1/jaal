// tests/kernel/pool_isolated_wait_test.cpp — shutdown() waits for isolated
// tasks, and is still bounded when one refuses to stop.
//
// THE BUG THIS PINS. post_isolated() detaches its thread, and shutdown()
// used to request stop on it and return immediately. jaal's own memory was
// safe (an abandoned worker only touches the shared `core` it co-owns), but
// an isolated task is exactly where a program puts work that touches its
// OWN world -- and jaal hands back no handle, so the program has no way to
// wait for it either.
//
// agentty hit it: a task_isolated symbol scan walking a
// `static std::vector<std::regex>` while main() returned and the CRT
// destroyed it. TSan named the pair (~_NFA on main vs _M_dfs on the
// worker); users saw an intermittent abort on Linux and 0xC0000005 on
// Windows, ~7 launches in 10 when stdin was already at EOF.
//
// Two properties, and they pull against each other -- which is why both are
// tested:
//
//   1. a BUSY isolated task is waited for. It gets the microseconds it
//      needs to observe its stop_token and leave, so anything it borrowed
//      outlives it.
//   2. a WEDGED isolated task is still abandoned at the deadline. The whole
//      point of `isolated` is that a hung syscall cannot hold the process
//      open; the fix must not trade a race for a hang.

#include <jaal/core/co_owned.hpp>
#include <jaal/kernel/pool.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

using namespace std::chrono_literals;

// What a task and the test share. Atomics only, so it is Sync.
struct Flags {
    std::atomic<bool> inside{false};
    std::atomic<bool> observed_stop{false};
    std::atomic<bool> release{false};
    std::atomic<int>  ran{0};
};
using flags_t = jaal::co_owned<Flags>;

// ── 1. a busy task is waited for ────────────────────────────────────────
// The task holds a flag that stands in for "something the program owns is
// still being read". If shutdown() returns while the flag is set, a real
// program would be destroying that thing right now.
void waits_for_busy_isolated() {
    std::printf("--- shutdown() waits for a busy isolated task ---\n");

    auto f = flags_t::make();
    bool still_inside_at_shutdown = false;

    {
        jaal::kernel::pool p{1};
        p.post_isolated([](std::stop_token st, flags_t f) {
            f->inside.store(true, std::memory_order_release);
            // Cooperative: poll the token the way a scan polls between
            // files. Long enough that an unbounded shutdown() would
            // certainly return first.
            for (int i = 0; i < 2000 && !st.stop_requested(); ++i)
                std::this_thread::sleep_for(1ms);
            f->observed_stop.store(st.stop_requested(), std::memory_order_release);
            f->inside.store(false, std::memory_order_release);
        }, f);

        // Let it actually enter the body before we tear down.
        while (!f->inside.load(std::memory_order_acquire))
            std::this_thread::sleep_for(200us);

        const auto abandoned = p.shutdown(2s);
        still_inside_at_shutdown = f->inside.load(std::memory_order_acquire);
        check(abandoned == 0, "a cooperative task is not reported abandoned");
    }

    check(!still_inside_at_shutdown,
          "shutdown() returned only after the task left its body");
    check(f->observed_stop.load(std::memory_order_acquire),
          "the task saw its stop_token (it was asked, not killed)");
}

// ── 2. a wedged task is still abandoned ─────────────────────────────────
// This is the promise the original design was protecting. A task that never
// checks its token must NOT hold teardown open past the grace.
void abandons_wedged_isolated() {
    std::printf("--- shutdown() stays bounded when a task ignores stop ---\n");

    auto f = flags_t::make();
    std::chrono::milliseconds took{0};
    std::size_t abandoned = 0;

    {
        jaal::kernel::pool p{1};
        p.post_isolated([](std::stop_token, flags_t f) {
            f->inside.store(true, std::memory_order_release);
            // Deliberately ignores the token, like a blocking syscall.
            while (!f->release.load(std::memory_order_acquire))
                std::this_thread::sleep_for(1ms);
        }, f);
        while (!f->inside.load(std::memory_order_acquire))
            std::this_thread::sleep_for(200us);

        const auto t0 = std::chrono::steady_clock::now();
        abandoned = p.shutdown(150ms);
        took = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0);

        // Let the wedged thread finish before the core goes away. It
        // co-owns the core, so this is about keeping the TEST tidy, not
        // about correctness of the pool.
        f->release.store(true, std::memory_order_release);
        std::this_thread::sleep_for(20ms);
    }

    check(took < 1s, "shutdown() returned at the grace, not on the task");
    check(abandoned >= 1, "the wedged task is REPORTED as abandoned");
}

// ── 3. the count is right when nothing is running ───────────────────────
void clean_shutdown_reports_zero() {
    std::printf("--- a pool with no work shuts down clean ---\n");
    jaal::kernel::pool p{2};
    auto f = flags_t::make();
    p.post_isolated([](std::stop_token, flags_t f) { f->ran.fetch_add(1); }, f);
    std::this_thread::sleep_for(30ms);          // let it finish on its own
    check(p.shutdown(1s) == 0, "a finished isolated task is not abandoned");
    check(f->ran.load() == 1, "the task ran exactly once");
}

}  // namespace

int main() {
    std::printf("=== pool_isolated_wait_test ===\n");
    waits_for_busy_isolated();
    abandons_wedged_isolated();
    clean_shutdown_reports_zero();
    if (failures) {
        std::printf("FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
