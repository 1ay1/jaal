// tests/kernel/kernel_test.cpp — the kernel, end to end, on the headless
// host and the simulated clock. One section per rule carried over from
// maya's run<P> (DESIGN.md 9.2): each is a bug maya actually had.

#include <jaal/core/core_fx.hpp>
#include <jaal/core/program.hpp>
#include <jaal/core/sub.hpp>
#include <jaal/host/headless.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace std::chrono_literals;
using jaal::Sink;
using jaal::make_row;
using jaal::row_union;
namespace fx = jaal::fx;

// A router over a tiny "key" event, like maya's on_key.
struct Key { char c; };
struct on_key {
    static constexpr std::string_view name = "on_key";
    using event_type = Key;
    template <class Msg> struct type {
        std::function<std::optional<Msg>(const Key&)> filter;
    };
    template <class F, class M>
    static auto fmap(F&& f, type<M> e) -> type<std::invoke_result_t<F, M>> {
        using B = std::invoke_result_t<F, M>;
        return {[g = std::move(e.filter), f = std::forward<F>(f)](const Key& k) -> std::optional<B> {
            if (auto m = g(k)) return f(std::move(*m));
            return std::nullopt;
        }};
    }
    template <class M>
    static std::optional<M> route(const type<M>& p, const Key& k) { return p.filter(k); }
    template <class Self, class Msg> struct ctors {
        template <class F> static Self on_key(F f) { return Self(type<Msg>{std::move(f)}); }
    };
};

// A non-core effect, recorded by the headless host.
struct Beep { int n; };
using beep = jaal::pure_fx<Beep, "beep">;

// ── rule 1: quit stops the batch ─────────────────────────────────────────
struct QuitApp {
    struct Model { int seen = 0; };
    struct Go { int n; };
    using Msg = std::variant<Go>;
    using Cmd = jaal::Cmd<Msg, row_union<jaal::core_fx, make_row<beep>>>;
    static Model init() { return {}; }
    static std::pair<Model, Cmd> update(Model m, Msg msg) {
        auto g = std::get<Go>(msg);
        ++m.seen;
        if (g.n == 2) return {m, Cmd::batch(Cmd(Beep{g.n}), Cmd::quit(3), Cmd(Beep{99}))};
        return {m, Cmd(Beep{g.n})};
    }
};
static_assert(jaal::Program<QuitApp>);

static int rule1() {
    jaal::headless<QuitApp> h;
    auto& k = h.kernel();
    k.dispatch(QuitApp::Go{1});
    k.dispatch(QuitApp::Go{2});
    k.dispatch(QuitApp::Go{3});         // queued behind the quit
    auto t = k.step(h.record());
    if (!t.quit || t.exit_code != 3) return 101;
    if (h.model().seen != 2) return 102;                 // Go{3} never folded
    auto beeps = h.effects<beep>();
    // Go{1} beeped; Go{2} beeped, then quit — the Beep{99} after the quit
    // in the SAME batch never ran, and Go{3} never ran at all.
    if (beeps.size() != 2 || beeps[0].n != 1 || beeps[1].n != 2) return 103;
    return 0;
}

// ── rule 2: re-subscribe between events (the "^T m o" bug) ───────────────
// ^T opens a picker. While the picker is open, keys go to it. A fast
// terminal delivers "^T m o" in one read. If all three are routed through
// the subscription from BEFORE ^T, m and o land in the editor.
struct Picker {
    struct Model { bool picker = false; std::string editor, picked; };
    struct Open {}; struct EditorKey { char c; }; struct PickerKey { char c; };
    using Msg = std::variant<Open, EditorKey, PickerKey>;
    using Cmd = jaal::CoreCmd<Msg>;
    using Sub = jaal::Sub<Msg, make_row<on_key>>;
    static Model init() { return {}; }
    static std::pair<Model, Cmd> update(Model m, Msg msg) {
        std::visit(jaal::overload{
            [&](Open)        { m.picker = true; },
            [&](EditorKey e) { m.editor += e.c; },
            [&](PickerKey p) { m.picked += p.c; },
        }, msg);
        return {m, Cmd::none()};
    }
    static Sub subscribe(const Model& m) {
        if (m.picker)
            return Sub::on_key([](const Key& k) -> std::optional<Msg> { return PickerKey{k.c}; });
        return Sub::on_key([](const Key& k) -> std::optional<Msg> {
            if (k.c == '\x14') return Open{};             // ^T
            return EditorKey{k.c};
        });
    }
};

