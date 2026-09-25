#pragma once
// jaal::platform — the capability layer: waiting, time and signals.
//
//   #include <jaal/platform.hpp>
//
//   Reactor          the concept every backend satisfies
//   native_reactor   epoll (Linux), kqueue (macOS/BSD), WaitForMultipleObjects
//                    (Windows), poll (other POSIX)
//   native_signals   signals as events, never as handler callbacks
//   owned_handle     an OS handle as a linear resource (move-only, one close)
//   steady_clock / sim_clock
//
// NO OS headers anywhere in this tree: they live only in src/platform/<os>/,
// so <windows.h>'s macros never reach user code. select.hpp holds the one
// #if chain over operating systems.
//
// Layer umbrellas (docs/design.md §7): <jaal/meta.hpp>, <jaal/core.hpp>,
// <jaal/kernel.hpp>, <jaal/platform.hpp>, <jaal/host.hpp>, and
// <jaal/jaal.hpp> for everything.

#include "platform/clock.hpp"
#include "platform/concepts.hpp"
#include "platform/handle.hpp"
#include "platform/select.hpp"
#include "platform/signal.hpp"
