// bench/bench.cpp — what does jaal's architecture cost?
//
// Measures the paths docs/design.md §11 sets targets for, and the same
// shapes ~/projects/tea (the C prior art) reports, so the numbers compare:
//
//   fold           one Msg through update(), from dispatch: the whole
//                  in-loop round trip (queue, fold, interpret none)
//   fold+effect    same, with update returning an `after` effect
//   cross-thread   N producers sending through Sinks, the loop draining:
//                  mailbox lock + wake + drain + fold, per message
//   step idle      one step() with nothing to do (must not allocate or
//                  syscall; this is the cost of every wakeup)
//   reconcile      subscribe() re-run with an unchanged 8-timer Sub
//
// Build in release (the `release` preset); debug numbers mean nothing.
//   cmake --preset release -DJAAL_BUILD_BENCH=ON && cmake --build --preset release
//   ./build/release/bench/jaal_bench

#include <jaal/jaal.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <variant>
#include <vector>

using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

namespace {

template <class F>
double ns_per(std::size_t n, F&& f) {
    const auto t0 = clk::now();
    f();
    const auto dt = std::chrono::duration<double, std::nano>(clk::now() - t0).count();
    return dt / static_cast<double>(n);
}

void report(const char* name, std::size_t n, double ns) {
    std::printf("  %-14s %10zu msgs  %8.1f ns/msg  (%6.1fM/s)\n", name, n, ns, 1e3 / ns);
}

// ── fold: the in-loop round trip ─────────────────────────────────────────
struct Counter {
    struct Model { std::uint64_t n = 0; };
    struct Inc {};
    using Msg = std::variant<Inc>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model& m, Inc) { ++m.n; return {}; }
};

void bench_fold() {
    constexpr std::size_t N = 5'000'000;
    jaal::kernel::options opt;
    opt.fold_budget = N;                              // one step folds everything
    jaal::headless<Counter> h(opt);
    auto& k = h.kernel();
    const double ns = ns_per(N, [&] {
        for (std::size_t i = 0; i < N; ++i) k.dispatch(Counter::Inc{});
        k.step(h.record());
    });
    if (h.model().n != N) std::printf("  !! fold lost messages\n");
    report("fold", N, ns);
}

// ── fold + effect ────────────────────────────────────────────────────────
struct WithEffect {
    struct Model { std::uint64_t n = 0; };
    struct Inc {};
    using Msg = std::variant<Inc>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model& m, Inc) {
        ++m.n;
        return Cmd::after(1h, Inc{});                  // interpreted: a timer push
    }
};

void bench_fold_effect() {
    constexpr std::size_t N = 1'000'000;
    jaal::kernel::options opt;
    opt.fold_budget = N;
    jaal::headless<WithEffect> h(opt);
    auto& k = h.kernel();
    const double ns = ns_per(N, [&] {
        for (std::size_t i = 0; i < N; ++i) k.dispatch(WithEffect::Inc{});
        k.step(h.record());
    });
    report("fold+effect", N, ns);
}

// ── cross-thread ─────────────────────────────────────────────────────────
void bench_cross_thread() {
    constexpr std::size_t P = 4, PER = 250'000, N = P * PER;
    jaal::kernel::options opt;
    opt.fold_budget = 1u << 20;
    jaal::headless<Counter> h(opt);
    auto& k = h.kernel();
    auto sink = h.sink();
    const double ns = ns_per(N, [&] {
        std::vector<std::jthread> ps;
        for (std::size_t p = 0; p < P; ++p)
            ps.emplace_back([sink] { for (std::size_t i = 0; i < PER; ++i) sink.send(Counter::Inc{}); });
        while (h.model().n < N) k.step(h.record());
    });
    report("cross-thread", N, ns);
    std::printf("  %-14s %s\n", "", "(4 producers; per message: lock, maybe wake, drain, fold)");
}

// ── step with nothing to do ──────────────────────────────────────────────
void bench_idle_step() {
    constexpr std::size_t N = 5'000'000;
    jaal::headless<Counter> h;
    auto& k = h.kernel();
    const double ns = ns_per(N, [&] {
        for (std::size_t i = 0; i < N; ++i) k.step(h.record());
    });
    std::printf("  %-14s %10zu steps %8.1f ns/step\n", "step idle", N, ns);
}

// ── reconcile an unchanged subscription ──────────────────────────────────
struct Timers {
    struct Model { std::uint64_t n = 0; };
    struct Tick {}; struct Poke {};
    using Msg = std::variant<Tick, Poke>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;
    template <class M> static Cmd update(Model& m, M) { ++m.n; return {}; }
    static Sub subscribe(const Model&) {
        std::vector<Sub> v;
        for (int i = 1; i <= 8; ++i) v.push_back(Sub::every(std::chrono::milliseconds(100 * i), Tick{}));
        return Sub::batch(std::move(v));
    }
};

