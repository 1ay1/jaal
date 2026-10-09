#pragma once
// jaal::platform::native_reactor / native_signals / native_file_lock — the
// backends for the OS this is being compiled for.
//
// This is the ONE #if chain over operating systems in jaal (docs/design.md §7).
// Everything above it names native_reactor and native_signals; nothing
// above it names an OS.
//
//   Linux           epoll_reactor   + posix_signals   + posix_file_lock   + posix_process
//   macOS, BSDs     kqueue_reactor  + posix_signals   + posix_file_lock   + posix_process
//   other POSIX     poll_reactor    + posix_signals   + posix_file_lock   + posix_process
//   Windows         wait_reactor    + console_signals + windows_file_lock + windows_process

#include "concepts.hpp"
#include "file_lock.hpp"
#include "process.hpp"
#include "signal.hpp"

#if defined(_WIN32)
#  include "windows/console_signals.hpp"
#  include "windows/file_lock.hpp"
#  include "windows/process.hpp"
#  include "windows/wait_reactor.hpp"
namespace jaal::platform {
using native_reactor   = wait_reactor;
using native_signals   = console_signals;
using native_file_lock = windows_file_lock;
using native_process   = windows_process;
}  // namespace jaal::platform
#elif defined(__linux__)
#  include "linux/epoll_reactor.hpp"
#  include "posix/file_lock.hpp"
#  include "posix/process.hpp"
#  include "posix/signals.hpp"
namespace jaal::platform {
using native_reactor   = epoll_reactor;
using native_signals   = posix_signals;
using native_file_lock = posix_file_lock;
using native_process   = posix_process;
}  // namespace jaal::platform
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#  include "darwin/kqueue_reactor.hpp"
#  include "posix/file_lock.hpp"
#  include "posix/process.hpp"
#  include "posix/signals.hpp"
namespace jaal::platform {
using native_reactor   = kqueue_reactor;
using native_signals   = posix_signals;
using native_file_lock = posix_file_lock;
using native_process   = posix_process;
}  // namespace jaal::platform
#elif defined(__unix__)
#  include "posix/file_lock.hpp"
#  include "posix/poll_reactor.hpp"
#  include "posix/process.hpp"
#  include "posix/signals.hpp"
namespace jaal::platform {
using native_reactor   = poll_reactor;
using native_signals   = posix_signals;
using native_file_lock = posix_file_lock;
using native_process   = posix_process;
}  // namespace jaal::platform
#else
#  error "jaal: no platform backend for this OS"
#endif

namespace jaal::platform {
static_assert(Reactor<native_reactor>);
static_assert(SignalSource<native_signals>);
static_assert(FileLock<native_file_lock>);
static_assert(Process<native_process>);
}  // namespace jaal::platform
