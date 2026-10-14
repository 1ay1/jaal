// tests/kernel/input_test.cpp — input through on(const Model&, const Event&).
//
//   1. on() reads the model when the event arrives: the ^T m o case
//   2. overloads pick the event kind; a variant event goes to the overload
//      for the alternative it holds; kinds with no overload are ignored
//   3. signals are input: on(const Model&, jaal::sig)
//   4. on() can return Msg, optional<Msg>, or a Msg case
//   5. a throwing on() is a fault (site: input), and the loop carries on
//   6. composition: child::on and children::on forward to the focused
//      child, which reads ITS model and sees nothing of its siblings

#include <jaal/jaal.hpp>

#include <cstdio>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %-60s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

struct Key   { char c; };
struct Click { int x; };
struct Wheel { int dy; };
using Events = std::variant<Key, Click, Wheel>;

// ── 1-5 ──────────────────────────────────────────────────────────────────
struct Editor {
    struct Model {
        bool        picker = false;
        std::string text, picked;
        int         clicks = 0, quits = 0;
    };
    struct Open {}; struct Type { char c; }; struct Pick { char c; };
    struct Clicked {}; struct Quit {};
    using Msg = std::variant<Open, Type, Pick, Clicked, Quit>;
    using Cmd = jaal::Cmd<Msg>;

    static Cmd update(Model& m, Open)    { m.picker = true; return {}; }
    static Cmd update(Model& m, Type t)  { m.text += t.c; return {}; }
    static Cmd update(Model& m, Pick p)  { m.picked += p.c; return {}; }
    static Cmd update(Model& m, Clicked) { ++m.clicks; return {}; }
    static Cmd update(Model& m, Quit)    { ++m.quits; return {}; }

    // returns a case: converts to Msg
    static Msg on(const Model& m, const Key& k) {
        if (k.c == '!') throw std::runtime_error("bad key");
        if (m.picker)     return Pick{k.c};
        if (k.c == '\x14') return Open{};          // ^T
        return Type{k.c};
    }
    // returns optional: may decline
    static std::optional<Msg> on(const Model& m, const Click&) {
        if (m.picker) return std::nullopt;        // clicks ignored in the picker
        return Clicked{};
    }
    // a signal is just another overload
    static std::optional<Msg> on(const Model&, jaal::sig s) {
        if (s == jaal::sig::interrupt) return Quit{};
        return std::nullopt;
    }
    // (no Wheel overload: wheel events are dropped, not an error)
};

void basics() {
    std::printf("--- on(model, event) ---\n");
    jaal::kernel::options opt;
    opt.on_fault = jaal::fault_policy::skip;
    jaal::headless<Editor, std::variant<Key, Click, Wheel, jaal::sig>> h(opt);

    h.event(Key{'a'});
    h.event(Click{1});
    // ^T m o in one burst: m and o see the model ^T left
    h.event(Key{'\x14'});
    h.event(Key{'m'});
    h.event(Key{'o'});
    h.event(Click{2});                             // declined: picker open
    h.event(Wheel{3});                             // no overload: dropped
    h.event(jaal::sig::interrupt);
    h.event(jaal::sig::child);                     // declined

    const auto& m = h.model();
    check(m.text == "a" && m.picked == "mo", "^T m o: each key sees the model the last one left");
    check(m.clicks == 1, "optional<Msg>: a declined click does nothing");
    check(m.quits == 1, "signals are input: on(const Model&, jaal::sig)");

    h.event(Key{'!'});                             // on() throws
    check(h.kernel().fault_count() == 1, "a throwing on() is a fault");
    h.event(Key{'z'});
    check(h.model().picked == "moz", "and the loop carries on");
}

// ── 6: composition ───────────────────────────────────────────────────────
struct Pane {
    struct Model { std::string typed; };
    struct Typed { char c; };
    using Msg = std::variant<Typed>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model& m, Typed t) { m.typed += t.c; return {}; }
    static Msg on(const Model&, const Key& k) { return Typed{k.c}; }
};

