#pragma once
// jaal — a typed Elm runtime for C++.
//
//   #include <jaal/jaal.hpp>
//
// is everything an application needs: Program, Cmd/Sub, the core effects
// and sources (quit, send, after, task, now, random, every, stream),
// routers, composition (child, children), the kernel, run<P>(), the test
// hosts, faults, tracing, replay and resume. Platform backends come in
// through run.hpp; nothing here names an OS.
//
// The shape of a program is in core/program.hpp.
//
// Docs: docs/scope.md (what's offered), docs/decisions.md (why).

#include "core/child.hpp"
#include "core/children.hpp"
#include "core/cmd.hpp"
#include "core/core_fx.hpp"
#include "core/debounce.hpp"
#include "core/diff.hpp"
#include "core/effect.hpp"
#include "core/frozen.hpp"
#include "core/fx.hpp"
#include "core/program.hpp"
#include "core/rng.hpp"
#include "core/router.hpp"
#include "core/row.hpp"
#include "core/sendable.hpp"
#include "core/shared.hpp"
#include "core/sink.hpp"
#include "core/stream.hpp"
#include "core/sub.hpp"
#include "kernel/executor.hpp"
#include "kernel/fault.hpp"
#include "kernel/guarded.hpp"
#include "kernel/kernel.hpp"
#include "kernel/replay.hpp"
#include "kernel/run.hpp"
#include "kernel/scope.hpp"
#include "kernel/timeline.hpp"
#include "kernel/trace.hpp"
#include "host/given.hpp"
#include "host/headless.hpp"
#include "host/sim.hpp"

namespace jaal {
inline constexpr int version_major = 0;
inline constexpr int version_minor = 2;
inline constexpr int version_patch = 0;
}  // namespace jaal
