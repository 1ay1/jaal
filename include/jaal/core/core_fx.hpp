#pragma once
// jaal::Cmd / jaal::Sub — the types a program names, and the core rows.
//
//   using Cmd = jaal::Cmd<Msg>;                 // core effects
//   using Cmd = jaal::Cmd<Msg, beep>;           // core effects + beep
//   using Sub = jaal::Sub<Msg, on_key>;         // core sources + on_key
//
// The core row is ALWAYS in: a program lists only what it adds, so there's
// one way to spell a program's effects and nothing to union by hand.
//
//   core_fx   quit, send, after, task, now, random       (Cmd)
//   core_src  every, stream                              (Sub)
//
// A program that only uses these runs on ANY host; a host adds its own
// (maya adds terminal effects, a server adds socket effects).
//
// basic_cmd / basic_sub take an exact row; they're for the kernel and for
// generic code (child, children) that maps rows it didn't choose.

#include "cmd.hpp"
#include "fx.hpp"
#include "row.hpp"
#include "stream.hpp"
#include "sub.hpp"

namespace jaal {

using core_fx  = make_row<fx::quit, fx::send, fx::after, fx::task, fx::now, fx::random>;
using core_src = make_row<fx::every, fx::stream>;

/// A program's Cmd: the core effects plus `Extra`.
template <class Msg, Effect... Extra>
using Cmd = basic_cmd<Msg, row_union<core_fx, make_row<Extra...>>>;

/// A program's Sub: the core sources plus `Extra`.
template <class Msg, Effect... Extra>
using Sub = basic_sub<Msg, row_union<core_src, make_row<Extra...>>>;

}  // namespace jaal
