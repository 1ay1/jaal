#pragma once
// jaal effects — descriptors, and the Effect concept every one must meet.
//
// An effect family ("after a delay, send this Msg") depends on the app's
// Msg type. C++ has no higher-kinded types, so each effect is a plain
// DESCRIPTOR type with:
//
//   name            a string for diagnostics
//   type<Msg>       the payload for a given Msg (the thing in Cmd::inner)
//   fmap(f, e)      re-target a payload at another Msg type (for Cmd::map)
//
// and optionally:
//
//   ctors<Self, Msg>   static factory functions that Cmd<Msg, Row> inherits
//                      when this effect is in its row, e.g. Cmd::after(...).
//                      This is how maya keeps its spelling
//                      (Cmd<Msg>::commit_scrollback(n)) with the effect
//                      defined in maya, not jaal.
//
// Effects that don't carry a Msg use pure_fx<T, "name">, which provides all
// of it from a plain payload type.

#include <concepts>
#include <functional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "../meta/fixed_string.hpp"
#include "../meta/type_name.hpp"

namespace jaal {

namespace detail::probe {
// Private test types for the Effect concept. The concept instantiates the
// descriptor at probe_msg, maps with probe_fn, and checks it lands on
// probe_result: so fmap really changes the Msg and nothing else.
struct probe_msg    { int v; };
struct probe_result { long v; };
struct probe_fn {
    probe_result operator()(probe_msg m) const { return {m.v}; }
};
}  // namespace detail::probe

template <class D>
concept Effect =
    requires {
        { D::name } -> std::convertible_to<std::string_view>;
        typename D::template type<detail::probe::probe_msg>;
        typename D::template type<detail::probe::probe_result>;
    }
    && std::movable<typename D::template type<detail::probe::probe_msg>>
    && requires(typename D::template type<detail::probe::probe_msg> e,
                detail::probe::probe_fn f) {
        { D::fmap(f, std::move(e)) }
            -> std::same_as<typename D::template type<detail::probe::probe_result>>;
    };

/// The payload type of effect D for message type Msg.
template <Effect D, class Msg>
using payload_t = typename D::template type<Msg>;

/// Does D's payload actually mention the Msg? (If not, the effect is
/// Msg-independent and map() just copies it across.)
template <class D>
inline constexpr bool carries_msg_v =
    !std::is_same_v<payload_t<D, detail::probe::probe_msg>,
                    payload_t<D, detail::probe::probe_result>>;

// ── pure_fx: an effect with no Msg in it ─────────────────────────────────
template <class T, meta::fixed_string Name>
    requires std::movable<T>
struct pure_fx {
    static constexpr std::string_view name = Name;
    template <class>
    using type = T;
    // No Msg inside, so mapping is the identity on the payload.
    template <class F>
    static T fmap(F&&, T e) { return e; }
};

// ── diagnostics ──────────────────────────────────────────────────────────
template <class D>
consteval std::string_view effect_name() {
    if constexpr (requires { { D::name } -> std::convertible_to<std::string_view>; })
        return D::name;
    else
        return meta::type_name<D>();
}

}  // namespace jaal