struct Split {
    struct ToLeft;  struct ToRight;  struct ToTab;
    struct Model {
        Pane::Model left, right;
        std::map<int, Pane::Model> tabs;
        int focus = 0;                             // 0 left, 1 right, 2+ tab id
    };
    struct ToLeft  { Pane::Msg msg; };
    struct ToRight { Pane::Msg msg; };
    struct ToTab   { int id; Pane::Msg msg; };
    struct Focus   { int to; };
    using Msg = std::variant<ToLeft, ToRight, ToTab, Focus>;
    using Cmd = jaal::Cmd<Msg>;
    using L    = jaal::child<Pane, Split, ToLeft>;
    using R    = jaal::child<Pane, Split, ToRight>;
    using Tabs = jaal::children<Pane, Split, ToTab>;

    static Cmd init(Model& m) {
        return Cmd::batch(Tabs::add(m.tabs, 2), Tabs::add(m.tabs, 3));
    }
    static Cmd update(Model& m, ToLeft w)  { return L::update(m.left, std::move(w)); }
    static Cmd update(Model& m, ToRight w) { return R::update(m.right, std::move(w)); }
    static Cmd update(Model& m, ToTab w)   { return Tabs::update(m.tabs, std::move(w)); }
    static Cmd update(Model& m, Focus f)   { m.focus = f.to; return {}; }

    static std::optional<Msg> on(const Model& m, const Key& k) {
        if (k.c == '\t') return Focus{(m.focus + 1) % 4};
        switch (m.focus) {
            case 0:  return L::on(m.left, k);
            case 1:  return R::on(m.right, k);
            default: return Tabs::on(m.tabs, m.focus, k);
        }
    }
};

void composition() {
    std::printf("--- composition: input to the focused child ---\n");
    jaal::headless<Split, Key> h;
    for (char c : std::string("ab\tcd\tef\tgh\tij")) h.event(Key{c});
    const auto& m = h.model();
    check(m.left.typed == "abij" && m.right.typed == "cd", "child::on: keys reach the focused slot");
    check(m.tabs.at(2).typed == "ef" && m.tabs.at(3).typed == "gh",
          "children::on: keys reach the focused id");
}

// ── 7: sim drives input, interleaved with tasks, under every seed ─────────
// Type a query key by key; each key starts a keyed search. Results arrive
// at random times relative to later keys. Whatever the order, the shown
// result is never for a query that isn't current.
struct Live {
    struct Model { std::string query, shown; bool stale = false; };
    struct Typed { char c; };
    struct Found { std::string q; };
    using Msg = std::variant<Typed, Found>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model& m, Typed t) {
        m.query += t.c;
        return Cmd::perform<Found>("search", [](std::string q) { return q; }, m.query);
    }
    static Cmd update(Model& m, Found f) {
        if (f.q != m.query) m.stale = true;
        m.shown = f.q;
        return {};
    }
    static Msg on(const Model&, const Key& k) { return Typed{k.c}; }
};

void sim_input() {
    std::printf("--- sim: input through on(), 200 seeds ---\n");
    jaal::sim_options o;
    o.task_latency_max = std::chrono::milliseconds(15);
    auto rep = jaal::explore<Live, Key>(1, 200, o, [](jaal::sim<Live, Key>& s) {
        int t = 0;
        for (char c : std::string("jaal")) s.input(std::chrono::milliseconds(t += 4), Key{c});
        s.check("never a stale result", [](const Live::Model& m) { return !m.stale; });
    });
    check(rep.ok(), "input via sim::input; no stale result under any seed");
    if (!rep.ok()) std::puts(rep.failure->describe().c_str());

    jaal::sim<Live, Key> s(7, o);
    int t = 0;
    for (char c : std::string("jaal")) s.input(std::chrono::milliseconds(t += 4), Key{c});
    auto r = s.run();
    check(r.ok() && s.model().query == "jaal" && s.model().shown == "jaal",
          "the last query's result is what's shown");
}

}  // namespace

int main() {
    basics();
    composition();
    sim_input();
    std::printf(failures ? "FAILED (%d)\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
