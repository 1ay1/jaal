// tests/platform/conformance.cpp — one suite, every reactor backend.
//
// Each check is a template over a Reactor plus a small per-OS fixture (how
// to make a readable channel, close its writer, and interrupt a wait). A
// backend is correct when it passes all of them; a new backend is added by
// instantiating the suite for it in main().
//
//   1. a wake from another thread interrupts a blocking wait
//   2. many wakes before one wait are ONE wakeup (coalescing)
//   3. timeout 0 polls without blocking; a wait with nothing ready times out
//   4. a huge timeout doesn't wrap to a short one
//   5. readiness is reported with the caller's token
//   6. a closed peer is REPORTED (hangup), not a silent spin
//   7. dropping a registration stops events for that handle
//   8. readiness is reported until drained (level-triggered)
//   9. a registration outliving its reactor is a safe no-op
//  10. (POSIX) EINTR doesn't end a wait early
//  11. (POSIX) wake() works from inside a signal handler

#include <jaal/platform/concepts.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <optional>
#include <thread>

#if defined(_WIN32)
#  include <jaal/platform/windows/wait_reactor.hpp>
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <jaal/platform/posix/poll_reactor.hpp>
#  if defined(__linux__)
#    include <jaal/platform/linux/epoll_reactor.hpp>
#  endif
#  if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#    include <jaal/platform/darwin/kqueue_reactor.hpp>
#    define JAAL_TEST_KQUEUE 1
#  endif
#  include <csignal>
#  include <fcntl.h>
#  include <pthread.h>
#  include <unistd.h>
#endif

using namespace std::chrono_literals;
namespace pf = jaal::platform;
using clk = std::chrono::steady_clock;

