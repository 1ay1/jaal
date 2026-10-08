#pragma once
// jaal::Program — the one shape an app is written in.
//
//   struct Counter {
//       struct Model { int n = 0; };
//       struct Inc {};
//       struct Reset {};
//       using Msg = std::variant<Inc, Reset>;
//       using Cmd = jaal::Cmd<Msg>;                    // + extra effects: Cmd<Msg, beep>
//       using Sub = jaal::Sub<Msg>;                    // optional; + extra sources
//
//       static Cmd init(Model& m);                     // optional
//       static Cmd update(Model& m, Inc);              // one per Msg case
//       static Cmd update(Model& m, Reset);
//       static Sub subscribe(const Model& m);          // optional
//   };
//
// That's the whole contract, and there is no second way to write it:
//
//   * Msg is a std::variant. The kernel dispatches each alternative to the
//     update overload for it, so there's no std::visit to write, and a
//     case with no update is a compile error that NAMES the case and the
//     line to add. Overload resolution does the matching, so one generic
//     overload (`template <class M> static Cmd update(Model&, M)`) can
//     handle a family of cases.
//   * Msg may be a variant OF VARIANTS, which is how a big app is organised
//     (agentty: 232 message types in 20 domains, one reducer TU each). jaal
//     routes down the tree to the leaves, so each leaf still gets its own
//     checked update. A domain that would rather be handled in one place
//     opts in by name — see handled_as_group below.
//   * update takes the Model BY REFERENCE and returns only the Cmd. The
//     model is changed in place; there is no pair to build and nothing to
//     forget to return. `return {};` means "no effects".
//   * init is optional: without it the Model is value-initialised. With it,
//     init sets the model up and returns its first Cmd.
//   * Cmd is DECLARED, not deduced from what update happens to return, so
//     the program's effect set is one line you can read.
//
// view is deliberately NOT part of Program: a server has none, and a host
// that draws asks for Viewable<P, ItsOutput> instead.
//
// What C++ can't check: that update is PURE apart from the model it's
// given. jaal can't stop update from calling printf. The headless host
// makes violations easy to spot (an effect done by hand won't appear in the
// recorded list), and replay makes them reproducible.
//
// Everything that runs a program (kernel, given, replay, timeline, child,
// children) goes through prog::init / prog::update / prog::subscribe below:
// ONE place knows how a program is called.

#include <concepts>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <variant>

#include "../meta/diagnose.hpp"
#include "../meta/declares.hpp"
#include "../meta/type_name.hpp"
#include "cmd.hpp"
#include "core_fx.hpp"
#include "sendable.hpp"
#include "sub.hpp"

