// tests/kernel/keyed_task_test.cpp — keyed tasks, cancel, perform, attempt.
//
// What each promises, checked on the real kernel and pool:
//
//   1. latest wins: a new task with a key stops the running one (its
//      stop_token fires) and NOTHING it sends reaches update(), not even
//      a message it queued before the replace
//   2. cancel(key) stops it the same way; cancelling an idle key is a no-op
//   3. different keys don't interfere; unkeyed tasks are untouched
//   4. children: two children running the same key are two tasks
//   5. perform: the body returns a value, it arrives as Ok{value}; void
//      bodies send Ok{}; a stop_token parameter is optional
//   6. attempt: a throw arrives as Fail{what()} instead of a fault
//   7. sim: a replaced task's late result never folds, whatever the seed
//   8. given: perform/attempt settle inline, so reducers test without threads

#include <jaal/jaal.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

using namespace std::chrono_literals;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %-62s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

// ── 1-3: search-as-you-type ──────────────────────────────────────────────
std::atomic<int> stopped{0};

struct Search {
    struct Model {
        std::string           query;
        std::vector<std::string> shown;   // every result that reached update
        int                   other = 0;
    };
    struct Type   { std::string q; };
    struct Found  { std::string q; };
    struct Stop   {};
    struct Other  { int n; };
    struct KickOther {};
    using Msg = std::variant<Type, Found, Stop, Other, KickOther>;
    using Cmd = jaal::Cmd<Msg>;

    // Sends "<q>:early" at once, then waits; a stopped run sends nothing
    // more. A replaced run's early message is already QUEUED when the
    // replace happens: it must still never fold.
    static void body(jaal::Sink<Msg> out, std::stop_token st, std::string q, int wait_ms) {
        out.send(Found{q + ":early"});
        for (int i = 0; i < wait_ms && !st.stop_requested(); ++i)
            std::this_thread::sleep_for(1ms);
        if (st.stop_requested()) { stopped.fetch_add(1); return; }
        out.send(Found{q});
    }

    static Cmd update(Model& m, Type t) {
        m.query = t.q;
        return Cmd::task("search", &body, t.q, 200);
    }
    static Cmd update(Model& m, Found f) { m.shown.push_back(f.q); return {}; }
    static Cmd update(Model&, Stop) { return Cmd::cancel("search"); }
    static Cmd update(Model& m, Other o) { m.other += o.n; return {}; }
    static Cmd update(Model&, KickOther) {
        return Cmd::task([](jaal::Sink<Msg> out, std::stop_token) { out.send(Other{1}); });
    }
};

bool has(const std::vector<std::string>& v, const std::string& s) {
    for (auto& x : v) if (x == s) return true;
    return false;
}

void latest_wins() {
    std::printf("--- latest wins ---\n");
    stopped = 0;
    jaal::headless<Search> h;
    h.send(Search::Type{"a"});
    std::this_thread::sleep_for(30ms);          // "a:early" is queued by now
    h.send(Search::Type{"ab"});
    h.send(Search::KickOther{});                // unkeyed, untouched
    // The current task sleeps 200 ms; idle ("nothing posted for 20 ms")
    // comes sooner, so wait for the result itself.
    for (int i = 0; i < 300 && !has(h.model().shown, "ab"); ++i) h.run_until_idle(20ms);

    const auto& s = h.model().shown;
    check(!has(s, "a") && !has(s, "a:early"),
          "nothing from the replaced task folds, even what it queued");
    check(has(s, "ab:early") && has(s, "ab"), "the current task's results fold");
    check(stopped.load() == 1, "the replaced task's stop_token fired");
    check(h.model().other == 1, "an unkeyed task is untouched");
    check(h.kernel().has_task("search"), "the key stays current until replaced");
}

void cancel_stops() {
    std::printf("--- cancel ---\n");
    stopped = 0;
    jaal::headless<Search> h;
    h.send(Search::Type{"x"});
    std::this_thread::sleep_for(30ms);
    h.send(Search::Stop{});
    h.send(Search::Stop{});                     // idle key: no-op
    h.run_until_idle(3s);
    check(h.model().shown.empty(), "a cancelled task delivers nothing");
    check(stopped.load() == 1, "its stop_token fired");
    check(!h.kernel().has_task("search"), "the key is free again");
}

