#pragma once
// jaal::child<Child, Parent, Wrap> — embed one program inside another.
//
// The parent keeps the child's Model as a field and gives the child's Msg a
// case of its own. child<> is the wiring between them, typed:
//
//   struct Left  { Counter::Msg msg; };           // a case of the parent's Msg
//   struct Right { Counter::Msg msg; };
//   using Msg = std::variant<Left, Right, Reset>;
//   using L = jaal::child<Counter, App, Left>;
//   using R = jaal::child<Counter, App, Right>;
//
//   struct Model { Counter::Model left, right; };
//   static Cmd init(Model& m) { return Cmd::batch(L::init(m.left), R::init(m.right)); }
//   static Cmd update(Model& m, Left l)  { return L::update(m.left, l); }
//   static Cmd update(Model& m, Right r) { return R::update(m.right, r); }
//   static Sub subscribe(const Model& m) {
//       return Sub::batch(L::subscribe(m.left, "left"), R::subscribe(m.right, "right"));
//   }
//
// A child's messages reach the parent as an ordinary case, so routing them
// is an ordinary update overload: no matching, no unwrapping.
//
// Rules, all checked by the compiler:
//   * Wrap is a case of Parent::Msg and holds exactly one field, the
//     child's Msg (`struct Wrap { Child::Msg msg; }`).
//   * The child's effects must fit in the parent's: the returned Cmd has
//     the CHILD's row and converts to the parent's only if every effect is
//     allowed there. A child that needs an effect the parent doesn't list
//     doesn't compile.
//   * Stream keys get the given prefix ("left/feed"), so two copies of the
//     same child don't fight over one key. `every` needs no prefix: equal
//     timers already get separate ordinals.
//
// The wrapper is a captureless function, so it's allowed on task and stream
// threads (Cmd::map / Sub::map require that). For a variable number of
// children, each with an id, see children<> (children.hpp).
//
// `Parent` is used only for its Msg, and only inside member functions, so
// `using L = jaal::child<Counter, App, Left>;` may appear inside App itself.

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "program.hpp"
#include "stream.hpp"
#include "sub.hpp"

namespace jaal {

namespace detail::childx {

template <class V, class T> struct is_alt : std::false_type {};
template <class... Ts, class T>
struct is_alt<std::variant<Ts...>, T> : std::bool_constant<(std::same_as<T, Ts> || ...)> {};

// Add a prefix to every stream key in a Sub, in place.
template <class S>
void prefix_streams(S& s, std::string_view prefix) {
    using M = typename S::msg_type;
    std::visit([&]<class X>(X& x) {
        using U = std::remove_cvref_t<X>;
        if constexpr (std::same_as<U, typename S::Batch>) {
            for (auto& inner : x.subs) prefix_streams(inner, prefix);
        } else if constexpr (in_row<fx::stream, typename S::row_type>
                             && std::same_as<U, payload_t<fx::stream, M>>) {
            x.key.insert(0, prefix.data(), prefix.size());
        }
    }, s.inner);
}

// The child's Msg inside a one-field wrapper case.
template <class Wrap>
decltype(auto) inner(Wrap&& w) noexcept {
    auto&& [msg] = std::forward<Wrap>(w);
    return std::forward<decltype(msg)>(msg);
}

}  // namespace detail::childx

template <Program Child, class Parent, class Wrap>
struct child {
    using model_type = typename Child::Model;
    using msg_type   = typename Child::Msg;

    /// Child::Msg → Parent::Msg. Captureless, so it may run on task threads.
    template <class PM = typename Parent::Msg>
    static PM wrap(msg_type m) {
        static_assert(detail::childx::is_alt<PM, Wrap>::value,
                      "jaal::child<Child, Parent, Wrap>: Wrap must be a case of Parent::Msg");
        return PM{Wrap{std::move(m)}};
    }

    /// Set up the child's model (its init) and return its first Cmd, mapped.
    [[nodiscard]] static auto init(model_type& slot) {
        auto [m, c] = prog::init<Child>();
        slot = std::move(m);
        return std::move(c).map(&wrap<>);
    }

    /// Run the child's update on `slot`, in place; return its Cmd, mapped.
    template <class W>
        requires std::same_as<std::remove_cvref_t<W>, Wrap>
    [[nodiscard]] static auto update(model_type& slot, W&& w) {
        return prog::update<Child>(slot, msg_type(detail::childx::inner(std::forward<W>(w))))
            .map(&wrap<>);
    }

    /// The child's subscriptions, mapped, with stream keys under `prefix/`.
    [[nodiscard]] static auto subscribe(const model_type& m, std::string_view prefix) {
        auto s = prog::subscribe<Child>(m);
        if (!prefix.empty()) {
            std::string p(prefix);
            p += '/';
            detail::childx::prefix_streams(s, p);
        }
        return std::move(s).map(&wrap<>);
    }
};

}  // namespace jaal
