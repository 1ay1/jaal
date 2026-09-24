// tests/kernel/features_test.cpp — streams, now, tracing, replay,
// program<>/router<>/step, and turn's exit representation.

#include <jaal/jaal.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace std::chrono_literals;
using jaal::Sink;

namespace {

// ── streams ──────────────────────────────────────────────────────────────
struct Streamer {
    struct Model { bool on = true; int got = 0; int epoch = 0; };
    struct Item { int epoch; }; struct Toggle {}; struct Rekey {};
    using Msg = std::variant<Item, Toggle, Rekey>;
    using Cmd = jaal::CoreCmd<Msg>;
    using Sub = jaal::CoreSub<Msg>;
    static Model init() { return {}; }
    static std::pair<Model, Cmd> update(Model m, Msg msg) {
        if (auto* i = std::get_if<Item>(&msg)) {
            // A stale stream's message must never arrive here.
            if (i->epoch != m.epoch) std::abort();
            ++m.got;
        } else if (std::holds_alternative<Toggle>(msg)) {
            m.on = !m.on;
        } else {
            ++m.epoch;                                // new key: new run
        }
        return {m, Cmd::none()};
    }
    static Sub subscribe(const Model& m) {
        if (!m.on) return Sub::none();
        return Sub::stream("s" + std::to_string(m.epoch),
            [](Sink<Msg> out, std::stop_token st, int epoch) {
                // ignores the stop token on purpose: its late sends must be
                // dropped by the kernel, not by the body's good manners
                for (int i = 0; i < 50; ++i) {
                    (void)st;
                    std::this_thread::sleep_for(2ms);
                    out.send(Item{epoch});
                }
            }, m.epoch);
    }
};

int streams() {
    jaal::headless<Streamer> h;
    for (int i = 0; i < 30 && h.model().got < 5; ++i) {
        std::this_thread::sleep_for(5ms);
        h.kernel().step(h.record());
    }
    if (h.model().got < 5) return 101;                // it runs
    h.send(Streamer::Rekey{});                        // key changes: old run's sends dropped
    const int at = h.model().got;
    for (int i = 0; i < 40; ++i) { std::this_thread::sleep_for(3ms); h.kernel().step(h.record()); }
    if (h.model().got <= at) return 102;              // the new run delivers
    h.send(Streamer::Toggle{});                       // unsubscribe
    const int off_at = h.model().got;
    for (int i = 0; i < 40; ++i) { std::this_thread::sleep_for(3ms); h.kernel().step(h.record()); }
    if (h.model().got != off_at) return 103;          // nothing after unsubscribing
    return 0;                                         // (Item epoch checks abort on staleness)
}

// ── now ──────────────────────────────────────────────────────────────────
struct Clocked {
    struct Model { std::chrono::nanoseconds seen{-1}; };
    struct Ask {}; struct At { std::chrono::steady_clock::time_point t; };
    using Msg = std::variant<Ask, At>;
    using Cmd = jaal::CoreCmd<Msg>;
    static Model init() { return {}; }
    static std::pair<Model, Cmd> update(Model m, Msg msg) {
        if (std::holds_alternative<Ask>(msg))
            return {m, Cmd::now([](auto t) { return Msg{At{t}}; })};
        m.seen = std::get<At>(msg).t.time_since_epoch();
        return {m, Cmd::none()};
    }
};

int now_uses_kernel_clock() {
    jaal::headless<Clocked> h;
    h.advance(1500ms);                                // SIMULATED time
    h.send(Clocked::Ask{});
    if (h.model().seen != std::chrono::nanoseconds(1500ms)) return 201;
    h.advance(250ms);
    h.send(Clocked::Ask{});
    if (h.model().seen != std::chrono::nanoseconds(1750ms)) return 202;
    return 0;
}

// ── tracing ──────────────────────────────────────────────────────────────
struct Traced {
    struct Model { int n = 0; };
    struct A {}; struct B {};
    using Msg = std::variant<A, B>;
    using Cmd = jaal::CoreCmd<Msg>;
    static Model init() { return {}; }
    static std::pair<Model, Cmd> update(Model m, Msg msg) {
        ++m.n;
        if (std::holds_alternative<B>(msg)) return {m, Cmd::after(1h, A{})};
        return {m, Cmd::none()};
    }
};

int tracing() {
    std::vector<jaal::trace_event> ev;
    jaal::kernel::options opt;
    opt.trace = [&](const jaal::trace_event& e) { ev.push_back(e); };
    jaal::headless<Traced> h(opt);
    h.send(Traced::A{});
    h.send(Traced::B{});
    int folds = 0, effects = 0, steps = 0;
    std::size_t last_index = 99;
    for (auto& e : ev) {
        if (e.kind == jaal::trace_kind::fold)   { ++folds; last_index = e.msg_index; }
        if (e.kind == jaal::trace_kind::effect) { ++effects; if (e.name != "after") return 301; }
        if (e.kind == jaal::trace_kind::step)   ++steps;
    }
    if (folds != 2 || effects != 1 || steps < 2) return 302;
    if (last_index != 1) return 303;                  // B is alternative 1
    // a throwing hook must not break the loop
    jaal::kernel::options bad;
    bad.trace = [](const jaal::trace_event&) { throw 1; };
    jaal::headless<Traced> h2(bad);
    h2.send(Traced::A{});
    if (h2.model().n != 1) return 304;
    return 0;
}

// ── record and replay ────────────────────────────────────────────────────
struct Ledger {
    struct Model {
        std::vector<int> log;
        bool operator==(const Model&) const = default;
    };
    struct Add { int v; }; struct Work {};
    using Msg = std::variant<Add, Work>;
    using Cmd = jaal::CoreCmd<Msg>;
    static Model init() { return {}; }
    static std::pair<Model, Cmd> update(Model m, Msg msg) {
        if (auto* a = std::get_if<Add>(&msg)) { m.log.push_back(a->v); return {m, Cmd::none()}; }
        // an effect whose RESULT comes back as a message: replay must not
        // re-run it, the recorded Add already holds what it produced
        return {m, Cmd::task([](Sink<Msg> out, std::stop_token) { out.send(Add{42}); })};
    }
};

int replay() {
    jaal::recording<Ledger::Msg> rec;
    jaal::headless<Ledger> h({}, rec.hook());
    h.send(Ledger::Add{1});
    h.send(Ledger::Work{});
    if (!h.run_until_idle(2s)) return 401;
    h.send(Ledger::Add{2});
    const auto live = h.model();
    if (live.log != std::vector<int>{1, 42, 2}) return 402;
    if (rec.size() != 4) return 403;                  // Add, Work, Add{42}, Add
    auto again = jaal::replay<Ledger>(rec.messages());
    if (!(again == live)) return 404;                 // same run, no effects executed
    std::size_t seen = 0;
    jaal::replay_each<Ledger>(rec.messages(), [&](std::size_t i, const Ledger::Model&) { seen = i; });
    if (seen != 4) return 405;
    return 0;
}

// ── program<> / router<> / step ─────────────────────────────────────────
struct Key { char c; };
using on_key = jaal::router<Key, "on_key">;

struct KModel { std::string typed; };
using KMsg = std::variant<Key>;

struct Typist : jaal::program<KModel, KMsg, jaal::fx_list<>, jaal::src_list<on_key>> {
    static Model init() { return {}; }
    static step update(Model m, Msg msg) {
        m.typed += std::get<Key>(msg).c;
        if (m.typed == "quit") return {m, Cmd::quit(9)};
        return m;                                     // a Model alone: no effects
    }
    static Sub subscribe(const Model&) {
        return Sub::on(on_key{}, [](const Key& k) { return Msg{k}; });
    }
};
static_assert(jaal::Program<Typist>);
static_assert(std::is_same_v<jaal::fx_of<Typist>, jaal::core_fx>);

int program_aliases() {
    jaal::headless<Typist, Key> h;
    for (char c : std::string("quit")) h.event(Key{c});
    if (h.model().typed != "quit") return 501;
    if (!h.quit()) return 502;
    if (std::move(h).finish() != 9) return 503;
    return 0;
}

// ── turn: exit code only when quitting ──────────────────────────────────
static_assert(!std::is_same_v<decltype(jaal::kernel::turn{}.exit), int>,
              "exit must be optional: a code without a quit can't be represented");
int turn_exit() {
    jaal::kernel::turn t;
    if (t.quit()) return 601;
    t.exit = 3;
    if (!t.quit() || *t.exit != 3) return 602;
    return 0;
}

}  // namespace

int main() {
    std::jthread watchdog([](std::stop_token st) {
        for (int i = 0; i < 200 && !st.stop_requested(); ++i) std::this_thread::sleep_for(100ms);
        if (!st.stop_requested()) { std::fprintf(stderr, "features_test: hung\n"); std::_Exit(3); }
    });
    int (*const checks[])() = {streams, now_uses_kernel_clock, tracing, replay,
                               program_aliases, turn_exit};
    for (auto f : checks)
        if (int r = f()) {
            std::fprintf(stderr, "features_test: check %d failed\n", r);
            return 1;
        }
    return 0;
}