void bench_reconcile() {
    constexpr std::size_t N = 200'000;
    jaal::headless<Timers> h;
    auto& k = h.kernel();
    const double ns = ns_per(N, [&] {
        for (std::size_t i = 0; i < N; ++i) {
            k.dispatch(Timers::Poke{});                // model changes → subscribe re-runs
            k.step(h.record());
        }
    });
    std::printf("  %-14s %10zu msgs  %8.1f ns/msg  (fold + subscribe + 8-key diff)\n",
                "reconcile", N, ns);
}

// ── simulation: whole scenarios per second ───────────────────────────────
// A search box: 3 keystrokes, 3 tasks with random latency, a timer, 2
// invariants checked after every message. One run = one seed.
struct Search {
    struct Model { int query = 0; int shown = 0; };
    struct Type { int q; }; struct Results { int q; };
    using Msg = std::variant<Type, Results>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model& m, Type t) {
        m.query = t.q;
        return Cmd::task([](jaal::Sink<Msg> out, std::stop_token, int q) {
                             out.send(Results{q});
                         }, t.q);
    }
    static Cmd update(Model& m, Results r) {
        if (r.q == m.query) m.shown = m.query;
        return {};
    }
};

void bench_sim() {
    constexpr std::size_t N = 20'000;
    const double ns = ns_per(N, [&] {
        auto r = jaal::explore<Search>(1, N, {}, [](jaal::sim<Search>& s) {
            s.at(0ms, Search::Type{1});
            s.at(2ms, Search::Type{2});
            s.at(4ms, Search::Type{3});
            s.check("shown <= query", [](const Search::Model& m) { return m.shown <= m.query; });
        });
        if (!r.ok()) std::printf("  !! sim broke\n");
    });
    std::printf("  %-14s %10zu runs  %8.1f us/run  (%6.0f scenarios/s)\n", "sim", N,
                ns / 1e3, 1e9 / ns);
}

// ── reconcile, as the subscription count grows ─────────────────────────
// The number that matters isn't the absolute cost, it's the SHAPE: ns per
// timer must stay flat. Three separate quadratic terms used to live on this
// path (the reconciler's duplicate scan, its ordinal scan, and the timer
// heap's replace_payload), and the symptom was the same each time — a UI
// with one subscription per visible row got slower the more it showed. At
// 8 -> 256 timers that was 32x the work for 295x the time.
//
// If a future change reintroduces one, the per-timer column climbs and this
// benchmark says so.
template <int N>
struct ManyTimers {
    struct Model { std::uint64_t n = 0; };
    struct Tick {}; struct Poke {};
    using Msg = std::variant<Tick, Poke>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;
    template <class M> static Cmd update(Model& m, M) { ++m.n; return {}; }
    static Sub subscribe(const Model&) {
        std::vector<Sub> v;
        v.reserve(N);
        // Distinct intervals: N distinct ordinal bases, the worst case.
        for (int i = 1; i <= N; ++i)
            v.push_back(Sub::every(std::chrono::milliseconds(100 * i), Tick{}));
        return Sub::batch(std::move(v));
    }
};

template <int N>
void bench_reconcile_at(std::size_t iters) {
    jaal::headless<ManyTimers<N>> h;
    auto& k = h.kernel();
    k.dispatch(typename ManyTimers<N>::Poke{});     // warm: start the timers
    k.step(h.record());
    const double ns = ns_per(iters, [&] {
        for (std::size_t i = 0; i < iters; ++i) {
            k.dispatch(typename ManyTimers<N>::Poke{});
            k.step(h.record());
        }
    });
    std::printf("  %-14s %10d subs  %9.1f ns/reconcile  %7.1f ns/sub\n",
                "  scaling", N, ns, ns / N);
}

void bench_reconcile_scaling() {
    bench_reconcile_at<16>(50'000);
    bench_reconcile_at<64>(20'000);
    bench_reconcile_at<256>(5'000);
}

}  // namespace

int main() {
    std::printf("jaal bench (build: %s)\n",
#ifdef NDEBUG
                "release"
#else
                "DEBUG: numbers are not meaningful"
#endif
    );
    bench_fold();
    bench_fold_effect();
    bench_cross_thread();
    bench_idle_step();
    bench_reconcile();
    bench_reconcile_scaling();
    bench_sim();
    return 0;
}
