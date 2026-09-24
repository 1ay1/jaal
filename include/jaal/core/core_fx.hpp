#pragma once
// jaal core rows: the effects and sources every kernel runs itself.
//
//   core_fx   quit, after, task, now, random              (Cmd)
//   core_src  every, stream                            (Sub)
//
// A program that only uses these runs on ANY host; a host adds its own
// (maya adds terminal effects, a server adds socket effects) with
// row_union.

#include "cmd.hpp"
#include "fx.hpp"
#include "row.hpp"
#include "stream.hpp"
#include "sub.hpp"

namespace jaal {

using core_fx  = make_row<fx::quit, fx::after, fx::task, fx::now, fx::random>;
using core_src = make_row<fx::every, fx::stream>;

/// The Cmd / Sub types for a program that only uses the core rows.
template <class Msg> using CoreCmd = Cmd<Msg, core_fx>;
template <class Msg> using CoreSub = Sub<Msg, core_src>;

}  // namespace jaal
