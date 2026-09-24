#pragma once
// jaal::platform::posix_signals — POSIX signals through a self-pipe.
//
// Declared with NO OS headers; implemented in src/platform/posix/signals.cpp.
//
// How it works: a process-wide handler, installed per signal while anyone
// wants that signal. Each installed source owns a slot (its own pipe and
// pending bits), so several sources can each see every signal they asked
// for: one doesn't consume another's.
//
// Rules, each one a check in tests/platform/signals_test.cpp:
//   * the handler only does async-signal-safe things: lock-free atomics and
//     one write(2) per slot. It saves and restores errno.
//   * wakes coalesce: five SIGWINCHes before a take() are one `resize`
//   * take() drains the pipe BEFORE clearing the pending bits. The other
//     order can lose a signal that lands between the two.
//   * uninstalling the last source restores the PREVIOUS disposition, so an
//     app's own handler comes back
//   * a signal the process inherited as ignored stays ignored. Under nohup,
//     SIGHUP is SIG_IGN; installing a handler for it would break nohup.
//   * teardown can't write into a recycled fd. maya blocked the signal with
//     pthread_sigmask during close, but that only blocks the CALLING thread:
//     a handler already running on another thread could still write to the
//     fd after close and the kernel's reuse of it. Here the handler counts
//     itself in and out (a lock-free atomic), and teardown waits for zero
//     in-flight handlers after unpublishing the fd, before closing it.
//
// Why not signalfd on Linux: signalfd needs the signal blocked in EVERY
// thread, including threads the app or other libraries started. A library
// can't guarantee that.

#include <atomic>
#include <memory>
#include <utility>

#include "../../core/error.hpp"
#include "../signal.hpp"

namespace jaal::platform {

class posix_signals {
public:
    using native_handle = int;            // the slot pipe's read end

    [[nodiscard]] static result<posix_signals> install(signal_set wanted);

    posix_signals(posix_signals&& o) noexcept;
    posix_signals& operator=(posix_signals&& o) noexcept;
    posix_signals(const posix_signals&)            = delete;
    posix_signals& operator=(const posix_signals&) = delete;
    ~posix_signals();

    /// Watch this for read in the reactor.
    [[nodiscard]] native_handle handle() const noexcept;

    /// Signals that arrived since the last take(). Loop thread only.
    [[nodiscard]] signal_set take() noexcept;

    /// What this source asked for and actually got. A signal inherited as
    /// ignored (nohup's SIGHUP) is left ignored and is NOT in this set.
    [[nodiscard]] signal_set watching() const noexcept;

private:
    explicit posix_signals(int slot, signal_set got) noexcept : slot_(slot), got_(got) {}
    void release() noexcept;
    int        slot_ = -1;
    signal_set got_{};
};

static_assert(SignalSource<posix_signals>);

/// Test-only hook: spin iterations inside the handler between reading a
/// slot's fd and writing to it, to make teardown races reproducible. Leave
/// it at zero outside tests.
namespace test { extern std::atomic<int> signals_window; }

}  // namespace jaal::platform
