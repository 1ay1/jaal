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

// Checked one case at a time so a failure names the case (and, on C++26,
// the line to add). Returns true so it can sit in a fold expression.
template <class P, class C>
consteval bool check_case() {
    if constexpr (!updates_case<P, C>) {
#if defined(__cpp_static_assert) && __cpp_static_assert >= 202306L
        static_assert(updates_case<P, C>,
                      meta::cat<512>("jaal: '", meta::type_name<P>(),
                                     "' has no update for message '", meta::type_name<C>(),
                                     "'; add: static Cmd update(Model&, ",
                                     meta::type_name<C>(), ")"));
#else
        static_assert(updates_case<P, C>,
                      "jaal: a Msg case has no update(Model&, Case) returning Cmd");
#endif
    }
    return true;
}

template <class P, class V> inline constexpr bool updates_every_case = false;
template <class P, class... Cs>
inline constexpr bool updates_every_case<P, std::variant<Cs...>> = (check_case<P, Cs>() && ...);

template <class P>
concept has_init = requires(typename P::Model& m) {
    { P::init(m) } -> std::convertible_to<typename P::Cmd>;
};

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
template <Program P>
[[nodiscard]] typename P::Cmd update(typename P::Model& m, typename P::Msg msg) {
    return std::visit(
        [&]<class C>(C&& c) -> typename P::Cmd {
            // Always true for a Program; the check keeps a missing case to
            // ONE error (check_case's) instead of a second from this call.
            if constexpr (detail::prog::updates_case<P, std::remove_cvref_t<C>>)
                return P::update(m, std::forward<C>(c));
            else
                return {};
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
