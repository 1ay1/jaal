#pragma once
// jaal — a typed Elm runtime for C++.
//
//   #include <jaal/jaal.hpp>
//
// is everything an application needs: Program, Cmd/Sub, the core effects
// and sources (quit, after, task, now, every, stream), routers, program<>
// aliases, the kernel, run<P>(), the headless test host, faults, tracing
// and replay. Platform backends come in through run.hpp; nothing here names
// an OS.
//
// Docs: docs/scope.md (what's offered), docs/decisions.md (why).

#include "core/cmd.hpp"
#include "core/core_fx.hpp"
#include "core/diff.hpp"
#include "core/effect.hpp"
#include "core/frozen.hpp"
#include "core/fx.hpp"
#include "core/overload.hpp"
#include "core/program.hpp"
#include "core/program_base.hpp"
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
