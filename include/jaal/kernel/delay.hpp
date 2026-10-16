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

#include "../core/capability.hpp"

namespace jaal::kernel {

namespace delay_detail {
// The wait itself, shared by delay_for and backoff_for. True if stopped.
template <class Rep, class Period>
[[nodiscard]] inline bool timed_wait(std::stop_token st, std::chrono::duration<Rep, Period> d) {
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
}  // namespace delay_detail

/// Block for `d`, or until `st` is stopped, whichever comes first.
///
/// Returns true if the stop won (so the caller should bail), false if the
/// duration elapsed normally. A default-constructed / never-stopped token
/// makes this a plain sleep.
///
/// Takes `unlocked`: a sleep under a lock stalls every thread that needs
/// it, so one doesn't compile. Inside a lock, use backoff_for.
///
///     if (jaal::kernel::delay_for(cap, st, 20s)) return;   // asked to stop
///     do_the_work();
template <class Rep, class Period>
[[nodiscard]] inline bool delay_for(const unlocked&, std::stop_token st,
                                    std::chrono::duration<Rep, Period> d) {
    // Still checked at run time: a capability minted where it was a lie
    // (thread_root inside a lock body) would otherwise slip through.
    jaal::detail::cap::require_unlocked("delay_for");
    return delay_detail::timed_wait(std::move(st), d);
}

/// Longest pause backoff_for will take.
inline constexpr std::chrono::milliseconds kMaxBackoff{2000};

/// A short pause INSIDE an operation that may run under a lock: the retry
/// backoff in an HTTP client whose caller holds a single-flight lane across
/// the request (a token refresh). Such a lane already holds across the
/// network wait, and this pause is part of that wait: it waits for time, not
/// for another thread, so it can't deadlock. delay_for refuses it only
/// because a long sleep under a lock stalls the lock's other users, so this
/// one is capped at kMaxBackoff. Wakes on stop like delay_for; same result.
///
/// Use delay_for for everything else.
template <class Rep, class Period>
[[nodiscard]] inline bool backoff_for(std::stop_token st,
                                      std::chrono::duration<Rep, Period> d) {
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(d);
    return delay_detail::timed_wait(std::move(st), ms > kMaxBackoff ? kMaxBackoff : ms);
}

}  // namespace jaal::kernel
