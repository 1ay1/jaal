// tests/docs/manual_examples.cpp — docs/manual.md's examples, compiled and run.
//
// The manual walks through one app (a search box), section by section. This
// file is that app, plus each smaller example, so a sample that drifts from
// the API breaks the build instead of a reader's afternoon. Fix the manual
// in the same commit.

#include <jaal/jaal.hpp>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

using namespace std::chrono_literals;

namespace {

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

// ── §2 your first program ────────────────────────────────────────────────
struct Counter {
    struct Model { int n = 0; };

    struct Inc {};
    struct Reset {};
    using Msg = std::variant<Inc, Reset>;

    using Cmd = jaal::Cmd<Msg>;

    static Cmd update(Model& m, Inc)   { ++m.n; return {}; }
    static Cmd update(Model& m, Reset) { m.n = 0; return {}; }
};

// ── §3-§8 the search box ─────────────────────────────────────────────────
struct Key { char c; };

// slow, and sometimes it fails
std::vector<std::string> run_search(const std::string& q) {
    if (q == "!") throw std::runtime_error("offline");
    return {q + " one", q + " two"};
}

struct Search {
    struct Model {
        std::string query;
        std::vector<std::string> results;
        bool searching = false;
        std::string error;
    };

    struct Typed   { char c; };
    struct Clear   {};
    struct Cleared {};
    struct Settled {};
    struct Found   { std::vector<std::string> items; };
    struct Failed  { std::string why; };
    struct Esc     {};
    struct Quit    {};
    using Msg = std::variant<Typed, Clear, Cleared, Settled, Found, Failed, Esc, Quit>;

    using Cmd = jaal::Cmd<Msg>;

    // §7: wait for typing to settle, then search once
    static Cmd update(Model& m, Typed t) {
        m.query += t.c;
        return Cmd::after("settle", 200ms, Settled{});    // restarts on every key
    }
    static Cmd update(Model& m, Settled) {
        m.searching = true;
        // §5 + §6: off the loop, keyed so the newest wins, failure as a message
        return Cmd::attempt<Found, Failed>("search", &run_search, m.query);
    }
    static Cmd update(Model& m, Found f) {
        m.results = std::move(f.items);
        m.searching = false;
        return {};
    }
    static Cmd update(Model& m, Failed f) {
        m.error = std::move(f.why);
        m.searching = false;
        return {};
    }
    // §4
    static Cmd update(Model& m, Clear) {
        m.query.clear();
        m.results.clear();
        return Cmd::send(Cleared{});
    }
    static Cmd update(Model&, Cleared) { return {}; }
    // §6
    static Cmd update(Model& m, Esc) {
        m.searching = false;
        return Cmd::batch(Cmd::cancel("search"), Cmd::cancel("settle"));
    }
    static Cmd update(Model&, Quit) { return Cmd::quit(0); }

    // §8: input reads the live model
    static std::optional<Msg> on(const Model& m, const Key& k) {
        if (k.c == '\x1b') return Esc{};
        if (m.searching && k.c == '\n') return std::nullopt;   // ignore Enter while busy
        return Typed{k.c};
    }
    static std::optional<Msg> on(const Model&, jaal::sig s) {
        if (s == jaal::sig::interrupt) return Quit{};
        return std::nullopt;
    }
    using Event = std::variant<Key, jaal::sig>;
};
static_assert(jaal::Program<Search>);

// §5: the body shapes
struct Shapes {
    struct Model { int got = 0; std::vector<int> progress; };
    struct Found { std::vector<std::string> items; };
    struct Progress { int pct; };
    struct Go {}; struct Stream {};
    using Msg = std::variant<Found, Progress, Go, Stream>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model& m, Found f) { m.got = static_cast<int>(f.items.size()); return {}; }
    static Cmd update(Model& m, Progress p) { m.progress.push_back(p.pct); return {}; }
    static Cmd update(Model&, Go) {
        return Cmd::perform<Found>([](std::stop_token st, const std::string& q) {
            std::vector<std::string> out;
            for (auto& row : run_search(q)) {
                if (st.stop_requested()) break;    // cancelled: stop early
                out.push_back(row);
            }
            return out;
        }, std::string("jaal"));
    }
    static Cmd update(Model&, Stream) {
        return Cmd::task([](jaal::Sink<Msg> out, std::string) {
            for (int pct = 0; pct <= 100; pct += 25) out.send(Progress{pct});
        }, std::string("q"));
    }
};