namespace jaal {

namespace detail::prog {

template <class T> inline constexpr bool is_variant_v = false;
template <class... Ts> inline constexpr bool is_variant_v<std::variant<Ts...>> = true;

// ── time, as an argument ─────────────────────────────────────────────────────────────
//
// A program that needs the time declares the clock it reads:
//
//     using Clock = jaal::platform::steady_clock;
//     static Cmd update(Model& m, Tick, Clock::time_point now);
//
// and then EVERY update takes `now` as its third argument. The kernel fills
// it with the step's time (one read per step, shared by every message the
// step folds — see kernel::step_now for why that matters).
//
// This is the purity rule made structural rather than advisory. The
// alternative — reducers calling std::chrono::steady_clock::now() — is a
// read of global mutable state, and its cost is concrete: replay re-runs
// update() with a different time and gets a different model (D23), a test
// can't hold time still, and two messages in one step see two "nows". With
// time as an argument, a reducer cannot see the clock without the caller
// supplying it, and the caller is the kernel, which owns the clock.
//
// Why an argument and not a Model field the kernel writes: a field is state
// update can read at any moment, including moments it is stale (a helper
// called from view(), a model copied into a test). An argument exists only
// for the duration of the fold it belongs to.
//
// It is per-PROGRAM, not per-handler. Declaring Clock is the decision; every
// case then takes `now`, and a handler that forgot it is a compile error
// naming the case. A program that doesn't declare Clock is unchanged.

JAAL_DECLARES_MEMBER(declares_clock, Clock);

// What a program's declared Clock must be. Stated here, in core, rather than
// reusing platform::Clock: core sits BELOW platform in jaal's layering
// (docs/design.md §7), and the program shape is a core concept. This is the
// same rule with no OS behind it — a steady clock with a time_point — and
// both platform clocks satisfy it, which the kernel static_asserts.
//
// Steady, as platform::Clock requires, for the same reason: a wall clock can
// jump backwards when the user changes it, and a reducer comparing `now`
// against a deadline it set earlier would then see time run in reverse.
template <class C>
concept steady_program_clock = requires {
    typename C::duration;
    typename C::time_point;
    requires C::is_steady;
} && requires(C& c) {
    { c.now() } -> std::same_as<typename C::time_point>;
};

template <class P>
concept clocked = requires { typename P::Clock; }
                  && steady_program_clock<typename P::Clock>;

/// The type update's `now` parameter has: the declared clock's time_point,
/// or an empty tag a non-clocked program never sees.
struct no_time {};
template <class P, bool = clocked<P>> struct now_of { using type = no_time; };
template <class P> struct now_of<P, true> { using type = typename P::Clock::time_point; };
template <class P> using now_t = typename now_of<P>::type;

/// Does P have an update for message case C? A clocked program's update
/// takes the step time as a third argument; an unclocked one does not.
/// Exactly one shape is accepted per program, so a clocked program whose
/// handler forgot `now` is a missing case — the diagnostic names it.
template <class P, class C>
concept updates_case =
    (clocked<P> &&
     requires(typename P::Model& m, C&& c, now_t<P> t) {
         { P::update(m, std::move(c), t) } -> std::convertible_to<typename P::Cmd>;
     })
    || (!clocked<P> &&
     requires(typename P::Model& m, C&& c) {
         { P::update(m, std::move(c)) } -> std::convertible_to<typename P::Cmd>;
     });

// ── the routing plan ────────────────────────────────────────────────
//
// A big app doesn't have one flat Msg. agentty has 232 message types in 20
// groups (`using ComposerMsg = std::variant<ComposerEnter, ...>`, and
// `using Msg = std::variant<ComposerMsg, StreamMsg, ...>`), so that each
// group's reducer lives in its own translation unit and touching one leaf
// doesn't rebuild the world.
//
// So routing is a TREE, and the rule at each node is the one C++ already
// taught everyone — the most specific handler wins:
//
//   update(Model&, ComposerEnter)   a LEAF handler: jaal calls it
//   update(Model&, ComposerMsg)     a GROUP handler: jaal calls it and does
//                                   NOT descend (that domain is handled whole)
//   neither, and it's a variant      descend and ask the same question of
//                                   each alternative
//   neither, and it isn't            the program is incomplete: a compile
//                                   error naming the case AND the path to it
//
// A program picks per domain: leaf handlers where exhaustiveness is worth
// having, one group handler where a domain is better handled in one place.
// Both compose, in the same Msg.
//
// The plan is computed ONCE, as a type, and read TWICE: the Program concept
// checks it, and prog::update walks it. Neither reimplements the other, so
// "it compiled" and "it dispatches there" cannot drift apart — which is the
// property that makes a 232-case program safe to refactor.

}  // namespace detail::prog

// Handling a GROUP whole is opted into BY NAME. Two reasons, and the second
// is the one that matters:
//
//   * A catch-all `template <class M> update(Model&, M)` matches a group type
//     just as happily as a leaf. If that counted as "handles this group", the
//     group would route there and the program's own leaf handlers for that
//     domain would silently never run — and every exhaustiveness check below
//     it would be switched off. The code compiles, the dispatch is wrong, and
//     the check that should have caught it is the thing that got disabled.
//     (Measured while building this: a Catchall program sent Enter and the
//     generic handler took it. There is no way to ask C++ "is this overload a
//     template?" — taking the address of the exact signature succeeds either
//     way — so inference can't be made safe here.)
//
//   * "This whole domain is handled in one place" is a DESIGN decision about
//     an app's structure. It deserves a line that says so, not a property
//     that appears because of how an overload happened to be written.
//
// So: specialise handled_as_group for the variant type.
//
//     template <> inline constexpr bool jaal::handled_as_group<StreamMsg> = true;
//     static Cmd update(Model&, StreamMsg s);     // now reached
//
// Without it, jaal descends to the leaves and each one needs its own update
// — which is the default worth having, because it's the one that's checked.

/// Opt a Msg group into being handled by one update(Model&, Group) instead
/// of per leaf. See the note above.
template <class Group>
inline constexpr bool handled_as_group = false;

namespace detail::prog {

struct at_leaf {};                       // P::update(Model&, C) exists: call it
template <class... Ls> struct into {};    // C is a group: route each of Ls
struct nowhere {};                       // no handler, and nothing to descend into

template <class C> struct group_of            { using type = nowhere; };
template <class... Ls> struct group_of<std::variant<Ls...>> { using type = into<Ls...>; };

/// What jaal will do with a message of type C in program P.
template <class P, class C>
using plan_of = std::conditional_t<
    is_variant_v<C>,
    std::conditional_t<::jaal::handled_as_group<C> && updates_case<P, C>,
                       at_leaf, typename group_of<C>::type>,
    std::conditional_t<updates_case<P, C>, at_leaf, nowhere>>;

// Is every leaf under C reachable? (A variable template, not a concept:
// concepts can't recurse.)
template <class P, class C, class Plan> inline constexpr bool routable_with = false;
template <class P, class C> inline constexpr bool routable_with<P, C, at_leaf> = true;
template <class P, class C> inline constexpr bool routable_with<P, C, nowhere> = false;

template <class P, class C>
inline constexpr bool routable = routable_with<P, C, plan_of<P, C>>;

template <class P, class C, class... Ls>
inline constexpr bool routable_with<P, C, into<Ls...>> = (routable<P, Ls> && ...);

// ── diagnostics ────────────────────────────────────────────────────────
// A missing case two levels down is useless if the error just says "Msg".
// The check carries the path it took, so the message reads
//   Msg -> ComposerMsg -> ComposerEnter
// which is the group whose file you need to open.

template <class... Path>
consteval auto path_text() {
    meta::message<768> m;
    bool first = true;
    const auto step = [&](std::string_view n) {
        if (!first) m += " -> ";
        first = false;
        m.append_short(n, 96);
    };
    (step(meta::type_name<Path>()), ...);
    return m;
}

// Checked one case at a time so a failure names THAT case (and, on C++26,
// the path and the line to add). Returns true so it can sit in a fold.
//
// The recursion is expressed by passing the plan AS AN ARGUMENT: overload
// resolution picks the step, so the three cases are three overloads rather
// than one function that has to be declared before itself.
template <class P, class C, class... Path>
consteval bool check_route();

template <class P, class C, class... Path>
consteval bool check_step(at_leaf) { return true; }

template <class P, class C, class... Path>
consteval bool check_step(nowhere) {
#if defined(__cpp_static_assert) && __cpp_static_assert >= 202306L
    static_assert(routable<P, C>,
                  meta::cat<1024>("jaal: '", meta::type_name<P>(),
                                  "' has no update for message '", meta::type_name<C>(),
                                  "' (reached as ", path_text<Path..., C>().view(),
                                  "); add: static Cmd update(Model&, ",
                                  meta::type_name<C>(), ")"));
#else
    static_assert(routable<P, C>,
                  "jaal: a Msg case has no update(Model&, Case) returning Cmd");
#endif
    return true;
}

template <class P, class C, class... Path, class... Ls>
consteval bool check_step(into<Ls...>) {
    return (check_route<P, Ls, Path..., C>() && ...);
}

template <class P, class C, class... Path>
consteval bool check_route() {
    return check_step<P, C, Path...>(plan_of<P, C>{});
}

template <class P, class V> inline constexpr bool updates_every_case = false;
template <class P, class... Cs>
inline constexpr bool updates_every_case<P, std::variant<Cs...>> =
    (check_route<P, Cs, std::variant<Cs...>>() && ...);

template <class P>
concept has_init = requires(typename P::Model& m) {
    { P::init(m) } -> std::convertible_to<typename P::Cmd>;
};

// ── near-miss detection for optional hooks ─────────────────────────────
//
// Every optional hook (init, subscribe, subs_key, view, visual_hash,
// needs_warmup) is detected with a requires-test on the exact callable
// shape. A hook whose SIGNATURE has drifted silently reads as "the program
// doesn't have that hook" and jaal uses the default — a value-initialised
// Model, no subscriptions, subs re-run every step. This has bitten twice:
// AgenttyApp::init once returned std::pair<Model,Cmd> from an older shape
// (settings loaded then thrown away because the kernel value-initialised a
// fresh Model), and maya's terminal_host::attach took a base host_context
// instead of the derived one (attach never ran, no keys got in).
//
// So each optional hook gets a companion probe: does P *declare* a member
// with that name at all? JAAL_DECLARES_MEMBER (meta/declares.hpp) answers
// that on the NAME alone, so it sees a hook whatever shape it took —
// including the drifted shape, which is the whole point. (The obvious
// spelling, `requires { &P::init; }`, silently reports "absent" for a
// template member or an overload set, so it cannot be used for a check
// whose job is to catch wrong shapes.) If the probe is true and the shape
// check is false, the program wrote the hook with the wrong signature — a
// static_assert names the hook and shows the expected shape. If the probe
// is false too, the program legitimately opted out.
//
// The check runs once, in check_hooks<P>() from kernel::start, so a bad
// signature fails at kernel construction with a message that points at the
// hook — not on the first missed subscribe half an hour into the session.

JAAL_DECLARES_MEMBER(declares_init,         init);
JAAL_DECLARES_MEMBER(declares_subscribe,    subscribe);
JAAL_DECLARES_MEMBER(declares_subs_key,     subs_key);
JAAL_DECLARES_MEMBER(declares_view,         view);
JAAL_DECLARES_MEMBER(declares_visual_hash,  visual_hash);
JAAL_DECLARES_MEMBER(declares_needs_warmup, needs_warmup);

// The runtime half of the plan. `route` is the one function that decides
// where a message goes, and it asks plan_of the same question the concept
// did — so a program that compiles dispatches exactly where the check said.
//
// `now` rides the whole walk and is handed to the leaf. For an unclocked
// program it is the empty no_time tag and is never passed on, so its
// handlers keep their two-argument shape.
template <class P, class C>
[[nodiscard]] typename P::Cmd route(typename P::Model& m, C&& c, now_t<P> now = {}) {
    using plan = plan_of<P, std::remove_cvref_t<C>>;
    if constexpr (std::same_as<plan, at_leaf>) {
        if constexpr (clocked<P>)
            return P::update(m, std::forward<C>(c), now);
        else
            return P::update(m, std::forward<C>(c));
    } else if constexpr (std::same_as<plan, nowhere>) {
        return {};                    // unreachable for a Program: check_route
                                      // already failed, and this keeps that to
                                      // ONE error instead of a second here.
    } else {
        return std::visit(
            [&]<class L>(L&& leaf) -> typename P::Cmd {
                return route<P>(m, std::forward<L>(leaf), now);
            },
            std::forward<C>(c));
    }
}

template <class P>
concept has_subscribe = requires(const typename P::Model& m) {
    typename P::Sub;
    { P::subscribe(m) } -> std::same_as<typename P::Sub>;
};

}  // namespace detail::prog

// ── the concept ──────────────────────────────────────────────────────────
template <class P>
concept Program =
    requires {
        typename P::Model;
        typename P::Msg;
        typename P::Cmd;
    }
    && std::movable<typename P::Model>
    && std::default_initializable<typename P::Model>
    && detail::prog::is_variant_v<typename P::Msg>
    && Sendable<typename P::Msg>
    && detail::cmd::is_cmd_v<typename P::Cmd>
    && std::same_as<typename P::Cmd::msg_type, typename P::Msg>
    && detail::prog::updates_every_case<P, typename P::Msg>;

/// Does P subscribe to anything?
template <class P>
concept Subscribing = Program<P> && detail::prog::has_subscribe<P>;

/// The program's Cmd type, and the effect row it may return.
template <Program P> using cmd_of = typename P::Cmd;
template <Program P> using fx_of  = typename P::Cmd::row_type;

namespace detail::prog {
template <class P> struct sub_impl { using type = basic_sub<typename P::Msg, row<>>; };
template <Subscribing P> struct sub_impl<P> { using type = typename P::Sub; };
}  // namespace detail::prog

/// The program's Sub type (an empty-row Sub when it doesn't subscribe), and
/// the source row it may ask for.
template <Program P> using sub_of = typename detail::prog::sub_impl<P>::type;
template <Program P> using src_of = typename sub_of<P>::row_type;

/// P produces Out from its model (a host that draws requires this).
template <class P, class Out>
concept Viewable = Program<P> && requires(const typename P::Model& m) {
    { P::view(m) } -> std::convertible_to<Out>;
};

/// Optional: skip view() when the hash hasn't changed since the last one.
template <class P>
concept HasVisualHash = requires(const typename P::Model& m) {
    { P::visual_hash(m) } -> std::convertible_to<std::uint64_t>;
};

/// Optional: the part of the model subscribe() depends on.
///
///   static auto subs_key(const Model& m) {
///       return std::tuple{m.streaming, m.panel, m.timer_ms};
///   }
///
/// subscribe() runs after EVERY model change, and re-running it means
/// rebuilding the Sub and diffing it against what's running. But a model
/// changes far more often than its subscriptions do — a keystroke edits the
/// composer text; it doesn't open a panel or start a stream — so almost all
/// of that work finds nothing to do. Measured: a program with ONE timer paid
/// 71.7 ns per message against 24.1 with no subscribe at all; the difference
/// is entirely the rebuild and diff.
///
/// With subs_key, the kernel compares the key against the last one and
/// calls subscribe() only when it changed. The key is the program's own
/// statement of "these are the fields subscribe() reads" — the same idea as
/// visual_hash for view(), and agentty already structures its subscribe()
/// that way (it reads a dozen fields of a 665-line model).
///
/// It's a VALUE, not a hash, on purpose: a hash can collide, and a collision
/// here would silently keep a stale subscription (a timer that should have
/// stopped, a router for a closed panel). Equality can't be wrong in that
/// direction. Any equality-comparable, copyable type works: a tuple of the
/// fields, a small struct, an int.
///
/// The rule that keeps it honest: subs_key must cover EVERYTHING subscribe()
/// reads — including anything its routers CAPTURE. That second half is the
/// one that bites. A router may capture the model freely (it's rebuilt on
/// every subscribe, see core/router.hpp), so
///
///     Sub::on(on_key{}, [text = m.composer](const Key& k) { ... })
///
/// holds a COPY of m.composer. Skip the rebuild and that copy goes stale:
/// the next key is routed against old text. So a field a router captures
/// belongs in subs_key exactly as much as a field that decides which timers
/// run. Leave one out and a change to it won't re-subscribe.
///
/// Debug builds check it: on every call the key says is unchanged, the
/// kernel runs subscribe() anyway and asserts the result is equivalent
/// (same sources, same payloads). So a key that's too narrow fails loudly
/// in development instead of silently in production. The check can't see
/// inside a router's captures (they're opaque callables), which is why the
/// rule above is stated for them in words.
template <class P>
concept HasSubsKey = requires(const typename P::Model& m) {
    { P::subs_key(m) };
    requires std::equality_comparable<decltype(P::subs_key(m))>;
    requires std::copyable<decltype(P::subs_key(m))>;
};

/// Optional: render once off-screen before the real frame (warm caches).
template <class P>
concept HasNeedsWarmup = requires(const typename P::Model& m) {
    { P::needs_warmup(m) } -> std::convertible_to<bool>;
};

namespace detail::prog {

// ── check_hooks<P>() ────────────────────────────────────────────────────
//
// Called from kernel::start. For each optional hook, if the program
// DECLARES a member with that name but the callable shape doesn't match
// what jaal will actually invoke, fail here with a message that names the
// hook and the expected signature. If the member isn't declared at all,
// stay silent — the program legitimately opted out.
//
// Doing this once, at kernel construction, means "my init/subscribe/view
// silently didn't run" becomes a compile error at the line where the
// program is handed to the kernel, not a mystery weeks later.

template <class P>
consteval void check_hooks() {
    if constexpr (declares_init<P>) {
        static_assert(has_init<P>,
            "jaal: P::init is declared but its signature doesn't match "
            "the shape jaal calls. Expected: static Cmd init(Model&). "
            "An older shape (e.g. std::pair<Model,Cmd> init()) is silently "
            "skipped, leaving the kernel to value-initialise the Model — "
            "every field your init() set gets thrown away.");
    }
    if constexpr (declares_subscribe<P>) {
        static_assert(has_subscribe<P>,
            "jaal: P::subscribe is declared but its signature doesn't "
            "match. Expected: static Sub subscribe(const Model&) where Sub "
            "is a jaal::Sub<Msg,...>. A wrong shape means jaal thinks the "
            "program has no subscriptions — timers never arm, streams never "
            "start, on_signal never fires.");
    }
    if constexpr (declares_subs_key<P>) {
        static_assert(::jaal::HasSubsKey<P>,
            "jaal: P::subs_key is declared but doesn't satisfy HasSubsKey. "
            "Expected: static auto subs_key(const Model&) returning a "
            "copyable, equality-comparable value. A wrong shape means the "
            "key is ignored and subscribe() re-runs every message — correct, "
            "but wastes the fast path this hook exists to give you.");
    }
    if constexpr (declares_visual_hash<P>) {
        static_assert(::jaal::HasVisualHash<P>,
            "jaal: P::visual_hash is declared but its signature doesn't "
            "match. Expected: static std::uint64_t visual_hash(const Model&). "
            "A wrong shape means the host redraws on every model change — "
            "the point of the hook is lost.");
    }
    if constexpr (declares_needs_warmup<P>) {
        static_assert(::jaal::HasNeedsWarmup<P>,
            "jaal: P::needs_warmup is declared but its signature doesn't "
            "match. Expected: static bool needs_warmup(const Model&).");
    }
    // view() is checked for CALLABILITY on the model only. Whether its
    // return type is what the host draws is Viewable<P, Out>'s job, and Out
    // isn't known here — the host checks that. But "view() cannot be called
    // with the model at all" is unambiguous drift, and worth catching here
    // because a headless run never asks Viewable and would not notice.
    if constexpr (declares_view<P>) {
        static_assert(requires(const typename P::Model& m) { P::view(m); },
            "jaal: P::view is declared but its signature doesn't match. "
            "Expected: static Out view(const Model&), where Out is whatever "
            "the host draws (maya::Element for a terminal host). A view() "
            "that can't be called with the model means the program cannot "
            "draw at all.");
    }
}

}  // namespace detail::prog

// ── calling a program: the only place that does ──────────────────────────
namespace prog {

/// A fresh model and its first Cmd (init, or a value-initialised Model).
template <Program P>
[[nodiscard]] std::pair<typename P::Model, typename P::Cmd> init() {
    typename P::Model m{};
    if constexpr (detail::prog::has_init<P>) {
        typename P::Cmd c = P::init(m);
        return {std::move(m), std::move(c)};
    } else {
        return {std::move(m), typename P::Cmd{}};
    }
}

/// Fold one message into the model, in place; return its Cmd.
///
/// The ROOT is always descended. `Msg` is the program's message SET, not a
/// message: a handler taking the whole `Msg` would be a program with no
/// cases, and — worse — a generic `template <class M> update(Model&, M)`
/// matches `Msg` too, so treating the root as a leaf would silently swallow
/// every message at the top and disable every exhaustiveness check below it.
/// The concept folds over the root's alternatives for the same reason, so
/// the two agree by construction.
///
/// Below the root, `route` walks the plan the concept checked
/// (detail::prog::plan_of): a leaf handler is called, a group with its own
/// handler is called whole, anything else is descended into.
///
/// Two overloads, so the time is never silently defaulted. An unclocked
/// program calls update(m, msg) as it always has. A clocked program MUST
/// pass `now` — there is deliberately no default for it, because a
/// forgotten argument would compile and fold at the epoch, which is the
/// exact "reducer sees the wrong time" bug this mechanism exists to remove.
/// The kernel supplies the step time; given<>, replay and timeline supply
/// the time they recorded or were told.
template <Program P>
    requires(!detail::prog::clocked<P>)
[[nodiscard]] typename P::Cmd update(typename P::Model& m, typename P::Msg msg) {
    return std::visit(
        [&]<class C>(C&& c) -> typename P::Cmd {
            return detail::prog::route<P>(m, std::forward<C>(c));
        },
        std::move(msg));
}

template <Program P>
    requires detail::prog::clocked<P>
[[nodiscard]] typename P::Cmd update(typename P::Model& m, typename P::Msg msg,
                                     detail::prog::now_t<P> now) {
    return std::visit(
        [&]<class C>(C&& c) -> typename P::Cmd {
            return detail::prog::route<P>(m, std::forward<C>(c), now);
        },
        std::move(msg));
}

/// The subscriptions for a model (none when the program has no subscribe).
template <Program P>
[[nodiscard]] sub_of<P> subscribe(const typename P::Model& m) {
    if constexpr (Subscribing<P>) return P::subscribe(m);
    else                          return sub_of<P>{};
}

}  // namespace prog

}  // namespace jaal
