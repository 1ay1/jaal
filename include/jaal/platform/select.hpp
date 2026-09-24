#pragma once
// jaal::platform::native_reactor / native_signals — the backends for the OS
// this is being compiled for.
//
// This is the ONE #if chain over operating systems in jaal (docs/design.md §7).
// Everything above it names native_reactor and native_signals; nothing
// above it names an OS.
//
//   Linux           epoll_reactor   + posix_signals
//   macOS, BSDs     kqueue_reactor  + posix_signals   (macOS run; BSDs compiled)
//   other POSIX     poll_reactor    + posix_signals
//   Windows         wait_reactor    + console_signals

#include "concepts.hpp"
#include "signal.hpp"

#if defined(_WIN32)
#  include "windows/console_signals.hpp"
#  include "windows/wait_reactor.hpp"
namespace jaal::platform {
using native_reactor = wait_reactor;
using native_signals = console_signals;
}  // namespace jaal::platform
#elif defined(__linux__)
#  include "linux/epoll_reactor.hpp"
#  include "posix/signals.hpp"
namespace jaal::platform {
using native_reactor = epoll_reactor;
using native_signals = posix_signals;
}  // namespace jaal::platform
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#  include "darwin/kqueue_reactor.hpp"
#  include "posix/signals.hpp"
namespace jaal::platform {
using native_reactor = kqueue_reactor;
using native_signals = posix_signals;
}  // namespace jaal::platform
#elif defined(__unix__)
#  include "posix/poll_reactor.hpp"
#  include "posix/signals.hpp"
namespace jaal::platform {
using native_reactor = poll_reactor;
using native_signals = posix_signals;
}  // namespace jaal::platform
#else
#  error "jaal: no platform backend for this OS"
#endif

namespace jaal::platform {
static_assert(Reactor<native_reactor>);
static_assert(SignalSource<native_signals>);
}  // namespace jaal::platform
