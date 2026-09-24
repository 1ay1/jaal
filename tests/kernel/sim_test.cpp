// tests/kernel/sim_test.cpp — deterministic simulation.
//
// The headline check: a program with a real ordering bug (a stale fetch
// result overwriting a newer one) passes with the pool on a fast machine,
// and the sim finds a seed that breaks it, reproduces it exactly, and the
// recorded messages replay to the same bad model.

#include <jaal/jaal.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <variant>
#include <vector>

using namespace std::chrono_literals;
using jaal::Sink;

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,   \
                         __LINE__, #c);                                   \
            return 1;                                                     \
        }                                                                 \
    } while (0)

namespace {

// ── a search box with a race ─────────────────────────────────────────────
// Each keystroke starts a fetch. Results come back in any order. The buggy
// version shows whatever came back LAST; the fixed one ignores results for
// a query that isn't the current one.
template <bool Fixed>
struct Search {
    struct Model {
        int query   = 0;     // what the user typed last
        int shown   = 0;     // which query's results are on screen
        int fetches = 0;
        bool operator==(const Model&) const = default;
    };
    struct Type { int q; };
    struct Results { int q; };
    using Msg = std::variant<Type, Results>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;

    static Cmd update(Model& m, Type t) {
        m.query = t.q;
        ++m.fetches;
        return Cmd::task([](Sink<Msg> out, std::stop_token, int q) {
                             out.send(Results{q});
                         }, t.q);
    }
    static Cmd update(Model& m, Results r) {
        const int q = r.q;
        if (!Fixed || q == m.query) m.shown = q;
        return {};
    }
};

template <class P>
void typing(jaal::sim<P>& s) {
    // Three keystrokes 2 ms apart; fetches take 0-10 ms.
    s.at(0ms, typename P::Type{1});
    s.at(2ms, typename P::Type{2});
    s.at(4ms, typename P::Type{3});
    s.check("shown results are never older than an earlier shown result",
            [last = 0](const typename P::Model& m) mutable {
                bool ok = m.shown >= last;
                last = m.shown;
                return ok;
            });
    s.check("results shown belong to a query that was typed",
            [](const typename P::Model& m) { return m.shown <= m.query; });
}

int finds_the_race() {
    using Buggy = Search<false>;
    auto r = jaal::explore<Buggy>(1, 500, {}, [](jaal::sim<Buggy>& s) { typing(s); });
    CHECK(!r.ok());
    CHECK(r.runs < 500);
    const auto& f = *r.failure;
    CHECK(f.end == jaal::sim_end::broke);
    CHECK(f.broken.has_value());
    std::printf("  found: %s\n", f.describe().c_str());

    // Reproduce: the same seed breaks the same way, message for message.
    jaal::sim<Buggy> again(f.seed);
    typing(again);
    auto r2 = again.run();
    CHECK(!r2.ok());
    CHECK(r2.steps == f.steps);
    CHECK(r2.elapsed == f.elapsed);
    CHECK(r2.messages.size() == f.messages.size());

    // The log replays through update() to the model the sim ended on.
    CHECK(jaal::replay<Buggy>(f.messages) == again.model());
    return 0;
}

int fixed_version_holds() {
    using Fixed = Search<true>;
    auto r = jaal::explore<Fixed>(1, 500, {}, [](jaal::sim<Fixed>& s) { typing(s); });
    CHECK(r.ok());
    CHECK(r.runs == 500);
    return 0;
}

// Different seeds really do give different orders.
int seeds_differ() {
    using Buggy = Search<false>;
    std::vector<std::size_t> finals;
    for (std::uint64_t seed = 1; seed <= 64; ++seed) {
        jaal::sim<Buggy> s(seed);
        s.at(0ms, Buggy::Type{1});
        s.at(0ms, Buggy::Type{2});
        s.at(0ms, Buggy::Type{3});
        auto r = s.run();
        CHECK(r.end == jaal::sim_end::idle);
        CHECK(s.model().fetches == 3);
        finals.push_back(static_cast<std::size_t>(s.model().shown));
    }
    bool saw[4] = {};
    for (auto v : finals) saw[v] = true;
    CHECK(saw[1] && saw[2] && saw[3]);      // every order showed up
    return 0;
}

// ── timers, quit, limits ─────────────────────────────────────────────────
struct Clock {
    struct Model { int ticks = 0; };
    struct Tick {}; struct Stop {};
    using Msg = std::variant<Tick, Stop>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;
    static Cmd update(Model&, Stop) { return Cmd::quit(3); }
    static Cmd update(Model& m, Tick) {
        ++m.ticks;
        return {};
    }
    static Sub subscribe(const Model&) { return Sub::every(1s, Tick{}); }
};

int timers_and_quit() {
    jaal::sim<Clock> s(7);
    s.at(1h, Clock::Stop{});                  // an hour of sim time, instantly
    auto r = s.run();
    CHECK(r.end == jaal::sim_end::quit);
    CHECK(r.exit == 3);
    CHECK(s.model().ticks >= 3599 && s.model().ticks <= 3600);   // tie at 1h is seeded
    CHECK(r.elapsed == 1h);
    return 0;
}

int limits() {
    {
        jaal::sim_options o;
        o.max_steps = 10;
        jaal::sim<Clock> s(1, o);
        auto r = s.run();
        CHECK(r.end == jaal::sim_end::step_limit);
        CHECK(r.ok());                                  // a limit isn't a failure
    }
    {
        jaal::sim_options o;
        o.max_time = 10s;
        jaal::sim<Clock> s(1, o);
        auto r = s.run();
        CHECK(r.end == jaal::sim_end::time_limit);
        CHECK(s.model().ticks == 10);
    }
    return 0;
}

// ── fault injection ──────────────────────────────────────────────────────
struct Jobs {
    struct Model { int started = 0; int done = 0; };
    struct Go {}; struct Done {};
    using Msg = std::variant<Go, Done>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model& m, Go) {
        ++m.started;
        return Cmd::task([](Sink<Msg> out, std::stop_token) { out.send(Done{}); });
    }
    static Cmd update(Model& m, Done) {
        ++m.done;
        return {};
    }
};

