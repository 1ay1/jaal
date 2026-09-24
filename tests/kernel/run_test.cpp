// tests/kernel/run_test.cpp — jaal::run<P> on the real platform: native
// reactor, real clock, real signals, real threads. Nothing simulated.

#include <jaal/core/core_fx.hpp>
#include <jaal/kernel/run.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <variant>

#if !defined(_WIN32)
#  include <csignal>
#  include <sys/resource.h>
#  include <unistd.h>
#endif

using namespace std::chrono_literals;
using jaal::sig;
using jaal::signal_set;
using clk = std::chrono::steady_clock;

template <class Msg>
using SigSub = jaal::Sub<Msg, jaal::row_union<jaal::core_src, jaal::make_row<jaal::fx::on_signal>>>;

// ── 1. timers, a task, and quit with a code, for real ───────────────────
struct Ticker {
    struct Model { int ticks = 0; int task = 0; };
    struct Tick {}; struct Done { int v; };
    using Msg = std::variant<Tick, Done>;
    using Cmd = jaal::CoreCmd<Msg>;
    using Sub = SigSub<Msg>;
    static std::pair<Model, Cmd> init() {
        return {{}, Cmd::task([](jaal::Sink<Msg> out, std::stop_token, int x) {
            out.send(Done{x * 2});
        }, 21)};
    }
    static std::pair<Model, Cmd> update(Model m, Msg msg) {
        if (auto* d = std::get_if<Done>(&msg)) { m.task = d->v; return {m, Cmd::none()}; }
        ++m.ticks;
        if (m.ticks == 3) return {m, Cmd::quit(m.task == 42 ? 7 : 99)};
        return {m, Cmd::none()};
    }
    static Sub subscribe(const Model&) { return Sub::every(30ms, Tick{}); }
};

static int timers_and_tasks() {
    const auto t0 = clk::now();
    const int code = jaal::run<Ticker>();
    const auto took = clk::now() - t0;
    if (code != 7) return 101;                       // 99 would mean the task never landed
    if (took < 80ms) return 102;                     // 3 ticks at 30ms can't be faster
    if (took > 3s) return 103;
    return 0;
}

// ── 2. an idle program SLEEPS: no busy loop ─────────────────────────────
// Nothing scheduled for 400 ms. The loop must block in the reactor, not
// spin. Measured with process CPU time: a spinning loop burns ~400 ms of
// CPU in 400 ms of wall time; a sleeping one burns almost none.
struct Idle {
    struct Model {};
    struct Wake {};
    using Msg = std::variant<Wake>;
    using Cmd = jaal::CoreCmd<Msg>;
    static std::pair<Model, Cmd> init() { return {{}, Cmd::after(400ms, Wake{})}; }
    static std::pair<Model, Cmd> update(Model m, Msg) { return {m, Cmd::quit(0)}; }
};