// ── 4: children don't share keys ─────────────────────────────────────────
struct Tab {
    struct Model { std::vector<std::string> shown; };
    struct Go   { std::string q; };
    struct Got  { std::string q; };
    using Msg = std::variant<Go, Got>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model&, Go g) {
        return Cmd::perform<Got>("search", [](std::string q) { return q; }, g.q);
    }
    static Cmd update(Model& m, Got g) { m.shown.push_back(g.q); return {}; }
};

struct Tabs {
    struct ForTab;
    struct Model { std::map<int, Tab::Model> tabs; };
    struct ForTab { int id; Tab::Msg msg; };
    struct Open { int id; };
    struct Ask  { int id; std::string q; };
    using Msg = std::variant<ForTab, Open, Ask>;
    using Cmd = jaal::Cmd<Msg>;
    using kids = jaal::children<Tab, Tabs, ForTab>;
    static Cmd update(Model& m, ForTab f) { return kids::update(m.tabs, std::move(f)); }
    static Cmd update(Model& m, Open o) { return kids::add(m.tabs, o.id); }
    static Cmd update(Model& m, Ask a) {
        return kids::update(m.tabs, ForTab{a.id, Tab::Go{a.q}});
    }
};

void children_keys() {
    std::printf("--- children: same key, separate tasks ---\n");
    jaal::headless<Tabs> h;
    h.send(Tabs::Open{1});
    h.send(Tabs::Open{2});
    h.send(Tabs::Ask{1, "one"});
    h.send(Tabs::Ask{2, "two"});               // same key "search", other child
    h.run_until_idle(3s);
    const auto& t = h.model().tabs;
    check(t.at(1).shown == std::vector<std::string>{"one"}
              && t.at(2).shown == std::vector<std::string>{"two"},
          "each child's \"search\" delivered to that child");
}

// ── 5-6: perform / attempt ───────────────────────────────────────────────
struct Loader {
    struct Model {
        int         size = -1;
        bool        saved = false;
        std::string error;
        int         checked = 0;
    };
    struct Load    { std::string path; };
    struct Loaded  { int size; };
    struct Save    {};
    struct Saved   {};
    struct Broken  {};
    struct Failed  { std::string why; };
    struct Polite  {};
    using Msg = std::variant<Load, Loaded, Save, Saved, Broken, Failed, Polite>;
    using Cmd = jaal::Cmd<Msg>;

    static Cmd update(Model&, Load l) {
        // a `const std::string&` parameter: perform accepts it
        return Cmd::perform<Loaded>([](const std::string& p) {
            return static_cast<int>(p.size());
        }, l.path);
    }
    static Cmd update(Model& m, Loaded l) { m.size = l.size; return {}; }
    static Cmd update(Model&, Save) {
        return Cmd::perform<Saved>([] {});      // void body: Saved{}
    }
    static Cmd update(Model& m, Saved) { m.saved = true; return {}; }
    static Cmd update(Model&, Broken) {
        return Cmd::attempt<Loaded, Failed>([](std::string p) -> int {
            throw std::runtime_error("no such file: " + p);
        }, std::string("missing.txt"));
    }
    static Cmd update(Model& m, Failed f) { m.error = f.why; return {}; }
    static Cmd update(Model&, Polite) {
        // a stop_token parameter, taken when wanted
        return Cmd::perform<Loaded>([](std::stop_token st, int n) {
            return st.stop_requested() ? -1 : n;
        }, 42);
    }
};

void perform_attempt() {
    std::printf("--- perform / attempt ---\n");
    jaal::headless<Loader> h;
    h.send(Loader::Load{"hello"});
    h.send(Loader::Save{});
    h.send(Loader::Broken{});
    h.run_until_idle(3s);
    check(h.model().size == 5, "perform: the body's return arrives as Ok{value}");
    check(h.model().saved, "perform: a void body arrives as Ok{}");
    check(h.model().error == "no such file: missing.txt", "attempt: a throw arrives as Fail{what()}");
    check(h.kernel().fault_count() == 0, "attempt: the throw is not a task fault");
    h.send(Loader::Polite{});
    h.run_until_idle(3s);
    check(h.model().size == 42, "perform: a stop_token parameter is optional");
}