static int rule2() {
    jaal::headless<Picker, Key> h;
    for (char c : std::string("\x14mo")) h.event(Key{c});   // one "read"
    if (!h.model().picker) return 201;
    if (h.model().picked != "mo") return 202;             // went to the picker
    if (!h.model().editor.empty()) return 203;            // NOT to the editor
    return 0;
}

// ── rule 3: subscribe only runs when the model changed ──────────────────
struct CountSubs {
    static inline int subscribe_calls = 0;
    struct Model { int n = 0; };
    struct Inc {};
    using Msg = std::variant<Inc>;
    using Cmd = jaal::CoreCmd<Msg>;
    using Sub = jaal::Sub<Msg, jaal::core_src>;
    static Model init() { return {}; }
    static std::pair<Model, Cmd> update(Model m, Msg) { ++m.n; return {m, Cmd::none()}; }
    static Sub subscribe(const Model&) { ++subscribe_calls; return Sub::none(); }
};

static int rule3() {
    CountSubs::subscribe_calls = 0;
    jaal::headless<CountSubs> h;
    const int after_start = CountSubs::subscribe_calls;
    if (after_start != 1) return 301;
    for (int i = 0; i < 10; ++i) h.kernel().step(h.record());   // idle turns
    if (CountSubs::subscribe_calls != after_start) return 302;
    h.send(CountSubs::Inc{});
    if (CountSubs::subscribe_calls != after_start + 1) return 303;
    return 0;
}

// ── rule 4: timers keep phase; same-interval timers both fire ───────────
struct Clocks {
    struct Model { int a = 0, b = 0, other = 0; bool second = true; };
    struct A {}; struct B {}; struct Other {};
    using Msg = std::variant<A, B, Other>;
    using Cmd = jaal::CoreCmd<Msg>;
    using Sub = jaal::Sub<Msg, jaal::core_src>;
    static Model init() { return {}; }
    static std::pair<Model, Cmd> update(Model m, Msg msg) {
        std::visit(jaal::overload{
            [&](A)     { ++m.a; },
            [&](B)     { ++m.b; },
            [&](Other) { ++m.other; },
        }, msg);
        return {m, Cmd::none()};
    }
    static Sub subscribe(const Model& m) {
        // Two timers with the SAME interval. maya's interval-only key
        // starved the second one forever.
        if (m.second) return Sub::batch(Sub::every(10ms, A{}), Sub::every(10ms, B{}));
        return Sub::every(10ms, A{});
    }
};

static int rule4() {
    jaal::headless<Clocks> h;
    h.advance(35ms);
    if (h.model().a != 3 || h.model().b != 3) return 401;      // both fire
    // A message changes the model (so subscribe re-runs) mid-period. The
    // kept timer must NOT restart its period.
    h.advance(5ms);                                            // t = 40
    h.send(Clocks::Other{});                                   // model changes at t=40
    h.advance(1ms);                                            // t = 41
    if (h.model().a != 4) return 402;                          // the 40ms tick happened
    h.advance(9ms);                                            // t = 50: next tick, on phase
    if (h.model().a != 5) return 403;
    return 0;
}

// ── rule 5: after() with a "never" delay can't overflow ─────────────────
struct Never {
    struct Model { int fired = 0; };
    struct Tick {};
    using Msg = std::variant<Tick>;
    using Cmd = jaal::CoreCmd<Msg>;
    static std::pair<Model, Cmd> init() {
        return {{}, Cmd::after(std::chrono::milliseconds::max(), Tick{})};
    }
    static std::pair<Model, Cmd> update(Model m, Msg) { ++m.fired; return {m, Cmd::none()}; }
};