// §4: your own effect
struct Bell {};
using ring = jaal::pure_fx<Bell, "ring">;
struct Ringer {
    struct Model {};
    struct Ding {};
    using Msg = std::variant<Ding>;
    using Cmd = jaal::Cmd<Msg, ring>;            // core effects + ring
    static Cmd update(Model&, Ding) { return Bell{}; }
};
static_assert(jaal::Program<Ringer>);

// §9: subscriptions
struct Live {
    struct Model { bool live = false; std::string query = "q"; int ticks = 0; };
    struct Tick {}; struct Update { int v; }; struct Toggle {};
    using Msg = std::variant<Tick, Update, Toggle>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;
    static Cmd update(Model& m, Tick)   { ++m.ticks; return {}; }
    static Cmd update(Model&, Update)   { return {}; }
    static Cmd update(Model& m, Toggle) { m.live = !m.live; return {}; }
    static int poll(const std::string&) { return 1; }
    static Sub subscribe(const Model& m) {
        if (!m.live) return Sub::none();
        return Sub::batch(
            Sub::every(1s, Tick{}),
            Sub::stream("feed", [](jaal::Sink<Msg> out, std::stop_token st, std::string q) {
                while (!st.stop_requested()) {
                    if (!out.send(Update{poll(q)})) return;
                    std::this_thread::sleep_for(1ms);
                }
            }, m.query));
    }
    static auto subs_key(const Model& m) { return std::tuple{m.live, m.query}; }
};
static_assert(jaal::HasSubsKey<Live>);

// §10: a child, and children
struct App {
    struct ToSearch { Search::Msg msg; };
    using Msg = std::variant<ToSearch>;
    using Cmd = jaal::Cmd<Msg>;
    using Box = jaal::child<Search, App, ToSearch>;

    struct Model { Search::Model search; };

    static Cmd update(Model& m, ToSearch w) { return Box::update(m.search, w); }
    static std::optional<Msg> on(const Model& m, const Key& k) {
        return Box::on(m.search, k);                   // input goes to the child
    }
    using Event = Key;
};
static_assert(jaal::Program<App>);

struct Tabbed {
    struct ToTab { int id; Search::Msg msg; };
    struct NewTab {}; struct CloseTab { int id; };
    using Msg = std::variant<ToTab, NewTab, CloseTab>;
    using Cmd = jaal::Cmd<Msg>;
    using Tabs = jaal::children<Search, Tabbed, ToTab>;

    struct Model { Tabs::map tabs; int focused = 0; };

    static Cmd update(Model& m, ToTab w)     { return Tabs::update(m.tabs, w); }
    static Cmd update(Model& m, NewTab)      { return Tabs::add(m.tabs).second; }
    static Cmd update(Model& m, CloseTab c)  { Tabs::remove(m.tabs, c.id); return {}; }
    static std::optional<Msg> on(const Model& m, const Key& k) {
        return Tabs::on(m.tabs, m.focused, k);         // keys go to the focused tab
    }
    using Event = Key;
};
static_assert(jaal::Program<Tabbed>);

// §5/§6: the other spellings the manual shows, each compiled
struct Spellings {
    struct Model { std::vector<std::string> results; };
    struct A {}; struct B {}; struct C {}; struct D {}; struct E {};
    struct Found { std::vector<std::string> items; };
    struct Failed { std::string why; };
    struct Progress { int pct; };
    using Msg = std::variant<A, B, C, D, E, Found, Failed, Progress>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model& m, Found f) { m.results = std::move(f.items); return {}; }
    static Cmd update(Model&, Failed) { return {}; }
    static Cmd update(Model&, Progress) { return {}; }
    static Cmd update(Model&, A) {
        const std::string query = "q";
        return Cmd::perform<Found>(&run_search, query);                     // no key
    }
    static Cmd update(Model&, B) {
        return Cmd::attempt<Found, Failed>(&run_search, std::string("q"));  // no key
    }
    static Cmd update(Model&, C) {
        return Cmd::perform<Found>(jaal::key("search", jaal::fx::isolated), &run_search,
                                   std::string("q"));
    }
    static Cmd update(Model&, D) { return Cmd::cancel("search"); }
    static Cmd update(Model&, E) {
        return Cmd::task([](jaal::Sink<Msg> out, std::string) {
            for (int pct = 0; pct <= 100; pct += 25) out.send(Progress{pct});
        }, std::string("q"));
    }
};
static_assert(jaal::Program<Spellings>);

// §3: a nested group
struct Grouped {
    struct Model { int n = 0; };
    struct Typed { char c; }; struct Clear {};
    using Edit = std::variant<Typed, Clear>;           // a group
    struct Quit {};
    using Msg = std::variant<Edit, Quit>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model& m, Typed) { ++m.n; return {}; }   // the leaves
    static Cmd update(Model& m, Clear) { m.n = 0; return {}; }
    static Cmd update(Model&, Quit)    { return Cmd::quit(0); }
};
static_assert(jaal::Program<Grouped>);

