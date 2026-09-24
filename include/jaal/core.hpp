#pragma once
// jaal::core — the types apps write against, with no kernel.
//
//   #include <jaal/core.hpp>
//
// Program, Cmd/Sub and their rows, the core effect descriptors, Sink,
// Sendable/Frozen, shared<T>, diff, child programs and the rng. Depends on
// meta/ only: nothing here starts a thread, waits on a handle or names an OS.
//
// Include this when you're defining effects, writing a library of
// descriptors, or unit-testing pure update() logic. For a whole application
// use <jaal/jaal.hpp>, which adds the kernel, run<P>() and the test hosts.
//
// Layer umbrellas (docs/design.md §7): <jaal/meta.hpp>, <jaal/core.hpp>,
// <jaal/kernel.hpp>, <jaal/platform.hpp>, <jaal/host.hpp>, and
// <jaal/jaal.hpp> for everything.

#include "core/child.hpp"
#include "core/children.hpp"
#include "core/cmd.hpp"
#include "core/core_fx.hpp"
#include "core/debounce.hpp"
#include "core/diff.hpp"
#include "core/effect.hpp"
#include "core/error.hpp"
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