static int rule5() {
    jaal::headless<Never> h;
    h.advance(24h * 365);
    if (h.model().fired != 0) return 501;                      // didn't wrap into the past
    return 0;
}

// ── rule 6: a Sink can't keep the kernel alive ──────────────────────────
struct Echo {
    struct Model { int got = 0; };
    struct Got { int v; };
    using Msg = std::variant<Got>;
    using Cmd = jaal::CoreCmd<Msg>;
    static Model init() { return {}; }
    static std::pair<Model, Cmd> update(Model m, Msg msg) { m.got += std::get<Got>(msg).v; return {m, Cmd::none()}; }
};

static int rule6() {
    Sink<Echo::Msg> kept;
    {
        jaal::headless<Echo> h;
        kept = h.sink();
        if (!kept.send(Echo::Got{1})) return 601;
        h.run_until_idle();
        if (h.model().got != 1) return 602;
        std::move(h).finish();
    }
    if (kept.send(Echo::Got{2})) return 603;                   // gone: false, no crash
    if (kept.open()) return 604;
    return 0;
}

// ── rule 7: background messages arrive without a wake ───────────────────
struct Worker {
    struct Model { std::vector<int> results; };
    struct Start { int n; }; struct Done { int v; };
    using Msg = std::variant<Start, Done>;
    using Cmd = jaal::CoreCmd<Msg>;
    static Model init() { return {}; }
    static std::pair<Model, Cmd> update(Model m, Msg msg) {
        if (auto* s = std::get_if<Start>(&msg))
            return {m, Cmd::task([](Sink<Msg> out, std::stop_token, int n) {
                out.send(Done{n * n});
            }, s->n)};
        m.results.push_back(std::get<Done>(msg).v);
        return {m, Cmd::none()};
    }
};

static int rule7() {
    // No wake callback at all: the kernel still drains every step.
    jaal::headless<Worker> h;
    for (int i = 1; i <= 5; ++i) h.send(Worker::Start{i});
    if (!h.run_until_idle()) return 701;
    auto r = h.model().results;
    std::sort(r.begin(), r.end());
    if (r != std::vector<int>{1, 4, 9, 16, 25}) return 702;
    return 0;
}

// ── fold budget: a message storm can't starve the host ─────────────────
struct Storm {
    struct Model { int n = 0; };
    struct M {};
    using Msg = std::variant<M>;
    using Cmd = jaal::CoreCmd<Msg>;
    static Model init() { return {}; }
    static std::pair<Model, Cmd> update(Model m, Msg) { ++m.n; return {m, Cmd::none()}; }
};

static int budget() {
    jaal::kernel::options opt;
    opt.fold_budget = 10;
    jaal::headless<Storm> h(opt);
    for (int i = 0; i < 25; ++i) h.kernel().dispatch(Storm::M{});
    auto t = h.kernel().step(h.record());
    if (t.folded != 10 || h.model().n != 10) return 801;       // capped
    h.kernel().step(h.record());
    h.kernel().step(h.record());
    if (h.model().n != 25) return 802;                         // the rest, in later turns
    return 0;
}

// ── HostFor: a host must run every effect the program returns ───────────
struct NoBeepHost {
    // handles nothing beyond core
};
static_assert(!jaal::HostFor<NoBeepHost, QuitApp>);             // QuitApp returns Beep
static_assert(jaal::HostFor<NoBeepHost, Echo>);                // Echo only uses core
static_assert(jaal::HostFor<jaal::recorder, QuitApp>);

int main() {
    // Exit codes wrap at 256, so report the code as text.
    int (*const checks[])() = {rule1, rule2, rule3, rule4, rule5, rule6, rule7, budget};
    for (auto f : checks)
        if (int r = f()) {
            std::fprintf(stderr, "kernel_test: check %d failed\n", r);
            return 1;
        }
    return 0;
}
