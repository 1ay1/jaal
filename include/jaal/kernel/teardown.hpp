#pragma once
// jaal::kernel::teardown — shutdown order as a type, not as a comment.
//
// A running program owns things that must be given up in ONE order, and the
// order is not obvious:
//
//   1. signal handlers   come off FIRST. Shutdown can take seconds (a task
//                        that ignores its stop token holds it for the whole
//                        grace), and a handler left installed catches ^C
//                        into a pipe nobody drains any more: the process
//                        becomes unkillable during its own exit (D34).
//   2. host.release()    the host's turn: restore the terminal, close
//                        sockets. The loop is over, so no more events.
//   3. kernel.finish()   stop timers and sources, ask tasks to stop, join
//                        workers within the grace, close the mailbox.
//
// jaal wrote those as three statements at the end of run(), and that was
// wrong in two ways at once. An exception from a host callback jumped clean
// over them (measured: release() never ran, and the kernel then shut down
// for the full grace with the handlers still live — the same unkillable
// process, reached by a different path). And anyone writing their own driver
// — which docs/hosts.md invites — had to know the order and repeat it.
//
// Two changes make the mistake unwritable:
//
//   * the order is a DESTRUCTOR. Members are destroyed in reverse
//     declaration order on every path out of a scope: return, break, or
//     throw. There's no method to call and nothing to forget.
//   * kernel::finish() takes a teardown_key, which only this class can
//     make. So the last step can't be taken early, or alone, or out of
//     order: `std::move(k).finish()` doesn't compile.
//
// Using it:
//
//   kernel::teardown guard{k, host, std::move(sigs)};
//   while (...) { ... }
//   return guard.exit_code();      // or just fall out of the scope
//
// The signal source is taken BY VALUE, so the caller gives it up: an lvalue
// won't bind, and a second owner can't keep the handlers installed.

#include <optional>
#include <utility>

#include "kernel.hpp"

namespace jaal::kernel {

/// A signal source that isn't there: for a driver with no signals. Destroying
/// it does nothing, and it's falsy so the same loop code works either way.
struct no_signals {
    constexpr explicit operator bool() const noexcept { return false; }
};

/// Owns the shutdown order of a running program. Declare one after the
/// kernel and let it go out of scope.
///
/// K: the kernel (borrowed; it must outlive this).
/// H: the host (borrowed).
/// S: the signal source, moved in; `no_signals` when there is none.
template <class K, class H, class S>
class teardown {
public:
    teardown(K& kernel, H& host, S sigs) noexcept
        : sigs_(std::move(sigs)), host_(host), kernel_(kernel) {}

    teardown(const teardown&)            = delete;
    teardown& operator=(const teardown&) = delete;
    teardown(teardown&&)                 = delete;
    teardown& operator=(teardown&&)      = delete;

    /// The signal source, for the loop to drain while it runs.
    [[nodiscard]] S&       signals() noexcept       { return sigs_; }
    [[nodiscard]] const S& signals() const noexcept { return sigs_; }

    /// Shut down now, in order, and report the exit code. The destructor
    /// calls this if the caller doesn't, so the order holds either way;
    /// calling it twice is harmless.
    int exit_code() noexcept {
        if (!code_) code_ = run_it();
        return *code_;
    }

    ~teardown() { (void)exit_code(); }

private:
    int run_it() noexcept {
        // 1. signals first. Destroying the source restores each signal's
        //    PREVIOUS disposition (an inherited SIG_IGN stays ignored), so
        //    from here a second ^C acts as it would have without jaal.
        { S dead = std::move(sigs_); (void)dead; }

        // 2. the host's turn. It's program code, so it can throw: catching
        //    here is what keeps step 3 reachable. A wedged worker must not
        //    outlive the process's last chance to stop it.
        if constexpr (requires { host_.release(); }) {
            try { host_.release(); } catch (...) {}
        }

        // 3. the kernel: bounded, and the source of the exit code.
        return std::move(kernel_).finish(teardown_key{});
    }

    S                  sigs_;
    H&                 host_;
    K&                 kernel_;
    std::optional<int> code_;
};

}  // namespace jaal::kernel
