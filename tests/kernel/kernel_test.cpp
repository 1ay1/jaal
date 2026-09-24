// tests/kernel/kernel_test.cpp — the kernel, end to end, on the headless
// host and the simulated clock. One section per rule carried over from
// maya's run<P> (docs/design.md 9.2): each is a bug maya actually had.

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
    using Cmd = jaal::Cmd<Msg, beep>;
    static Cmd update(Model& m, Go g) {
        ++m.seen;
        if (g.n == 2) return Cmd::batch(Cmd(Beep{g.n}), Cmd::quit(3), Cmd(Beep{99}));
        return Beep{g.n};
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
    if (!t.quit() || t.exit != 3) return 101;
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
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg, on_key>;
    static Cmd update(Model& m, Open)        { m.picker = true; return {}; }
    static Cmd update(Model& m, EditorKey e) { m.editor += e.c; return {}; }
    static Cmd update(Model& m, PickerKey p) { m.picked += p.c; return {}; }
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
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;
    static Cmd update(Model& m, Inc) { ++m.n; return {}; }
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
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;
    static Cmd update(Model& m, A)     { ++m.a; return {}; }
    static Cmd update(Model& m, B)     { ++m.b; return {}; }
    static Cmd update(Model& m, Other) { ++m.other; return {}; }
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
    using Cmd = jaal::Cmd<Msg>;
    static Cmd init(Model&) {
        return Cmd::after(std::chrono::milliseconds::max(), Tick{});
    }
    static Cmd update(Model& m, Tick) { ++m.fired; return {}; }
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
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model& m, Got g) { m.got += g.v; return {}; }
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
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model&, Start s) {
        return Cmd::task([](Sink<Msg> out, std::stop_token, int n) {
            out.send(Done{n * n});
        }, s.n);
    }
    static Cmd update(Model& m, Done d) { m.results.push_back(d.v); return {}; }
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
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model& m, M) { ++m.n; return {}; }
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

// ── a host with several event kinds (a terminal, a GUI) ──────────────────
// The host's event type is a variant; each router takes one alternative.
// on_key only sees keys, on_click only clicks, and routing stays one event
// at a time with a re-subscribe between (the ^T m o rule still holds).
struct Click { int x, y; };
struct on_click {
    static constexpr std::string_view name = "on_click";
    using event_type = Click;
    template <class Msg> struct type {
        std::function<std::optional<Msg>(const Click&)> filter;
    };
    template <class F, class M>
    static auto fmap(F&& f, type<M> e) -> type<std::invoke_result_t<F, M>> {
        using B = std::invoke_result_t<F, M>;
        return {[g = std::move(e.filter), f = std::forward<F>(f)](const Click& c) -> std::optional<B> {
            if (auto m = g(c)) return f(std::move(*m));
            return std::nullopt;
        }};
    }
    template <class M>
    static std::optional<M> route(const type<M>& p, const Click& c) { return p.filter(c); }
    template <class Self, class Msg> struct ctors {
        template <class F> static Self on_click(F f) { return Self(type<Msg>{std::move(f)}); }
    };
};
using KeyOrClick = std::variant<Key, Click>;

struct TwoKinds {
    struct Model { std::string keys; int clicks = 0; bool clicks_off = false; };
    struct K { char c; }; struct C {};
    using Msg = std::variant<K, C>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg, on_key, on_click>;
    static Cmd update(Model& m, K k) {
        m.keys += k.c;
        if (k.c == 'x') m.clicks_off = true;               // stop listening to clicks
        return {};
    }
    static Cmd update(Model& m, C) { ++m.clicks; return {}; }
    static Sub subscribe(const Model& m) {
        auto keys = Sub::on_key([](const Key& k) -> std::optional<Msg> { return K{k.c}; });
        if (m.clicks_off) return keys;
        return Sub::batch(std::move(keys),
                          Sub::on_click([](const Click&) -> std::optional<Msg> { return C{}; }));
    }
};

static int variant_events() {
    jaal::headless<TwoKinds, KeyOrClick> h;
    h.event(Key{'a'});
    h.event(Click{1, 1});
    h.event(Key{'b'});
    h.event(Click{2, 2});
    if (h.model().keys != "ab" || h.model().clicks != 2) return 901;
    // 'x' turns clicks off; the next click must see the NEW subscription
    h.event(Key{'x'});
    h.event(Click{3, 3});
    if (h.model().clicks != 2) return 902;
    return 0;
}

static_assert(jaal::detail::host_ev::routable_v<KeyOrClick, Key>);
static_assert(jaal::detail::host_ev::routable_v<KeyOrClick, Click>);
static_assert(jaal::detail::host_ev::routable_v<Key, Key>);
static_assert(!jaal::detail::host_ev::routable_v<Key, Click>);
static_assert(!jaal::detail::host_ev::routable_v<std::variant<Key>, Click>);

// ── HostFor: a host must run every effect the program returns ───────────
struct NoBeepHost {
    // handles nothing beyond core
};
static_assert(!jaal::HostFor<NoBeepHost, QuitApp>);             // QuitApp returns Beep
static_assert(jaal::HostFor<NoBeepHost, Echo>);                // Echo only uses core
static_assert(jaal::HostFor<jaal::recorder, QuitApp>);

int main() {
    // Exit codes wrap at 256, so report the code as text.
    int (*const checks[])() = {rule1, rule2, rule3, rule4, rule5, rule6, rule7, budget,
                               variant_events};
    for (auto f : checks)
        if (int r = f()) {
            std::fprintf(stderr, "kernel_test: check %d failed\n", r);
            return 1;
        }
    return 0;
}
