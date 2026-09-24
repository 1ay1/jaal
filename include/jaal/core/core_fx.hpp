#pragma once
// jaal core rows: the effects every kernel runs itself.

#include "cmd.hpp"
#include "fx.hpp"
#include "row.hpp"

namespace jaal {

using core_fx = make_row<fx::quit, fx::after, fx::task, fx::isolated_task>;

/// The Cmd type for a program that only uses core effects.
template <class Msg>
using CoreCmd = Cmd<Msg, core_fx>;

}  // namespace jaal