int injected_faults() {
    jaal::sim_options o;
    o.task_crash = 0.3;
    o.task_lose  = 0.2;
    o.kernel.on_fault = jaal::fault_policy::skip;
    jaal::sim<Jobs> s(42, o);
    for (int i = 0; i < 200; ++i) s.at(std::chrono::milliseconds(i), Jobs::Go{});
    auto r = s.run();
    CHECK(r.end == jaal::sim_end::idle);
    CHECK(s.model().started == 200);
    CHECK(s.model().done < 200);
    CHECK(!r.faults.empty());
    // started = done + crashed + lost, and each crash was reported.
    CHECK(static_cast<std::size_t>(s.model().done) + r.faults.size() <= 200);
    for (auto& f : r.faults) CHECK(f.find("injected task crash") != std::string::npos);
    return 0;
}

// ── streams are scripted through their sink ──────────────────────────────
struct Feed {
    struct Model { bool on = true; int got = 0; };
    struct Item {}; struct Off {};
    using Msg = std::variant<Item, Off>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;
    static Cmd update(Model& m, Off) {
        m.on = false;
        return {};
    }
    static Cmd update(Model& m, Item) {
        ++m.got;
        return {};
    }
    static Sub subscribe(const Model& m) {
        if (!m.on) return Sub::none();
        return Sub::stream("feed", [](Sink<Msg>, std::stop_token) {
            std::abort();                     // the sim never runs a stream body
        });
    }
};

int streams() {
    jaal::sim<Feed> s(1);
    s.run();
    auto in = s.stream("feed");
    CHECK(in.open());
    CHECK(in.send(Feed::Item{}));
    CHECK(in.send(Feed::Item{}));
    s.at(0ms, Feed::Off{});
    s.run();
    CHECK(s.model().got == 2);
    CHECK(!in.send(Feed::Item{}));             // unsubscribed: the sink went dead
    CHECK(!s.stream("feed").open());
    return 0;
}

// ── the RNG is the same everywhere ───────────────────────────────────────
int rng_is_fixed() {
    jaal::sim_rng r(0);
    // splitmix64 reference values for seed 0.
    CHECK(r.next() == 0xe220a8397b1dcdafULL);
    CHECK(r.next() == 0x6e789e6aa1b965f4ULL);
    jaal::sim_rng a(9), b(9);
    for (int i = 0; i < 1000; ++i) CHECK(a.below(17) == b.below(17));
    jaal::sim_rng c(3);
    for (int i = 0; i < 1000; ++i) CHECK(c.below(5) < 5);
    return 0;
}

// ── one seed controls the program's draws too ─────────────────────────────
// The sim's promise is that ONE seed reproduces the whole run. A program
// using Cmd::random is part of that run, so its draws must come from the
// sim seed as well, and two sims with the same seed must roll identically.
struct Dice {
    struct Model { std::vector<int> rolls; };
    struct Roll {};
    struct Rolled { int face; };
    using Msg = std::variant<Roll, Rolled>;
    using Cmd = jaal::Cmd<Msg>;

    static Cmd update(Model&, Roll) {
        return Cmd::random([](jaal::rng& r) -> Msg {
            return Rolled{static_cast<int>(r.in(1, 6))};
        });
    }
    static Cmd update(Model& m, Rolled r) {
        m.rolls.push_back(r.face);
        return {};
    }
};

std::vector<int> sim_rolls(std::uint64_t seed) {
    jaal::sim<Dice> s(seed);
    for (int i = 0; i < 12; ++i)
        s.at(std::chrono::milliseconds(i), Dice::Roll{});
    s.run();
    return s.model().rolls;
}

int random_follows_the_sim_seed() {
    const auto a = sim_rolls(4);
    CHECK(a.size() == 12);
    for (int f : a) CHECK(f >= 1 && f <= 6);
    CHECK(sim_rolls(4) == a);              // same seed, same rolls
    CHECK(sim_rolls(5) != a);              // a different seed rolls differently
    return 0;
}

}  // namespace

int main() {
    if (int r = rng_is_fixed())        return r;
    if (int r = finds_the_race())      return r;
    if (int r = fixed_version_holds()) return r;
    if (int r = seeds_differ())        return r;
    if (int r = timers_and_quit())     return r;
    if (int r = limits())              return r;
    if (int r = injected_faults())     return r;
    if (int r = streams())             return r;
    if (int r = random_follows_the_sim_seed()) return r;
    std::puts("sim: ok");
    return 0;
}
