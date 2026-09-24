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
#include "../meta/type_name.hpp"
#include "cmd.hpp"
#include "core_fx.hpp"
#include "sendable.hpp"
#include "sub.hpp"

namespace jaal {

namespace detail::prog {

template <class T> inline constexpr bool is_variant_v = false;
template <class... Ts> inline constexpr bool is_variant_v<std::variant<Ts...>> = true;

/// Does P have an update for message case C?
template <class P, class C>
concept updates_case = requires(typename P::Model& m, C&& c) {
    { P::update(m, std::move(c)) } -> std::convertible_to<typename P::Cmd>;
};

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

// The runtime half of the plan. `route` is the one function that decides
// where a message goes, and it asks plan_of the same question the concept
// did — so a program that compiles dispatches exactly where the check said.
template <class P, class C>
[[nodiscard]] typename P::Cmd route(typename P::Model& m, C&& c) {
    using plan = plan_of<P, std::remove_cvref_t<C>>;
    if constexpr (std::same_as<plan, at_leaf>) {
        return P::update(m, std::forward<C>(c));
    } else if constexpr (std::same_as<plan, nowhere>) {
        return {};                    // unreachable for a Program: check_route
                                      // already failed, and this keeps that to
                                      // ONE error instead of a second here.
    } else {
        return std::visit(
            [&]<class L>(L&& leaf) -> typename P::Cmd {
                return route<P>(m, std::forward<L>(leaf));
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

/// Optional: render once off-screen before the real frame (warm caches).
template <class P>
concept HasNeedsWarmup = requires(const typename P::Model& m) {
    { P::needs_warmup(m) } -> std::convertible_to<bool>;
};

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
template <Program P>
[[nodiscard]] typename P::Cmd update(typename P::Model& m, typename P::Msg msg) {
    return std::visit(
        [&]<class C>(C&& c) -> typename P::Cmd {
            return detail::prog::route<P>(m, std::forward<C>(c));
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
