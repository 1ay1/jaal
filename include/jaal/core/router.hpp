#pragma once
// jaal::router / jaal::source — define a subscription kind in one line.
//
// Writing a descriptor by hand is ~20 lines: a payload template, an fmap
// that re-targets it at another Msg, a route() or key(), and a factory for
// Sub::. All of it is mechanical. These generate it:
//
//   using on_key   = jaal::router<KeyEvent, "on_key">;
//   using on_mouse = jaal::router<MouseEvent, "on_mouse">;
//
//   using Sub = jaal::Sub<Msg, jaal::make_row<on_key, on_mouse>>;
//   Sub::on(on_key{}, [](const KeyEvent& k) -> std::optional<Msg> { ... });
//
// A router's filter is a plain callable Event -> optional<Msg>. It runs on
// the loop thread inside dispatch, and is rebuilt with every subscribe(),
// so it may capture freely (unlike a task body).
//
// The factory is Sub::on(Tag{}, f), one name for every router kind, picked
// by the tag. (Descriptors written by hand can still add named factories
// like Sub::on_key through `ctors`; router<> sticks to the tag so two
// routers never collide on a name.)
//
// A convenience form accepts a filter that returns a Msg directly (always
// fires) or a bool-returning predicate plus a Msg:
//
//   Sub::on(on_key{}, [](const KeyEvent&) { return Msg{Tick{}}; });

#include <concepts>
#include <functional>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "../meta/fixed_string.hpp"
#include "effect.hpp"

namespace jaal {

template <class Event, meta::fixed_string Name>
struct router {
    static constexpr std::string_view name = Name;
    using event_type = Event;
    using tag        = router;

    template <class Msg> struct type {
        std::function<std::optional<Msg>(const Event&)> filter;
    };

    template <class F, class M>
    static auto fmap(F&& f, type<M> e) -> type<std::invoke_result_t<F, M>> {
        using B = std::invoke_result_t<F, M>;
        return {[g = std::move(e.filter), f = std::forward<F>(f)](const Event& ev) -> std::optional<B> {
            if (auto m = g(ev)) return f(std::move(*m));
            return std::nullopt;
        }};
    }

    template <class M>
    static std::optional<M> route(const type<M>& p, const Event& ev) { return p.filter(ev); }

    // Not inherited into Sub as a base (see inherit_ctors below):
    // Sub::on(tag, f) calls ctors<Self, Msg>::on directly, so two router
    // kinds in one row never collide on a name.
    template <class Self, class Msg> struct ctors {
        /// Sub::on(tag{}, f) where f: const Event& -> optional<Msg>, or -> Msg.
        template <class F>
            requires std::invocable<F&, const Event&>
        [[nodiscard]] static Self on(router, F f) {
            using R = std::invoke_result_t<F&, const Event&>;
            if constexpr (std::is_convertible_v<R, std::optional<Msg>>
                          && !std::is_convertible_v<R, Msg>)
                return Self(type<Msg>{std::move(f)});
            else
                return Self(type<Msg>{[f = std::move(f)](const Event& e) -> std::optional<Msg> {
                    return Msg(f(e));
                }});
        }
    };
    // Sub inherits every descriptor's `ctors` as a base. router<>'s factory
    // is reached through Sub::on instead, so it opts out of inheritance.
    static constexpr bool inherit_ctors = false;
};

}  // namespace jaal
