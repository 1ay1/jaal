#pragma once
// jaal::Program — the contract an app writes against.
//
//   struct Counter {
//       struct Model { int n = 0; };
//       using Msg = std::variant<Tick, Quit>;
//
//       static Model init();                              // or {Model, Cmd}
//       static std::pair<Model, Cmd> update(Model, Msg);   // required
//       static Element view(const Model&);                 // optional, host-defined
//       static Sub subscribe(const Model&);                // optional
//   };
//
// The EFFECT SET is deduced, not declared: whatever row the Cmd returned by
// update carries is the program's row (fx_of<P>). Same for subscribe
// (src_of<P>). So a program never writes its row twice, and a host can
// check "can I run everything this program asks for?" from the type alone.
//
// view is deliberately NOT part of Program: a server program has none, and
// a host that draws asks for Viewable<P, ItsOutput> instead.
//
// What C++ can't check: that update and view are PURE. jaal can't stop
// update from calling printf. The headless host makes violations easy to
// spot (an effect done by hand won't appear in the recorded list).

#include <concepts>
#include <tuple>
#include <type_traits>
#include <utility>

#include "cmd.hpp"
#include "overload.hpp"
#include "sendable.hpp"
#include "sub.hpp"

namespace jaal {

template <class Model, class C> struct step;   // program_base.hpp

namespace detail::prog {

template <class P>
using step_t = decltype(P::update(std::declval<typename P::Model>(),
                                  std::declval<typename P::Msg>()));

template <class S> struct step_parts;
template <class M, class C> struct step_parts<std::pair<M, C>> {
    using model = M;
    using cmd   = C;
};
// jaal::step<Model, Cmd> (program_base.hpp): what update() returns when a
// program uses jaal::program<>. Same parts as the pair.
template <class M, class C>
struct step_parts<::jaal::step<M, C>> {
    using model = M;
    using cmd   = C;
};

/// Split whatever update()/init() returned into (model, cmd).
template <class S>
auto split(S&& s) {
    if constexpr (requires { s.model; s.cmd; })
        return std::pair{std::move(s.model), std::move(s.cmd)};
    else
        return std::pair{std::move(s.first), std::move(s.second)};
}

template <class P> using cmd_t = typename step_parts<step_t<P>>::cmd;

template <class P>
concept well_formed_step = requires {
    typename step_t<P>;
    typename step_parts<step_t<P>>::model;
    typename step_parts<step_t<P>>::cmd;
} && std::same_as<typename step_parts<step_t<P>>::model, typename P::Model>
  && detail::cmd::is_cmd_v<cmd_t<P>>
  && std::same_as<typename cmd_t<P>::msg_type, typename P::Msg>;

template <class P> using sub_t = decltype(P::subscribe(std::declval<const typename P::Model&>()));

}  // namespace detail::prog

/// The effect row a program's update can return.
template <class P>
using fx_of = typename detail::prog::cmd_t<P>::row_type;

/// The program's Cmd type.
template <class P>
using cmd_of = detail::prog::cmd_t<P>;

/// Does P define subscribe?
template <class P>
concept Subscribing = requires {
    typename detail::prog::sub_t<P>;
} && detail::sub::is_sub_v<detail::prog::sub_t<P>>
  && std::same_as<typename detail::prog::sub_t<P>::msg_type, typename P::Msg>;

/// The subscription row a program asks for (empty when it has no subscribe).
template <class P>
struct src_of_impl { using type = row<>; };
template <Subscribing P>
struct src_of_impl<P> { using type = typename detail::prog::sub_t<P>::row_type; };
template <class P> using src_of = typename src_of_impl<P>::type;

template <class P> using sub_of = typename detail::prog::sub_t<P>;

// ── init, in two shapes ──────────────────────────────────────────────────
template <class P>
concept HasPlainInit = requires {
    { P::init() } -> std::same_as<typename P::Model>;
};

template <class P>
concept HasCmdInit = requires {
    { P::init() } -> std::same_as<std::pair<typename P::Model, cmd_of<P>>>;
} || requires {
    // init() returning jaal::step<Model, Cmd> (program_base.hpp)
    requires std::same_as<typename detail::prog::step_parts<
                              decltype(P::init())>::model, typename P::Model>;
    requires std::same_as<typename detail::prog::step_parts<
                              decltype(P::init())>::cmd, cmd_of<P>>;
};

// ── the concept ──────────────────────────────────────────────────────────
template <class P>
concept Program =
    requires {
        typename P::Model;
        typename P::Msg;
    }
    && std::movable<typename P::Model>
    && Sendable<typename P::Msg>
    && detail::prog::well_formed_step<P>
    && (HasPlainInit<P> || HasCmdInit<P>);

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

// ── running a program's init, either shape ───────────────────────────────
template <Program P>
[[nodiscard]] auto run_init() -> std::pair<typename P::Model, cmd_of<P>> {
    if constexpr (HasCmdInit<P>) return detail::prog::split(P::init());
    else                         return {P::init(), cmd_of<P>::none()};
}

/// The subscriptions for a model (none when the program has no subscribe).
template <Program P>
[[nodiscard]] auto run_subscribe(const typename P::Model& m) {
    if constexpr (Subscribing<P>) return P::subscribe(m);
    else                          return Sub<typename P::Msg, row<>>{};
}

}  // namespace jaal
