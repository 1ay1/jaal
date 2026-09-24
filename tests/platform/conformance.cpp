// tests/platform/conformance.cpp — one suite, every reactor backend.
//
// Each check is a template over a Reactor. A backend is correct when it
// passes all of them; a new backend is added by instantiating the suite
// for it at the bottom. The checks are the Reactor contract in executable
// form (platform/concepts.hpp):
//
//   1. a wake from another thread interrupts a blocking wait
//   2. many wakes before one wait are ONE wakeup (coalescing)
//   3. timeout 0 polls without blocking; a wait with nothing ready times out
//   4. a very large timeout doesn't wrap into "forever" or "now"
//   5. readiness is reported with the caller's token
//   6. a closed peer is REPORTED (hangup), not a silent readable-with-0-bytes spin
//   7. dropping a registration stops events for that handle
//   8. a registration outliving its reactor is a safe no-op
//   9. EINTR doesn't end a wait early
//  10. wake() is async-signal-safe: calling it from a signal handler works

#include <jaal/platform/concepts.hpp>

#if defined(__unix__) || defined(__APPLE__)
#  include <jaal/platform/posix/poll_reactor.hpp>
#  include <csignal>
#  include <fcntl.h>
#  include <pthread.h>
#  include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <optional>
#include <thread>

using namespace std::chrono_literals;
namespace pf = jaal::platform;
using clk = std::chrono::steady_clock;

#if defined(__unix__) || defined(__APPLE__)

