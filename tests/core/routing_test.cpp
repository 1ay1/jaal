// tests/core/routing_test.cpp — how a message finds its update.
//
// A small program has one flat Msg and one update per case. A big one
// doesn't: agentty has 232 message types in 20 groups
//
//   using ComposerMsg = std::variant<ComposerEnter, ComposerBackspace, ...>;
//   using Msg         = std::variant<ComposerMsg, StreamMsg, ...>;
//
// so that each group's reducer compiles in its own translation unit and
// touching one leaf doesn't rebuild the world.
//
// So routing is a TREE, and the rule at each node is the one C++ already
// taught everyone: the most specific handler wins.
//
//   * a LEAF handler (update(Model&, ComposerEnter)) is called
//   * a GROUP handler (update(Model&, ComposerMsg)) is called, and jaal does
//     NOT descend: that domain is handled whole
//   * with neither, jaal descends and asks the same question of each
//     alternative
//   * a leaf with no handler anywhere on its path is a compile error naming
//     the leaf AND the path taken to it
//
// The plan is computed once and read twice — the Program concept checks it,
// prog::update walks it — so "it compiled" and "it dispatches there" can't
// drift apart. These tests pin the behaviour; the compile-fail cases pin the
// errors (tests/CMakeLists.txt: program_missing_leaf_in_group).

#include <jaal/jaal.hpp>

#include <cstdio>
#include <string>
#include <variant>

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,   \
                         __LINE__, #c);                                   \
            return 1;                                                     \
        }                                                                 \
    } while (0)

namespace {

// ── leaves, in two groups ────────────────────────────────────────────────
struct Enter {};
struct Backspace {};
struct Submit { std::string text; };
using ComposerMsg = std::variant<Enter, Backspace, Submit>;

struct Delta { int bytes; };
struct Done {};
struct Failed { std::string why; };
using StreamMsg = std::variant<Delta, Done, Failed>;

}  // namespace

// The stream domain is handled in ONE place, by choice. Saying so is a line
// of its own: it can't be inferred from an overload, because a catch-all
// template matches a group type too and would switch off the leaf checks
// for that domain without anyone asking.
template <> inline constexpr bool jaal::handled_as_group<StreamMsg> = true;

namespace {

// ── a program that mixes both styles, which is the point ─────────────────
struct App {
    struct Model {
        int         enters = 0;
        int         backs = 0;
        std::string submitted;
        int         bytes = 0;
        bool        done = false;
        std::string error;
        int         group_calls = 0;     // how often the GROUP handler ran
    };

    using Msg = std::variant<ComposerMsg, StreamMsg>;
    using Cmd = jaal::Cmd<Msg>;

    // Composer: one handler per LEAF. Adding a fourth composer message
    // without a handler here won't compile.
    static Cmd update(Model& m, Enter)     { ++m.enters; return {}; }
    static Cmd update(Model& m, Backspace) { ++m.backs;  return {}; }
    static Cmd update(Model& m, Submit s)  { m.submitted = std::move(s.text); return {}; }

    // Stream: ONE handler for the whole group. jaal stops here and hands
    // over the variant; this domain does its own dispatch.
    static Cmd update(Model& m, StreamMsg s) {
        ++m.group_calls;
        if (auto* d = std::get_if<Delta>(&s))       m.bytes += d->bytes;
        else if (std::get_if<Done>(&s))            m.done = true;
        else if (auto* f = std::get_if<Failed>(&s)) m.error = f->why;
        return {};
    }
};
static_assert(jaal::Program<App>);

int leaves_go_to_their_own_handlers() {
    jaal::headless<App> h;
    h.send(App::Msg{ComposerMsg{Enter{}}});
    h.send(App::Msg{ComposerMsg{Backspace{}}});
    h.send(App::Msg{ComposerMsg{Submit{"hello"}}});
    CHECK(h.model().enters == 1);
    CHECK(h.model().backs == 1);
    CHECK(h.model().submitted == "hello");
    CHECK(h.model().group_calls == 0);          // the group handler is elsewhere
    return 0;
}

int a_group_handler_takes_the_whole_domain() {
    jaal::headless<App> h;
    h.send(App::Msg{StreamMsg{Delta{40}}});
    h.send(App::Msg{StreamMsg{Delta{2}}});
    h.send(App::Msg{StreamMsg{Done{}}});
    CHECK(h.model().bytes == 42);
    CHECK(h.model().done);
    CHECK(h.model().group_calls == 3);          // jaal stopped at the group
    CHECK(h.model().enters == 0);
    return 0;
}

int both_styles_in_one_program() {
    jaal::headless<App> h;
    h.send(App::Msg{ComposerMsg{Submit{"go"}}});
    h.send(App::Msg{StreamMsg{Failed{"nope"}}});
    CHECK(h.model().submitted == "go");
    CHECK(h.model().error == "nope");
    return 0;
}

// ── the plan is a fact about the program, not about the call ─────────────
namespace p = jaal::detail::prog;

// A leaf with its own handler: routed there.
static_assert(std::same_as<p::plan_of<App, Enter>, p::at_leaf>);
static_assert(std::same_as<p::plan_of<App, Submit>, p::at_leaf>);
// A group that opted in AND has a handler: routed there, NOT descended.
static_assert(std::same_as<p::plan_of<App, StreamMsg>, p::at_leaf>);
// A group that didn't opt in: descended into its alternatives, even though
// the program could have had a handler for it.
static_assert(std::same_as<p::plan_of<App, ComposerMsg>, p::into<Enter, Backspace, Submit>>);
// Every leaf under Msg is reachable, which is what Program checks.
static_assert(p::routable<App, App::Msg>);
static_assert(p::routable<App, ComposerMsg>);
static_assert(p::routable<App, StreamMsg>);

// A program missing a leaf is NOT routable. (The compile-fail test checks
// that the error names the leaf and the path; here we just need the fact,
// so it's asked in a way that doesn't fire the static_assert.)
struct Incomplete {
    struct Model { int n = 0; };
    using Msg = std::variant<ComposerMsg>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model&, Enter)     { return {}; }
    static Cmd update(Model&, Backspace) { return {}; }
    // Submit: no handler, and no group handler to catch it
};
static_assert(!p::routable<Incomplete, Submit>);
static_assert(!p::routable<Incomplete, ComposerMsg>);
static_assert(std::same_as<p::plan_of<Incomplete, Submit>, p::nowhere>);

