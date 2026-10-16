// tests/kernel/backstop_test.cpp — the bugs fixed in the concurrency audit.
//
//   1. a lie (thread_root inside a lock body) is caught at the lock or wait
//      it would deadlock on, not left to hang
//   2. a second pool shutdown is still a barrier: it waits for jobs the
//      first one gave up on
//   3. a blocking mailbox post under a held lock is refused, not a hang
//   4. a task that faults after the kernel is gone doesn't touch the reactor
//
// The abort checks fork, so a passing run is the child dying with SIGABRT.

#include <jaal/jaal.hpp>
#include <jaal/core/co_owned.hpp>
#include <jaal/kernel/delay.hpp>
#include <jaal/kernel/pool.hpp>
#include <jaal/kernel/stop_group.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

using namespace std::chrono_literals;
using lo = jaal::rank<10, "test.lo">;
using hi = jaal::rank<20, "test.hi">;

jaal::guarded<int, lo> g_lo{0};
jaal::guarded<int, hi> g_hi{0};

// Runs f in a child with stderr silenced; true if it died of SIGABRT.
template <class F>
bool aborts(F f) {
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) ::dup2(devnull, 2);
        f();
        ::_exit(0);
    }
    int st = 0;
    ::waitpid(pid, &st, 0);
    return WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT;
}

// ── 1. lies are caught ──────────────────────────────────────────────────
void lies() {
    std::printf("--- a forged capability aborts instead of deadlocking ---\n");
    check(aborts([] {
        jaal::thread_root([](const jaal::unlocked& u) {
            g_hi.with(u, [](int&) {
                // the lie: "I hold nothing" inside hi's body
                jaal::thread_root([](const jaal::unlocked& u2) {
                    g_lo.with(u2, [](int& v) { v = 1; });
                });
            });
        });
    }), "thread_root inside a lock body");

    check(aborts([] {
        jaal::thread_root([](const jaal::unlocked& u) {
            g_lo.with(u, [](int&) {
                jaal::kernel::stop_group sg;
                (void)sg.stop_and_wait(0ms);
            });
        });
    }), "stop_group::stop_and_wait under a lock");

    check(aborts([] {
        jaal::thread_root([](const jaal::unlocked& u) {
            g_lo.with(u, [](int&) {
                (void)jaal::kernel::delay_for(std::stop_token{}, 1ms);
            });
        });
    }), "delay_for under a lock");

    // backoff_for is the pause INSIDE an operation a lock is held across
    // (an HTTP retry under a refresh lane): legal under a lock, and capped.
    {
        using clk = std::chrono::steady_clock;
        const auto t0 = clk::now();
        const bool stopped = jaal::thread_root([](const jaal::unlocked& u) {
            return g_lo.with(u, [](int&) {
                return jaal::kernel::backoff_for(std::stop_token{}, 20ms);
            });
        });
        check(!stopped && clk::now() - t0 >= 20ms, "backoff_for under a lock waits, no abort");
        std::stop_source src;
        src.request_stop();
        check(jaal::kernel::backoff_for(src.get_token(), 10s), "backoff_for wakes on stop");
        const auto t1 = clk::now();
        (void)jaal::kernel::backoff_for(std::stop_token{}, 0ms);
        check(clk::now() - t1 < 50ms, "a zero backoff returns at once");
        static_assert(jaal::kernel::kMaxBackoff == std::chrono::milliseconds{2000});
    }

    // and the honest shape still works: hi taken under lo
    const int got = jaal::thread_root([](const jaal::unlocked& u) {
        return g_lo.with(u, [](const jaal::held<lo>& h, int&) {
            return g_hi.with(h, [](int& v) { return ++v; });
        });
    });
    check(got == 1, "declared order still runs");
}

// ── 2. shutdown is a barrier on every call ───────────────────────────────
struct Gate {
    std::atomic<bool> release{false};
    std::atomic<bool> done{false};
};

void shutdown_twice(const jaal::unlocked& cap) {
    std::printf("--- a second shutdown waits for what the first gave up on ---\n");
    auto gate = jaal::co_owned<Gate>::make();
    jaal::kernel::pool p(1);
    p.post([](std::stop_token, jaal::co_owned<Gate> g) {
        while (!g->release.load()) std::this_thread::sleep_for(1ms);
        std::this_thread::sleep_for(20ms);
        g->done.store(true);
    }, gate);
    std::this_thread::sleep_for(10ms);
    const auto stuck = p.shutdown(cap, 5ms);          // gives up: job ignores stop
    check(stuck == 1, "first shutdown abandons the wedged job");
    gate->release.store(true);
    const auto left = p.shutdown(cap, 2s);
    check(left == 0 && gate->done.load(), "second shutdown waits until it's out");
}

// ── 3. a blocking post under a lock is refused ──────────────────────────
struct Echo {
    struct Model { int n = 0; };
    struct Ping {};
    using Msg = std::variant<Ping>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model& m, Ping) { ++m.n; return {}; }
};

void post_under_lock() {
    std::printf("--- a full blocking mailbox refuses a post under a lock ---\n");
    jaal::kernel::options opt;
    opt.mailbox.capacity = 1;
    opt.mailbox.on_full  = jaal::kernel::overflow::block;
    jaal::headless<Echo> h(opt);
    auto sink = h.sink();
    std::atomic<int> sent{0};
    std::thread t([&] {
        jaal::thread_root([&](const jaal::unlocked& u) {
            (void)sink.send(Echo::Ping{});             // fills it
            const bool ok = g_lo.with(u, [](int&, jaal::Sink<Echo::Msg> s) {
                return s.send(Echo::Ping{});           // would wait for the loop
            }, sink);
            sent.store(ok ? 2 : 1);
        });
    });
    t.join();
    check(sent.load() == 1, "refused (false), didn't block");
}

// ── 4. a fault after the kernel is gone ──────────────────────────────────
std::atomic<int> wakes{0};

struct Late {
    struct Model {};
    struct Go {};
    using Msg = std::variant<Go>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd init(Model&) {
        return Cmd::task([](jaal::Sink<Msg>, std::stop_token) {
            std::this_thread::sleep_for(150ms);        // ignore stop, outlive the grace
            throw std::runtime_error("late");
        });
    }
    static Cmd update(Model&, Go) { return {}; }
};

void late_fault() {
    std::printf("--- a task that faults after shutdown doesn't wake anyone ---\n");
    using K = jaal::kernel::kernel<Late, jaal::platform::steady_clock>;
    struct no_host {};
    {
        jaal::kernel::options opt;
        opt.shutdown_grace = 10ms;
        no_host hh;
        // A raw wake standing in for the reactor: it must not be called
        // once the kernel is gone.
        auto k = K::start(hh, {}, opt, [] { wakes.fetch_add(1); });
        jaal::kernel::teardown<K, no_host, jaal::kernel::no_signals> guard{
            k, hh, jaal::kernel::no_signals{}};
        (void)guard.exit_code();
    }
    const int before = wakes.load();
    std::this_thread::sleep_for(300ms);
    check(wakes.load() == before, "no wake after the mailbox closed");
}

}  // namespace

int main() {
    lies();
    jaal::thread_root([](const jaal::unlocked& u) { shutdown_twice(u); });
    post_under_lock();
    late_fault();
    std::printf(failures ? "FAILED (%d)\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