namespace {

struct pipe_pair {
    int r = -1, w = -1;
    pipe_pair() {
        int p[2];
        if (::pipe(p) == 0) { r = p[0]; w = p[1]; }
        ::fcntl(r, F_SETFL, ::fcntl(r, F_GETFL) | O_NONBLOCK);
    }
    ~pipe_pair() { close_read(); close_write(); }
    void close_read()  { if (r >= 0) { ::close(r); r = -1; } }
    void close_write() { if (w >= 0) { ::close(w); w = -1; } }
};

template <pf::Reactor R>
int wake_from_thread() {
    auto r = R::create().value();
    auto w = r.waker();
    std::jthread t([w] { std::this_thread::sleep_for(30ms); w.wake(); });
    const auto t0 = clk::now();
    auto res = r.wait(10s).value();                          // would block 10 s
    const auto took = clk::now() - t0;
    if (!res.woken) return 1;
    if (took > 2s) return 2;                                 // woken, not timed out
    return 0;
}

template <pf::Reactor R>
int wakes_coalesce() {
    auto r = R::create().value();
    auto w = r.waker();
    for (int i = 0; i < 1000; ++i) w.wake();
    auto a = r.wait(0ms).value();
    if (!a.woken) return 11;
    auto b = r.wait(0ms).value();                            // all drained by the first
    if (b.woken) return 12;
    return 0;
}

template <pf::Reactor R>
int timeouts() {
    auto r = R::create().value();
    auto t0 = clk::now();
    auto a = r.wait(0ms).value();
    if (!a.timeout || a.woken || a.count != 0) return 21;
    if (clk::now() - t0 > 500ms) return 22;                  // 0 means don't block
    t0 = clk::now();
    auto b = r.wait(40ms).value();
    const auto took = clk::now() - t0;
    if (!b.timeout) return 23;
    if (took < 30ms) return 24;                              // actually waited
    return 0;
}

template <pf::Reactor R>
int huge_timeout_doesnt_wrap() {
    // A raw int cast of a 64-bit timeout wraps. milliseconds::max() wraps
    // to -1, which poll() happens to read as "forever", so it hides the
    // bug. The dangerous values wrap to a SMALL POSITIVE number:
    // 2^32 + 50 ms becomes 50 ms, a "wait ~50 days" that returns almost
    // at once. Nothing else is ready, so a correct reactor must still be
    // waiting when we wake it after 300 ms.
    auto r = R::create().value();
    auto w = r.waker();
    std::jthread t([w] { std::this_thread::sleep_for(300ms); w.wake(); });
    const auto t0 = clk::now();
    auto res = r.wait(std::chrono::milliseconds((1LL << 32) + 50)).value();
    const auto took = clk::now() - t0;
    if (res.timeout) return 31;                              // returned on a wrapped 50 ms
    if (!res.woken) return 32;
    if (took < 250ms) return 33;

    // and max() is "a long time", not "now"
    std::jthread t2([w] { std::this_thread::sleep_for(30ms); w.wake(); });
    const auto t1 = clk::now();
    auto res2 = r.wait(std::chrono::milliseconds::max()).value();
    if (!res2.woken || clk::now() - t1 < 20ms) return 34;
    return 0;
}

template <pf::Reactor R>
int readiness_with_token() {
    auto r = R::create().value();
    pipe_pair p;
    auto reg = r.watch(p.r, pf::interest::read, 42).value();
    auto a = r.wait(0ms).value();
    if (a.count != 0) return 41;                             // nothing written yet
    [[maybe_unused]] auto n = ::write(p.w, "x", 1);
    auto b = r.wait(1s).value();
    if (b.count != 1 || b.ready[0].token != 42 || !b.ready[0].readable) return 42;
    return 0;
}

template <pf::Reactor R>
int hangup_is_reported() {
    auto r = R::create().value();
    pipe_pair p;
    auto reg = r.watch(p.r, pf::interest::read, 7).value();
    p.close_write();                                         // peer goes away
    auto a = r.wait(1s).value();
    if (a.count != 1 || a.ready[0].token != 7) return 51;
    if (!a.ready[0].hangup) return 52;
    return 0;
}

template <pf::Reactor R>
int unwatch_on_drop() {
    auto r = R::create().value();
    pipe_pair p;
    [[maybe_unused]] auto n = ::write(p.w, "x", 1);
    {
        auto reg = r.watch(p.r, pf::interest::read, 1).value();
        if (r.watched() != 1) return 61;
    }
    if (r.watched() != 0) return 62;
    auto a = r.wait(0ms).value();
    if (a.count != 0) return 63;                             // no longer reported

    // move-assigning over a live registration unwatches the old one
    auto a1 = r.watch(p.r, pf::interest::read, 1).value();
    auto a2 = r.watch(p.r, pf::interest::read, 2).value();
    a1 = std::move(a2);
    if (r.watched() != 1) return 64;
    return 0;
}

template <pf::Reactor R>
int registration_outlives_reactor() {
    pipe_pair p;
    std::optional<typename R::registration> keep;
    {
        auto r = R::create().value();
        keep.emplace(r.watch(p.r, pf::interest::read, 1).value());
    }                                                        // reactor gone
    keep.reset();                                            // must not touch freed memory
    return 0;
}

// Delivered to a specific thread to interrupt its poll() with EINTR.
extern "C" void noop_handler(int) {}

template <pf::Reactor R>
int eintr_doesnt_end_wait() {
    struct sigaction sa {};
    sa.sa_handler = noop_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                                         // no SA_RESTART: poll gets EINTR
    struct sigaction old {};
    ::sigaction(SIGUSR1, &sa, &old);

    auto r = R::create().value();
    const pthread_t me = ::pthread_self();
    std::jthread t([me] {
        for (int i = 0; i < 5; ++i) {
            std::this_thread::sleep_for(10ms);
            ::pthread_kill(me, SIGUSR1);
        }
    });
    const auto t0 = clk::now();
    auto res = r.wait(150ms);
    const auto took = clk::now() - t0;
    t.join();
    ::sigaction(SIGUSR1, &old, nullptr);
    if (!res) return 91;                                     // EINTR must not be an error
    if (!res->timeout) return 92;
    if (took < 120ms) return 93;                             // didn't return at the first signal
    return 0;
}

// wake() from inside a signal handler.
std::atomic<void*> g_waker{nullptr};
template <class W>
void wake_from_signal(int) { static_cast<const W*>(g_waker.load())->wake(); }

template <pf::Reactor R>
int wake_is_signal_safe() {
    auto r = R::create().value();
    auto w = r.waker();
    g_waker = &w;
    struct sigaction sa {};
    sa.sa_handler = &wake_from_signal<typename R::waker_ref>;
    sigemptyset(&sa.sa_mask);
    struct sigaction old {};
    ::sigaction(SIGUSR2, &sa, &old);
    const pthread_t me = ::pthread_self();
    std::jthread t([me] { std::this_thread::sleep_for(20ms); ::pthread_kill(me, SIGUSR2); });
    auto res = r.wait(5s);
    t.join();
    ::sigaction(SIGUSR2, &old, nullptr);
    g_waker = nullptr;
    if (!res || !res->woken) return 101;
    return 0;
}

template <pf::Reactor R>
int suite(const char* name) {
    int (*const checks[])() = {
        wake_from_thread<R>,   wakes_coalesce<R>,       timeouts<R>,
        huge_timeout_doesnt_wrap<R>, readiness_with_token<R>, hangup_is_reported<R>,
        unwatch_on_drop<R>,    registration_outlives_reactor<R>,
        eintr_doesnt_end_wait<R>, wake_is_signal_safe<R>,
    };
    for (auto f : checks)
        if (int r = f()) {
            std::fprintf(stderr, "conformance[%s]: check %d failed\n", name, r);
            return 1;
        }
    return 0;
}

}  // namespace

int main() {
    return suite<pf::poll_reactor>("poll_reactor");
}

#else
int main() { return 0; }   // POSIX backends only for now; windows backend next
#endif
