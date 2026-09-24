#pragma once
// jaal::program — declare a program's effects once; get its types for free.
//
// Without this, every program spells its types out in full:
//
//   using Cmd = jaal::Cmd<Msg, jaal::row_union<jaal::core_fx, jaal::make_row<beep>>>;
//   using Sub = jaal::Sub<Msg, jaal::row_union<jaal::core_src, jaal::make_row<on_key>>>;
//   static std::pair<Model, Cmd> update(Model m, Msg msg);
//
// With it:
//
//   struct Counter : jaal::program<Counter::Model, Counter::Msg,
//                                  jaal::fx_list<beep>, jaal::src_list<on_key>> {
//       ...
//       static step update(Model m, Msg msg) {
//           if (...) return {m, Cmd::quit()};
//           return m;                         // a Model alone means "no effects"
//       }
//   };
//
// program<> is a pure base of type aliases: no state, no virtuals, nothing
// the kernel depends on. A struct that spells its types by hand is exactly
// as much a Program as one that inherits these.
//
// The core rows (quit/after/task/now, every/stream) are always
// included; fx_list / src_list add to them.

#include <utility>

#include "../meta/list.hpp"
#include "cmd.hpp"
#include "core_fx.hpp"
#include "row.hpp"
#include "sub.hpp"

namespace jaal {

/// Extra effects a program uses, beyond core_fx.
template <class... Ds> struct fx_list {};
/// Extra subscription kinds a program uses, beyond core_src.
template <class... Ds> struct src_list {};

/// What update() returns: the new model and an effect. Constructible from a
/// Model alone (meaning "no effects"), so the common case needs no ceremony.
template <class Model, class C>
struct step {
    Model model;
    C     cmd{};

    step(Model m) : model(std::move(m)) {}                           // NOLINT: implicit on purpose
    step(Model m, C c) : model(std::move(m)), cmd(std::move(c)) {}
    template <class E>
        requires std::constructible_from<C, E> && (!std::same_as<std::remove_cvref_t<E>, C>)
    step(Model m, E&& e) : model(std::move(m)), cmd(std::forward<E>(e)) {}

    // So a step works wherever the kernel expects std::pair<Model, Cmd>.
    operator std::pair<Model, C>() && { return {std::move(model), std::move(cmd)}; }  // NOLINT
};

template <class Model, class Msg, class Fx = fx_list<>, class Src = src_list<>>
struct program;

template <class Model_, class Msg_, class... Fx, class... Src>
struct program<Model_, Msg_, fx_list<Fx...>, src_list<Src...>> {
    using Model = Model_;
    using Msg   = Msg_;
    using fx_row  = row_union<core_fx, make_row<Fx...>>;
    using src_row = row_union<core_src, make_row<Src...>>;
    using Cmd  = jaal::Cmd<Msg, fx_row>;
    using Sub  = jaal::Sub<Msg, src_row>;
    using step = jaal::step<Model, Cmd>;
};

}  // namespace jaal
