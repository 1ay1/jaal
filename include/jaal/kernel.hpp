#pragma once
// jaal::kernel — the loop as a value, plus the tools that inspect a run.
//
//   #include <jaal/kernel.hpp>
//
//   kernel<P, Event, Clock>  the loop: start / route / step / finish
//   run<P>(host)             drive it on the native platform until it quits
//   scope / nursery          structured background work tied to a lifetime
//   recorder / replay        fold a recorded run again, effects excluded
//   timeline / diff          walk a recorded run step by step
//
// Depends on meta/, core/ and platform/. A host drives the kernel; it never
// owns the thread (docs/decisions.md D4).
//
// Layer umbrellas (docs/design.md §7): <jaal/meta.hpp>, <jaal/core.hpp>,
// <jaal/kernel.hpp>, <jaal/platform.hpp>, <jaal/host.hpp>, and
// <jaal/jaal.hpp> for everything.

#include "kernel/fault.hpp"
#include "kernel/guarded.hpp"
#include "kernel/kernel.hpp"
#include "kernel/mailbox.hpp"
#include "kernel/replay.hpp"
#include "kernel/run.hpp"
#include "kernel/scope.hpp"
#include "kernel/timeline.hpp"
#include "kernel/trace.hpp"
