#pragma once
// jaal::child<Child, ParentMsg, Wrap> — embed one program inside another.
//
// The Elm way to compose: the parent keeps the child's Model as a field,
// wraps the child's Msg in one of its own, and maps the child's Cmd and Sub
// so their messages come back wrapped. Done by hand that's the same
// boilerplate every time; child<> is that boilerplate, typed.
//
//   struct Left  { Counter::Msg msg; };           // parent Msg alternatives
//   struct Right { Counter::Msg msg; };
//   using Msg = std::variant<Left, Right, Reset>;
//   using L = jaal::child<Counter, Msg, Left>;
//   using R = jaal::child<Counter, Msg, Right>;
//
//   struct Model { Counter::Model left = L::model(), right = R::model(); };
//   static std::pair<Model, Cmd> init() {
//       auto [l, lc] = L::init();  auto [r, rc] = R::init();
//       return {{l, r}, Cmd::batch(lc, rc)};
//   }
//   static std::pair<Model, Cmd> update(Model m, Msg msg) {
//       if (auto* c = L::match(msg)) return {m, L::update(m.left, *c)};
//       if (auto* c = R::match(msg)) return {m, R::update(m.right, *c)};
//       ...
//   }
//   static Sub subscribe(const Model& m) {
//       return Sub::batch(L::subscribe(m.left, "left"), R::subscribe(m.right, "right"));
//   }
//
// Rules, all checked by the compiler:
//   * Wrap is an alternative of ParentMsg and is built from a Child::Msg
//     (an aggregate `struct Wrap { Child::Msg msg; }` is the usual shape).
//   * The child's Cmd/Sub rows must fit in the parent's: the returned Cmd
//     has the CHILD's row and converts to the parent's only if every effect
//     is allowed there (the usual widening rule). A child that needs an
//     effect the parent doesn't list doesn't compile.
//   * Stream keys get the given prefix ("left/feed"), so two copies of the
//     same child don't fight over one key. `every` needs no prefix: equal
//     timers already get separate ordinals.
//
// The wrapper is a captureless function, so it's allowed on task and stream
// threads (Cmd::map / Sub::map require that). That's also why one child<>
// is one fixed slot: a LIST of children (each with an id) needs the id in
// the mapper, and that isn't supported yet.

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "../meta/list.hpp"
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

}  // namespace detail::childx

template <Program Child, class ParentMsg, class Wrap>
struct child {
    using model_type = typename Child::Model;
    using msg_type   = typename Child::Msg;
    using cmd_type   = decltype(std::declval<cmd_of<Child>>().map(
                           std::declval<ParentMsg (*)(msg_type)>()));

    static_assert(detail::childx::is_alt<ParentMsg, Wrap>::value,
                  "jaal::child<Child, ParentMsg, Wrap>: Wrap must be one of ParentMsg's "
                  "alternatives");
    static_assert(std::is_constructible_v<Wrap, msg_type>
                      || requires(msg_type m) { Wrap{std::move(m)}; },
                  "jaal::child<Child, ParentMsg, Wrap>: Wrap must be buildable from the "
                  "child's Msg, e.g. struct Wrap { Child::Msg msg; }");

    /// Child::Msg → ParentMsg. Captureless, so it may run on task threads.
    static ParentMsg wrap(msg_type m) { return ParentMsg{Wrap{std::move(m)}}; }

    /// The child's message inside `m`, or null when `m` isn't for this child.
    [[nodiscard]] static const msg_type* match(const ParentMsg& m) noexcept {
        if (auto* w = std::get_if<Wrap>(&m)) return &unwrap(*w);
        return nullptr;
    }

    /// The child's initial model and its init Cmd, mapped.
    [[nodiscard]] static std::pair<model_type, cmd_type> init() {
        auto [m, c] = run_init<Child>();
        return {std::move(m), std::move(c).map(&wrap)};
    }

    /// The child's initial model alone (its init Cmd is dropped).
    [[nodiscard]] static model_type model() { return run_init<Child>().first; }

    /// Run the child's update on `slot` in place; return its Cmd, mapped.
    [[nodiscard]] static cmd_type update(model_type& slot, msg_type msg) {
        auto [next, cmd] = detail::prog::split(Child::update(std::move(slot), std::move(msg)));
        slot = std::move(next);
        return std::move(cmd).map(&wrap);
    }

    /// The child's subscriptions, mapped, with stream keys prefixed.
    [[nodiscard]] static auto subscribe(const model_type& m, std::string_view prefix) {
        auto s = std::move(run_subscribe<Child>(m)).map(&wrap);
        if (!prefix.empty()) {
            std::string p(prefix);
            p += '/';
            detail::childx::prefix_streams(s, p);
        }
        return s;
    }

private:
    static const msg_type& unwrap(const Wrap& w) noexcept {
        if constexpr (std::is_convertible_v<const Wrap&, const msg_type&>) return w;
        else {
            // The aggregate shape: one field holding the child's Msg.
            const auto& [inner] = w;
            return inner;
        }
    }
};

}  // namespace jaal