// §12: run options compile (not run: run() owns the process's signals)
[[maybe_unused]] int run_with_options(jaal::headless_host& host) {
    jaal::run_options o;
    o.kernel.max_workers = 8;
    o.on_seed = [](std::uint64_t) {};
    o.kernel.on_fault = jaal::fault_policy::skip;
    return jaal::run<Counter>(host, o);
}

}  // namespace

int main() {
    // §2
    {
        jaal::headless<Counter> h;
        h.send(Counter::Inc{});
        h.send(Counter::Inc{});
        check(h.model().n == 2, "§2 counter");
    }

    // §11: call update directly
    {
        Counter::Model m;
        Counter::update(m, Counter::Inc{});
        check(m.n == 1, "§11 update is a function");
    }

    // §11: given
    {
        jaal::given<Search> t;
        t.when(Search::Typed{'j'});
        t.expect_effect<jaal::fx::after>(1);               // asked for the settle timer
        t.when(Search::Settled{});
        t.settle();                                        // runs the search inline
        t.expect("results shown", [](auto& m) { return !m.results.empty(); });
        check(t.ok(), "§11 given");
    }

    // §11: headless with a clock you move
    {
        jaal::headless<Search> h;
        for (char c : std::string("jaal")) { h.send(Search::Typed{c}); h.advance(50ms); }
        h.advance(200ms);                  // typing settled: one search starts
        h.run_until_idle();                // let the worker finish
        check(h.model().results.size() > 0 && h.model().results.front() == "jaal one",
              "§11 headless: one search, for the settled query");
        h.event(Key{'!'});                 // §8 input through on(); Event says Key
        check(h.model().query == "jaal!", "§8 on() turns a Key into Typed");
    }

    // §5: attempt turns a throw into Failed
    {
        jaal::headless<Search> h;
        h.send(Search::Typed{'!'});
        h.advance(250ms);
        h.run_until_idle();
        check(h.model().error == "offline", "§5 attempt: a throw arrives as Failed");
    }

    // §5: body shapes run
    {
        jaal::headless<Shapes> h;
        h.send(Shapes::Go{});
        h.send(Shapes::Stream{});
        h.run_until_idle();
        check(h.model().got == 2, "§5 perform with stop_token and const&");
        check(h.model().progress == std::vector<int>{0, 25, 50, 75, 100}, "§5 task sends progress");
    }

    // §9: a stream runs while live, and stops when not
    {
        jaal::headless<Live> h;
        h.send(Live::Toggle{});
        h.advance(3s);
        check(h.model().ticks == 3, "§9 every ticks while live");
        h.send(Live::Toggle{});
        h.advance(3s);
        check(h.model().ticks == 3, "§9 and stops when not");
    }

    // §10: input reaches the child
    {
        jaal::headless<App> h;
        h.event(Key{'j'});
        check(h.model().search.query == "j", "§10 child::on forwards input");
    }
    {
        jaal::headless<Tabbed> h;
        h.send(Tabbed::NewTab{});
        h.send(Tabbed::NewTab{});
        h.event(Key{'x'});
        check(h.model().tabs.at(0).query == "x" && h.model().tabs.at(1).query.empty(),
              "§10 children::on: the focused tab gets the key");
    }

    // §11: explore
    {
        auto r = jaal::explore<Search>(1, 200, {}, [](jaal::sim<Search>& s) {
            int t = 0;
            for (char c : std::string("jaal")) s.input(std::chrono::milliseconds(t += 30), Key{c});
            s.check("results match the query", [](const Search::Model& m) {
                return m.results.empty() || m.results.front().starts_with(m.query);
            });
        });
        if (!r.ok()) std::puts(r.failure->describe().c_str());
        check(r.ok(), "§11 explore: results always match the query");
    }

    // §5: the spellings run
    {
        jaal::headless<Spellings> h;
        h.send(Spellings::A{});
        h.run_until_idle();
        check(h.model().results.size() == 2, "§5 perform with no key");
        h.send(Spellings::C{});
        h.send(Spellings::D{});
        h.send(Spellings::E{});
        h.run_until_idle();
        jaal::headless<Grouped> g;
        g.send(Grouped::Edit{Grouped::Typed{'x'}});
        check(g.model().n == 1, "§3 a nested group routes to the leaf");
    }

    if (failures == 0) std::puts("manual examples: ok");
    return failures ? 1 : 0;
}
