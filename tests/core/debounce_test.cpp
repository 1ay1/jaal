// tests/core/debounce_test.cpp — debounce<T> and throttle.
//
// What must hold:
//   * only the newest token is ready; every superseded one is dropped
//   * a token still guards correctly when the VALUE returns to an earlier
//     one ("ab" -> "a"), which is where comparing values instead of tokens
//     silently breaks
//   * the same token rejects a late RESULT, not just a late timer
//   * throttle allows at most one per interval and remembers what it dropped
//   * both are usable in a model: Frozen (so timeline can snapshot) and
//     Sendable (so they can be part of a Msg)

#include <jaal/core.hpp>
#include <jaal/host/given.hpp>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <variant>
#include <vector>

using namespace std::chrono_literals;

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,   \
                         __LINE__, #c);                                   \
            return 1;                                                     \
        }                                                                 \
    } while (0)

namespace {

// ── debounce ─────────────────────────────────────────────────────────────
int only_the_newest_token_is_ready() {
    jaal::debounce<std::string> d;
    CHECK(d.empty());
    CHECK(!d.ready(0));                       // nothing set yet

    const auto t1 = d.set("a");
    const auto t2 = d.set("ab");
    const auto t3 = d.set("abc");
    CHECK(!d.empty());
    CHECK(!d.ready(t1));                      // superseded
    CHECK(!d.ready(t2));
    CHECK(d.ready(t3));                       // the last one wins
    CHECK(d.value() == "abc");
    return 0;
}

int a_value_returning_to_an_earlier_one_is_still_stale() {
    // This is the case that catches hand-rolled debouncing: the guard can't
    // be "is the value still what I searched for?", because deleting back to
    // a previous value makes an old timer look current again.
    jaal::debounce<std::string> d;
    const auto t_a  = d.set("a");
    const auto t_ab = d.set("ab");
    const auto t_a2 = d.set("a");              // back to "a" by backspace

    CHECK(d.value() == "a");                   // the VALUE matches the first
    CHECK(!d.ready(t_a));                      // ...but that token is stale
    CHECK(!d.ready(t_ab));
    CHECK(d.ready(t_a2));                      // only the newest
    return 0;
}

int invalidate_drops_everything_in_flight() {
    jaal::debounce<int> d;
    const auto t = d.set(5);
    CHECK(d.ready(t));
    const auto after = d.invalidate();         // Escape pressed
    CHECK(!d.ready(t));
    CHECK(d.value() == 5);                     // value kept
    CHECK(d.ready(after));
    return 0;
}

// ── a whole program: type, debounce, search, guard the result ────────────
struct Search {
    struct Model {
        jaal::debounce<std::string> query;
        std::vector<std::string>    searched;   // one entry per search RUN
        std::string                 shown;
    };
    struct Typed   { std::string text; };
    struct Fire    { std::uint64_t token; };
    struct Results { std::uint64_t token; std::string hit; };
    using Msg = std::variant<Typed, Fire, Results>;
    using Cmd = jaal::Cmd<Msg>;