#if !defined(_WIN32)
static double cpu_seconds() {
    rusage u{};
    ::getrusage(RUSAGE_SELF, &u);
    return static_cast<double>(u.ru_utime.tv_sec + u.ru_stime.tv_sec)
         + static_cast<double>(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
}
#endif

static int idle_sleeps() {
#if !defined(_WIN32)
    const double c0 = cpu_seconds();
    const auto t0 = clk::now();
    if (jaal::run<Idle>() != 0) return 201;
    const auto wall = clk::now() - t0;
    const double cpu = cpu_seconds() - c0;
    if (wall < 380ms) return 202;
    if (cpu > 0.10) {                                // well under the 0.4 s a spin would burn
        std::fprintf(stderr, "run_test: idle loop used %.3fs CPU in %.3fs\n",
                     cpu, std::chrono::duration<double>(wall).count());
        return 203;
    }
#endif
    return 0;
}

#if !defined(_WIN32)
// ── 3. a subscribed signal becomes a Msg ────────────────────────────────
struct Handles {
    struct Model { bool got_int = false; };
    struct Sig { sig s; }; struct Ready {};
    using Msg = std::variant<Sig, Ready>;
    using Cmd = jaal::CoreCmd<Msg>;
    using Sub = SigSub<Msg>;
    static std::pair<Model, Cmd> init() { return {{}, Cmd::after(20ms, Ready{})}; }
    static std::pair<Model, Cmd> update(Model m, Msg msg) {
        if (std::holds_alternative<Ready>(msg)) {
            ::raise(SIGINT);                         // arrives through the reactor
            return {m, Cmd::none()};
        }
        const auto s = std::get<Sig>(msg).s;
        if (s == sig::interrupt) return {m, Cmd::quit(11)};   // handled: our code
        return {m, Cmd::none()};
    }
    static Sub subscribe(const Model&) {
        return Sub::on_signal({sig::interrupt}, [](sig s) { return Msg{Sig{s}}; });
    }
};

static int subscribed_signal() {
    if (jaal::run<Handles>() != 11) return 301;
    return 0;
}

// ── 4. an UNsubscribed Ctrl+C still stops the program ───────────────────
struct Ignores {
    struct Model {};
    struct Ready {}; struct Never {};
    using Msg = std::variant<Ready, Never>;
    using Cmd = jaal::CoreCmd<Msg>;
    static std::pair<Model, Cmd> init() {
        return {{}, Cmd::batch(Cmd::after(20ms, Ready{}), Cmd::after(5s, Never{}))};
    }
    static std::pair<Model, Cmd> update(Model m, Msg msg) {
        if (std::holds_alternative<Ready>(msg)) ::raise(SIGINT);
        return {m, Cmd::none()};
    }
};

static int default_signal_exit() {
    const auto t0 = clk::now();
    const int code = jaal::run<Ignores>();
    if (code != 130) return 401;                     // 128 + SIGINT, like a shell
    if (clk::now() - t0 > 2s) return 402;            // didn't wait for the 5 s timer
    // and it can be turned off
    jaal::run_options opt;
    opt.default_signal_exit = false;
    struct Ignores2 : Ignores {
        static std::pair<Model, Cmd> update(Model m, Msg msg) {
            if (std::holds_alternative<Ready>(msg)) ::raise(SIGINT);
            return {m, std::holds_alternative<Never>(msg) ? Cmd::quit(0) : Cmd::none()};
        }
        static std::pair<Model, Cmd> init() {
            return {{}, Cmd::batch(Cmd::after(20ms, Ready{}), Cmd::after(150ms, Never{}))};
        }
    };
    if (jaal::run<Ignores2>(opt) != 0) return 403;   // survived the Ctrl+C
    return 0;
}
#endif

// ── 5. tasks outliving the program don't crash the shutdown ─────────────
struct Busy {
    struct Model {};
    struct Go {}; struct Late {};
    using Msg = std::variant<Go, Late>;
    using Cmd = jaal::CoreCmd<Msg>;
    static std::pair<Model, Cmd> init() {
        Cmd work = Cmd::batch(
            Cmd::task(jaal::fx::isolated, [](jaal::Sink<Msg> out, std::stop_token) {
                std::this_thread::sleep_for(150ms);     // still running at shutdown
                out.send(Late{});                       // must just return false
            }),
            Cmd::task([](jaal::Sink<Msg> out, std::stop_token st) {
                while (!st.stop_requested()) std::this_thread::sleep_for(1ms);
                out.send(Late{});
            }));
        return {{}, Cmd::batch(std::move(work), Cmd::after(20ms, Go{}))};
    }
    static std::pair<Model, Cmd> update(Model m, Msg msg) {
        if (std::holds_alternative<Go>(msg)) return {m, Cmd::quit(5)};
        return {m, Cmd::none()};
    }
};

static int shutdown_with_live_tasks() {
    if (jaal::run<Busy>() != 5) return 501;
    // The isolated task is still asleep; give it time to wake and send
    // into the dead mailbox. Nothing may crash (ASan/TSan watch this).
    std::this_thread::sleep_for(250ms);
    return 0;
}

int main() {
    // A check that hangs (a stop that never happens) must FAIL, not stall
    // CI until an outer timeout kills it with no message. 20 s is far more
    // than the whole file needs.
    std::jthread watchdog([](std::stop_token st) {
        for (int i = 0; i < 200 && !st.stop_requested(); ++i)
            std::this_thread::sleep_for(100ms);
        if (!st.stop_requested()) {
            std::fprintf(stderr, "run_test: a check hung for 20s\n");
            std::_Exit(2);
        }
    });
    int (*const checks[])() = {
        timers_and_tasks, idle_sleeps,
#if !defined(_WIN32)
        subscribed_signal, default_signal_exit,
#endif
        shutdown_with_live_tasks,
    };
    for (auto f : checks)
        if (int r = f()) {
            std::fprintf(stderr, "run_test: check %d failed\n", r);
            return 1;
        }
    return 0;
}
