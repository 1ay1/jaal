#pragma once
// jaal::kernel::delay — a wait that cancellation can cut short.
//
// Every background job eventually needs "do nothing for N, unless we are
// asked to stop": a GC that should not run in the first seconds of startup,
// a refresh that backs off, a retry. The shape is small and the hand-rolled
// version is always the same three members bolted onto the subsystem:
//
//     struct Bg {
//         std::mutex              mu;
//         std::condition_variable cv;
//         bool                    stop = false;
//     };
//     // ... and a notify_all() in whatever function means "shut down"
//
// Three members, a lock discipline and a shutdown edge, all to express one
// duration. Worse, it is a SECOND cancellation channel running beside the
// stop_token the job already has, so "stop" has two spellings that have to
// be kept in sync — and the thing that gets forgotten is the notify, which
// turns a cancelled job into one that still sleeps out its full delay.
//
// There is a standard answer (condition_variable_any::wait_for with a
// stop_token), it is not obvious, and its return value is easy to read
// backwards. So: one function, one meaning.
//
// WHY THE RETURN VALUE IS "STOPPED" AND NOT "TIMED OUT"
//
// Because the caller's next line is almost always `if (...) return;`. Naming
// the true case after the ABNORMAL outcome means the early-exit reads
// positively at every call site, and a caller who ignores the result still
// gets correct blocking behaviour rather than an inverted branch.
//
// NOT A POLL. The wait parks on the condition variable and is woken by the
// stop_token's own callback, so a cancelled sleep returns promptly instead
// of at the next tick of some granularity. A 20-second delay costs one
// parked thread and zero wakeups until it is either due or cancelled.

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>

namespace jaal::kernel {

/// Block for `d`, or until `st` is stopped, whichever comes first.
///
/// Returns true if the stop won (so the caller should bail), false if the
/// duration elapsed normally. A default-constructed / never-stopped token
/// makes this a plain sleep.
///
///     if (jaal::kernel::delay_for(st, 20s)) return;   // asked to stop
///     do_the_work();
template <class Rep, class Period>
[[nodiscard]] inline bool delay_for(std::stop_token st,
                                    std::chrono::duration<Rep, Period> d) {
    // Fast paths first: an already-stopped token and a non-positive duration
    // must not pay for a mutex, a condition variable and a stop_callback
    // registration. The second case is the common one in a retry loop whose
    // backoff has been clamped to zero.
    if (st.stop_requested()) return true;
    if (d <= std::chrono::duration<Rep, Period>::zero()) return false;

    // The mutex is a formality: nothing else can reach it, so it is never
    // contended and never held across anything but the wait itself. It
    // exists because condition_variable_any wants a Lockable, not because
    // there is shared state to protect — the shared state IS the token.
    std::mutex                   m;
    std::condition_variable_any  cv;
    std::unique_lock             lk(m);
    // wait_for returns the PREDICATE's value, so making the predicate "were
    // we stopped" is what makes the result mean "stopped" rather than the
    // easily-inverted "did not time out".
    return cv.wait_for(lk, st, d, [&st] { return st.stop_requested(); });
}

}  // namespace jaal::kernel