namespace {

// ── per-OS fixture: a channel whose read end the reactor can watch ───────
#if defined(_WIN32)
// An anonymous pipe is a byte-mode pipe: exactly the mintty/MSYS2 stdin
// case, watched with watch_pipe.
struct channel {
    HANDLE r = nullptr, w = nullptr;
    channel() { ::CreatePipe(&r, &w, nullptr, 0); }
    ~channel() { close_read(); close_write(); }
    void write1() { DWORD n; ::WriteFile(w, "x", 1, &n, nullptr); }
    void close_read()  { if (r) { ::CloseHandle(r); r = nullptr; } }
    void close_write() { if (w) { ::CloseHandle(w); w = nullptr; } }
    template <class R> auto watch(R& re, std::uint64_t tok) { return re.watch_pipe(r, tok); }
};
#else
struct channel {
    int r = -1, w = -1;
    channel() {
        int p[2];
        if (::pipe(p) == 0) { r = p[0]; w = p[1]; }
        ::fcntl(r, F_SETFL, ::fcntl(r, F_GETFL) | O_NONBLOCK);
    }
    ~channel() { close_read(); close_write(); }
    void write1() { [[maybe_unused]] auto n = ::write(w, "x", 1); }
    void close_read()  { if (r >= 0) { ::close(r); r = -1; } }
    void close_write() { if (w >= 0) { ::close(w); w = -1; } }
    template <class R> auto watch(R& re, std::uint64_t tok) {
        return re.watch(r, pf::interest::read, tok);
    }
};
#endif

template <pf::Reactor R>
int wake_from_thread() {
    auto r = R::create().value();
    auto w = r.waker();
    std::jthread t([w] { std::this_thread::sleep_for(30ms); w.wake(); });
    const auto t0 = clk::now();
    auto res = r.wait(10s).value();
    if (!res.woken) return 1;
    if (clk::now() - t0 > 2s) return 2;
    return 0;
}

template <pf::Reactor R>
int wakes_coalesce() {
    auto r = R::create().value();
    auto w = r.waker();
    for (int i = 0; i < 1000; ++i) w.wake();
    if (!r.wait(0ms).value().woken) return 11;
    if (r.wait(0ms).value().woken) return 12;       // all drained by the first
    return 0;
}

template <pf::Reactor R>
int timeouts() {
    auto r = R::create().value();
    auto t0 = clk::now();
    auto a = r.wait(0ms).value();
    if (!a.timeout || a.woken || a.count != 0) return 21;
    if (clk::now() - t0 > 500ms) return 22;
    t0 = clk::now();
    auto b = r.wait(40ms).value();
    if (!b.timeout) return 23;
    if (clk::now() - t0 < 30ms) return 24;
    return 0;
}

template <pf::Reactor R>
int huge_timeout_doesnt_wrap() {
    // A raw narrowing of a 64-bit timeout wraps. ms::max() happens to wrap
    // to -1 ("forever" to poll), hiding the bug; 2^32 + 50 ms wraps to 50 ms,
    // a "50 days" wait that returns at once. A correct reactor must still
    // be waiting when woken after 300 ms.
    auto r = R::create().value();
    auto w = r.waker();
    std::jthread t([w] { std::this_thread::sleep_for(300ms); w.wake(); });
    const auto t0 = clk::now();
    auto res = r.wait(std::chrono::milliseconds((1LL << 32) + 50)).value();
    if (res.timeout) return 31;
    if (!res.woken) return 32;
    if (clk::now() - t0 < 250ms) return 33;
    std::jthread t2([w] { std::this_thread::sleep_for(30ms); w.wake(); });
    const auto t1 = clk::now();
    auto res2 = r.wait(std::chrono::milliseconds::max()).value();
    if (!res2.woken || clk::now() - t1 < 20ms) return 34;
    return 0;
}

template <pf::Reactor R>
int readiness_with_token() {
    auto r = R::create().value();
    channel c;
    auto reg = c.watch(r, 42).value();
    if (r.wait(0ms).value().count != 0) return 41;
    c.write1();
    auto b = r.wait(1s).value();
    if (b.count != 1 || b.ready[0].token != 42 || !b.ready[0].readable) return 42;
    return 0;
}

template <pf::Reactor R>
int hangup_is_reported() {
    auto r = R::create().value();
    channel c;
    auto reg = c.watch(r, 7).value();
    c.close_write();
    auto a = r.wait(1s).value();
    if (a.count != 1 || a.ready[0].token != 7) return 51;
    if (!a.ready[0].hangup) return 52;
    return 0;
}

template <pf::Reactor R>
int unwatch_on_drop() {
    auto r = R::create().value();
    channel c;
    c.write1();
    {
        auto reg = c.watch(r, 1).value();
        if (r.watched() != 1) return 61;
    }
    if (r.watched() != 0) return 62;
    if (r.wait(0ms).value().count != 0) return 63;

    // One registration per handle, on every backend.
    auto a1 = c.watch(r, 1).value();
    auto dup = c.watch(r, 2);
    if (dup || dup.error().code != std::errc::file_exists) return 64;
    // After unwatching, the handle can be watched again (slot reused).
    a1 = typename R::registration{};
    if (r.watched() != 0) return 65;
    auto a2 = c.watch(r, 3);
    if (!a2 || r.watched() != 1) return 66;
    return 0;
}

template <pf::Reactor R>
int readiness_persists_until_drained() {
    // A reactor reports a handle as ready for as long as it IS ready, not
    // just once when it becomes ready. If the caller reads only part of
    // what's there, the next wait must report it again. Edge-triggered
    // epoll breaks this and the rest of the data sits unread forever.
    auto r = R::create().value();
    channel c;
    auto reg = c.watch(r, 9).value();
    c.write1();
    c.write1();
    auto a = r.wait(1s).value();
    if (a.count != 1 || !a.ready[0].readable) return 71;
    // don't read anything
    auto b = r.wait(200ms).value();
    if (b.timeout || b.count != 1 || b.ready[0].token != 9) return 72;
    return 0;
}

template <pf::Reactor R>
int registration_outlives_reactor() {
    channel c;
    std::optional<typename R::registration> keep;
    {
        auto r = R::create().value();
        keep.emplace(c.watch(r, 1).value());
    }
    keep.reset();                                   // must not touch freed memory
    return 0;
}

#if !defined(_WIN32)
extern "C" void noop_handler(int) {}

template <pf::Reactor R>
int eintr_doesnt_end_wait() {
    struct sigaction sa {};
    sa.sa_handler = noop_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                                // no SA_RESTART
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
    if (!res) return 91;
    if (!res->timeout) return 92;
    if (took < 120ms) return 93;
    return 0;
}

std::atomic<const void*> g_waker{nullptr};
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
#endif

template <pf::Reactor R>
int suite(const char* name) {
    int (*const checks[])() = {
        wake_from_thread<R>, wakes_coalesce<R>, timeouts<R>,
        huge_timeout_doesnt_wrap<R>, readiness_with_token<R>, hangup_is_reported<R>,
        unwatch_on_drop<R>, readiness_persists_until_drained<R>,
        registration_outlives_reactor<R>,
#if !defined(_WIN32)
        eintr_doesnt_end_wait<R>, wake_is_signal_safe<R>,
#endif
    };
    int n = 0;
    for (auto f : checks) {
        if (int r = f()) {
            std::fprintf(stderr, "conformance[%s]: check %d failed\n", name, r);
            return 1;
        }
        ++n;
    }
    std::printf("conformance[%s]: %d checks passed\n", name, n);
    return 0;
}

}  // namespace

int main() {
#if defined(_WIN32)
    return suite<pf::wait_reactor>("wait_reactor");
#else
    int r = suite<pf::poll_reactor>("poll_reactor");
#  if defined(__linux__)
    r |= suite<pf::epoll_reactor>("epoll_reactor");
#  endif
#  if defined(JAAL_TEST_KQUEUE)
    r |= suite<pf::kqueue_reactor>("kqueue_reactor");
#  endif
    return r;
#endif
}