// ── three levels, because nothing stops a group holding groups ───────────
struct Tick {};
struct Tock {};
using ClockMsg  = std::variant<Tick, Tock>;
using NestedMsg = std::variant<ClockMsg, ComposerMsg>;

struct Deep {
    struct Model { int ticks = 0, tocks = 0, enters = 0, backs = 0, submits = 0; };
    using Msg = std::variant<NestedMsg>;          // Msg -> NestedMsg -> ClockMsg -> Tick
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model& m, Tick)      { ++m.ticks;   return {}; }
    static Cmd update(Model& m, Tock)      { ++m.tocks;   return {}; }
    static Cmd update(Model& m, Enter)     { ++m.enters;  return {}; }
    static Cmd update(Model& m, Backspace) { ++m.backs;   return {}; }
    static Cmd update(Model& m, Submit)    { ++m.submits; return {}; }
};
static_assert(jaal::Program<Deep>);

int routing_goes_as_deep_as_the_variant() {
    jaal::headless<Deep> h;
    h.send(Deep::Msg{NestedMsg{ClockMsg{Tick{}}}});
    h.send(Deep::Msg{NestedMsg{ClockMsg{Tock{}}}});
    h.send(Deep::Msg{NestedMsg{ComposerMsg{Enter{}}}});
    CHECK(h.model().ticks == 1);
    CHECK(h.model().tocks == 1);
    CHECK(h.model().enters == 1);
    return 0;
}

// ── a generic handler still works, and still wins where it applies ───────
struct Catchall {
    struct Model { int seen = 0; int enters = 0; };
    using Msg = std::variant<ComposerMsg>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model& m, Enter) { ++m.enters; return {}; }   // more specific
    template <class M>
    static Cmd update(Model& m, M) { ++m.seen; return {}; }         // the rest
};
static_assert(jaal::Program<Catchall>);

int a_generic_handler_covers_the_rest() {
    jaal::headless<Catchall> h;
    h.send(Catchall::Msg{ComposerMsg{Enter{}}});
    h.send(Catchall::Msg{ComposerMsg{Backspace{}}});
    h.send(Catchall::Msg{ComposerMsg{Submit{"x"}}});
    CHECK(h.model().enters == 1);      // the exact overload won
    CHECK(h.model().seen == 2);        // the template took the others
    return 0;
}

// ── replay walks the same plan (it uses prog::update too) ────────────────
int replay_routes_identically() {
    std::vector<App::Msg> log{
        App::Msg{ComposerMsg{Enter{}}},
        App::Msg{StreamMsg{Delta{7}}},
        App::Msg{ComposerMsg{Submit{"end"}}},
    };
    const auto m = jaal::replay<App>(log);
    CHECK(m.enters == 1);
    CHECK(m.bytes == 7);
    CHECK(m.submitted == "end");
    CHECK(m.group_calls == 1);
    return 0;
}

}  // namespace

int main() {
    if (int r = leaves_go_to_their_own_handlers()) return r;
    if (int r = a_group_handler_takes_the_whole_domain()) return r;
    if (int r = both_styles_in_one_program()) return r;
    if (int r = routing_goes_as_deep_as_the_variant()) return r;
    if (int r = a_generic_handler_covers_the_rest()) return r;
    if (int r = replay_routes_identically()) return r;
    std::puts("routing_test: ok");
    return 0;
}