// ── 7: sim, every seed ───────────────────────────────────────────────────
void sim_latest_wins() {
    std::printf("--- sim: a replaced task never folds, any seed ---\n");
    struct S {
        struct Model { std::string query; std::string shown; bool stale = false; };
        struct Type  { std::string q; };
        struct Found { std::string q; };
        using Msg = std::variant<Type, Found>;
        using Cmd = jaal::Cmd<Msg>;
        static Cmd update(Model& m, Type t) {
            m.query = t.q;
            return Cmd::perform<Found>("search", [](std::string q) { return q; }, t.q);
        }
        static Cmd update(Model& m, Found f) {
            if (f.q != m.query) m.stale = true;  // an old query's result
            m.shown = f.q;
            return {};
        }
    };
    jaal::sim_options o;
    o.task_latency_max = 20ms;
    auto rep = jaal::explore<S>(1, 300, o, [](jaal::sim<S>& s) {
        s.at(0ms,  S::Type{"a"});
        s.at(3ms,  S::Type{"ab"});
        s.at(6ms,  S::Type{"abc"});
        s.check("no stale result folds", [](const S::Model& m) { return !m.stale; });
    });
    check(rep.ok(), "300 seeds: never a stale result");
    if (!rep.ok()) std::puts(rep.failure->describe().c_str());
}

// ── 9: a keyed after is a debounce ─────────────────────────────────────────
struct Debounced {
    struct Model { std::string text; int fired = 0; std::string fired_for; };
    struct Typed { char c; };
    struct Fire {};
    struct Esc {};
    using Msg = std::variant<Typed, Fire, Esc>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model& m, Typed t) {
        m.text += t.c;
        return Cmd::after("settle", 100ms, Fire{});     // restarts on every key
    }
    static Cmd update(Model& m, Fire) { ++m.fired; m.fired_for = m.text; return {}; }
    static Cmd update(Model&, Esc) { return Cmd::cancel("settle"); }
};

void keyed_after() {
    std::printf("--- keyed after: debounce and cancel ---\n");
    jaal::headless<Debounced> h;                       // sim clock: we move time
    for (char c : std::string("jaal")) { h.send(Debounced::Typed{c}); h.advance(40ms); }
    check(h.model().fired == 0, "each key restarts the timer: nothing fires while typing");
    h.advance(100ms);
    check(h.model().fired == 1 && h.model().fired_for == "jaal",
          "it fires once, 100ms after the last key");

    h.send(Debounced::Typed{'!'});
    h.advance(50ms);
    h.send(Debounced::Esc{});
    h.advance(500ms);
    check(h.model().fired == 1, "cancel drops a pending keyed timer");
    check(!h.kernel().has_task("settle"), "and frees the key");

    // The timer fires, and in the SAME step a key arrives before its Fire
    // is folded. The new key replaces it, so that queued Fire must not run.
    jaal::headless<Debounced> g;
    g.send(Debounced::Typed{'a'});
    auto& k = g.kernel();
    g.advance(99ms);
    k.clock().advance(1ms);                          // due now, not yet stepped
    k.dispatch(Debounced::Typed{'b'});               // queued AHEAD of the fire
    (void)k.step(g.record());                        // [Typed b, Fire(a)] in one step
    g.advance(50ms);
    check(g.model().fired == 0, "a fired timer replaced before its fold is dropped");
    g.advance(60ms);
    check(g.model().fired == 1 && g.model().fired_for == "ab",
          "and the replacement fires on its own schedule");
}

// ── 8: given settles perform inline ──────────────────────────────────────
void given_inline() {
    std::printf("--- given: perform/attempt settle inline ---\n");
    jaal::given<Loader> t;
    t.when(Loader::Load{"abc"});
    t.expect_effect<jaal::fx::task>(1);
    t.settle();
    t.expect("loaded", [](auto& m) { return m.size == 3; });
    jaal::given<Loader> u;
    u.when(Loader::Broken{});
    u.settle();
    u.expect("failed", [](auto& m) { return !m.error.empty(); });
    check(t.ok() && u.ok(), "the reducer and its tasks test with no threads");
}

}  // namespace

int main() {
    latest_wins();
    cancel_stops();
    children_keys();
    perform_attempt();
    sim_latest_wins();
    given_inline();
    keyed_after();
    std::printf(failures ? "FAILED (%d)\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