    static Cmd update(Model& m, Typed t) {
        auto tok = m.query.set(t.text);
        return Cmd::after(200ms, Msg{Fire{tok}});
    }
    static Cmd update(Model& m, Fire f) {
        if (!m.query.ready(f.token)) return {};                     // superseded
        m.searched.push_back(m.query.value());
        return Cmd::send(Msg{Results{f.token, m.query.value() + "!"}});
    }
    static Cmd update(Model& m, Results r) {
        if (!m.query.ready(r.token)) return {};                     // a late answer
        m.shown = r.hit;
        return {};
    }
};

int eight_keystrokes_run_one_search() {
    jaal::given<Search> t;
    // Type "abcdefgh": 8 keystrokes, 8 timers in flight.
    std::string text;
    std::vector<std::uint64_t> tokens;
    for (char c : std::string("abcdefgh")) {
        text += c;
        t.when(Search::Typed{text});
        tokens.push_back(t.model().query.token());
    }
    CHECK(t.model().searched.empty());          // nothing fired yet

    // Every timer eventually goes off. The 7 stale ones are dropped.
    for (auto tok : tokens) t.when(Search::Fire{tok});
    t.settle();

    CHECK(t.ok());
    CHECK(t.model().searched.size() == 1);      // ONE search
    CHECK(t.model().searched[0] == "abcdefgh"); // for the final text
    CHECK(t.model().shown == "abcdefgh!");
    return 0;
}

int a_late_result_is_dropped() {
    jaal::given<Search> t;
    t.when(Search::Typed{"a"});
    const auto stale = t.model().query.token();
    t.when(Search::Fire{stale});                // fires, sends Results
    t.settle();
    CHECK(t.model().shown == "a!");

    // Now the user types again, and the OLD search's answer arrives late.
    t.when(Search::Typed{"b"});
    t.when(Search::Results{stale, "a!"});
    t.settle();
    CHECK(t.ok());
    CHECK(t.model().shown == "a!");             // not overwritten by the stale hit
    CHECK(t.model().searched.size() == 1);      // and no extra search ran
    return 0;
}

// ── throttle ─────────────────────────────────────────────────────────────
int throttle_allows_one_per_interval() {
    using tp = jaal::throttle::time_point;
    const tp t0{};
    jaal::throttle th(100ms);

    CHECK(th.allow(t0));                        // the first always passes
    CHECK(!th.allow(t0 + 10ms));                // too soon
    CHECK(!th.allow(t0 + 99ms));
    CHECK(th.pending());                        // and it remembers
    CHECK(th.allow(t0 + 100ms));                // exactly at the interval
    CHECK(!th.pending());                       // the allowed one cleared it
    return 0;
}

int throttle_remembers_the_trailing_edge() {
    using tp = jaal::throttle::time_point;
    const tp t0{};
    jaal::throttle th(50ms);
    CHECK(th.allow(t0));
    for (int i = 1; i < 20; ++i) (void)th.allow(t0 + std::chrono::milliseconds(i));
    // A burst was dropped: the final state must not be the lost one.
    CHECK(th.pending());
    CHECK(th.take_pending());
    CHECK(!th.pending());                       // taken once
    CHECK(th.next_allowed().has_value());
    CHECK(*th.next_allowed() == t0 + 50ms);

    th.reset();
    CHECK(th.allow(t0 + 1ms));                  // history forgotten
    return 0;
}

// ── they belong in a model ───────────────────────────────────────────
static_assert(jaal::Sendable<jaal::debounce<std::string>>,
              "debounce must be able to cross into a task");
static_assert(jaal::Frozen<jaal::debounce<int>>,
              "debounce must be snapshottable, so timeline<> works");
static_assert(jaal::Sendable<jaal::throttle>);
static_assert(jaal::Frozen<jaal::throttle>);

// ...and the opt-in is CONDITIONAL, not a blanket promise: a debounce over a
// shared owner is still correctly rejected, so wrapping a value in debounce
// can't be used to smuggle it past Sendable or Frozen.
static_assert(!jaal::Sendable<jaal::debounce<int*>>,
              "a raw pointer isn't Sendable, so neither is a debounce of one");
static_assert(!jaal::Frozen<jaal::debounce<std::shared_ptr<int>>>,
              "a shared owner isn't Frozen, so neither is a debounce of one");

}  // namespace

int main() {
    if (int r = only_the_newest_token_is_ready())      return r;
    if (int r = a_value_returning_to_an_earlier_one_is_still_stale()) return r;
    if (int r = invalidate_drops_everything_in_flight()) return r;
    if (int r = eight_keystrokes_run_one_search())     return r;
    if (int r = a_late_result_is_dropped())            return r;
    if (int r = throttle_allows_one_per_interval())    return r;
    if (int r = throttle_remembers_the_trailing_edge()) return r;
    std::puts("debounce: ok");
    return 0;
}
