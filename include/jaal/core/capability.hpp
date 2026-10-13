#pragma once
// jaal capabilities — lock order as a type, checked by the compiler.
//
// THE PROBLEM
//
// Every lock-order deadlock has one shape: a thread holds lock A and asks
// for lock B while another thread holds B and asks for A. A runtime checker
// can only report that shape after a thread has walked into it, and only on
// the paths a test happened to run. This header removes the shape from the
// set of programs that compile.
//
// THE MODEL
//
// Every lock has a RANK, a type: `jaal::rank<20, "persistence.lane">`. A
// thread may only take a lock whose rank is strictly higher than every lock
// it already holds. Ranks are a strict total order, so two threads following
// the rule can never wait on each other in a cycle.
//
// The rule is enforced with CAPABILITIES, values that prove what the current
// thread holds:
//
//   jaal::unlocked          the thread holds no lock at all
//   jaal::held<R>           the thread holds a lock of rank R, and nothing
//                           ranked higher (handed to a lock's body)
//   jaal::below<R>          a PARAMETER type: "my caller holds only locks
//                           ranked lower than R", so this function may take
//                           locks of rank R and above
//
// Taking a lock of rank R needs a `below<R>`. `unlocked` and any `held<Q>`
// with Q < R convert to it; nothing else does. Inside the lock's body the
// thread gets a `held<R>`, which only converts to `below<S>` for S > R. So:
//
//   * taking a lower-ranked lock inside a higher one      does not compile
//   * taking two locks of the same rank (two leaves)      does not compile
//   * re-entering the lock you hold                       does not compile
//   * a blocking wait with any lock held                  does not compile
//                                                         (it needs `unlocked`)
//
// WHERE CAPABILITIES COME FROM
//
// Only jaal makes them, and only where the answer is known:
//
//   * at a thread's entry: every pool job, isolated job, worker_group job,
//     task body and stream body may take `const jaal::unlocked&` as its first
//     parameter, and jaal passes one (a fresh thread holds nothing)
//   * inside a lock's body: guarded<T, R>::with hands the body `held<R>`
//   * at the top of main(): jaal::thread_root(f). It is the one free-standing
//     constructor, so the ban-list (tests/lint) rejects it outside the files
//     allowlisted for it. A call inside a lock body would be a lie, and that
//     is exactly what the ban-list exists to make visible.
//
// They cannot be copied, moved, stored, or have their address taken, so a
// capability cannot outlive the frame that proved it, and cannot be carried
// to another thread (they are not Sendable, so no task argument or lock-body
// argument can be one). A lock body is captureless and its arguments are
// Sendable, so it cannot reach an outer capability either: the only one it
// has is the `held<R>` it was given.
//
// WHAT IS NOT CHECKED
//
// C++ has no borrow checker, so one escape remains: binding a capability to
// a `static` reference, or std::addressof on one. Both are deliberate, both
// are textual, and the ban-list flags them. Raw std::mutex and std::thread
// are banned outside allowlisted files for the same reason.

#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "../meta/fixed_string.hpp"
#include "sendable.hpp"

namespace jaal {

/// A lock's place in the global order. Lower ranks are taken first.
///
///     using auth_refresh = jaal::rank<15, "auth.refresh">;
///     jaal::guarded<Lane, auth_refresh> lane;
///
/// Values are 1..65534; 0 is "nothing held" and 65535 is `leaf`.
template <std::uint16_t Value, meta::fixed_string Name>
struct rank {
    static_assert(Value > 0 && Value < 0xFFFF,
                  "jaal::rank: values are 1..65534 (0 means \"nothing held\", "
                  "65535 is jaal::leaf)");
    static constexpr std::uint16_t    value = Value;
    static constexpr std::string_view name  = Name;
};

/// The default rank: taken last. A leaf's body can take no lock at all, so
/// two leaves can never be held together.
struct leaf {
    static constexpr std::uint16_t    value = 0xFFFF;
    static constexpr std::string_view name  = "leaf";
};

namespace detail::cap {
/// The rank of "nothing held". Not a Rank: no lock can be ranked here.
struct nothing {
    static constexpr std::uint16_t    value = 0;
    static constexpr std::string_view name  = "nothing";
};
template <class R>
concept has_rank_shape = requires {
    { R::value } -> std::convertible_to<std::uint16_t>;
    { R::name } -> std::convertible_to<std::string_view>;
};
}  // namespace detail::cap

/// A type usable as a lock's rank: jaal::rank<N, "name"> or jaal::leaf.
template <class R>
concept Rank = detail::cap::has_rank_shape<R> && (R::value > 0);

/// `A` is taken strictly before `B` in the global order.
template <class A, class B>
concept ranked_before = detail::cap::has_rank_shape<A> && detail::cap::has_rank_shape<B>
                        && (A::value < B::value);

struct cap_access;

namespace detail::cap {
// The rank of the innermost lock this thread holds through a guarded (0 =
// none). The types already forbid every bad order; this catches the one way
// around them -- a capability minted where it was a lie (thread_root inside
// a lock body) -- at the wait or lock it would have deadlocked on.
// One TLS load per lock, next to a mutex that costs ~20x that.
inline std::uint16_t& held_top() noexcept {
    thread_local std::uint16_t top = 0;
    return top;
}

[[noreturn]] inline void lie(const char* what) noexcept {
    std::fprintf(stderr,
                 "jaal: %s while this thread holds a lock (rank %u). A capability "
                 "was minted where it wasn't true: thread_root() inside a lock body?\n",
                 what, static_cast<unsigned>(held_top()));
    std::abort();
}

/// A blocking wait: the thread must hold nothing.
inline void require_unlocked(const char* what) noexcept {
    if (held_top() != 0) [[unlikely]] lie(what);
}
}  // namespace detail::cap

/// jaal's own terminal locks (core/capability.hpp cap_access::terminal).
/// A distinct type, so a terminal lock can never be confused with a real
/// `unlocked` and needs no tracking.
class terminal_cap {
    constexpr terminal_cap() noexcept = default;
    friend struct cap_access;
public:
    terminal_cap(const terminal_cap&)            = delete;
    terminal_cap& operator=(const terminal_cap&) = delete;
    void operator&() const                       = delete;
};

/// Proof that the current thread holds a lock of rank Top and nothing
/// ranked higher. held<nothing> is `unlocked`.
///
/// Pass by const reference. It cannot be copied, moved, or addressed, so it
/// cannot outlive the frame that proved it or reach another thread.
template <class Top>
class held {
public:
    using top = Top;

    held(const held&)            = delete;
    held(held&&)                 = delete;
    held& operator=(const held&) = delete;
    held& operator=(held&&)      = delete;
    void operator&() const       = delete;

private:
    constexpr held() noexcept = default;
    friend struct cap_access;
};

/// Proof that the current thread holds no lock at all. Required by every
/// operation that blocks waiting on another thread: guarded::wait_with,
/// pool::shutdown, worker_group::stop.
using unlocked = held<detail::cap::nothing>;

/// A parameter type: "the caller holds only locks ranked below R". A
/// function that takes locks of rank R (or above) declares one:
///
///     Token fresh_token(const jaal::below<auth_refresh>& cap);
///
/// It converts from `unlocked`, from `held<Q>` with Q < R, and from
/// `below<S>` with S <= R (a weaker promise). Never from anything else, so a
/// call from inside a lock ranked R or higher does not compile.
template <class R>
    requires Rank<R>
class below {
public:
    template <class Top>
        requires(Top::value < R::value)
    constexpr below(const held<Top>&) noexcept {}   // NOLINT: implicit on purpose

    template <class S>
        requires(Rank<S> && S::value <= R::value && !std::same_as<S, R>)
    constexpr below(const below<S>&) noexcept {}    // NOLINT: implicit on purpose

    below(const below&)            = delete;
    below(below&&)                 = delete;
    below& operator=(const below&) = delete;
    below& operator=(below&&)      = delete;
    void operator&() const         = delete;

    using limit = R;
};

/// jaal's key for making capabilities. Only jaal's own kernel and host code
/// use it (the ban-list enforces that); everything else receives them.
struct cap_access {
    template <class Top>
    [[nodiscard]] static constexpr held<Top> mint() noexcept { return held<Top>{}; }
    [[nodiscard]] static constexpr unlocked fresh_thread() noexcept { return unlocked{}; }

    /// For jaal's OWN terminal locks only: a leaf whose body is a few lines
    /// of jaal code that call nothing (push to a vector, flip a bool). Such a
    /// body cannot take a second lock, so it cannot be half of a lock-order
    /// cycle no matter what its caller holds -- which is why it may run from
    /// a user lock's body (a Sink post that wakes the headless host). Never
    /// for a lock whose body runs user code.
    [[nodiscard]] static constexpr terminal_cap terminal() noexcept { return terminal_cap{}; }
};

/// The root capability for a thread jaal did not start: main(), a test's
/// main, a thread a C library calls back on. f(const jaal::unlocked&) runs
/// with it. Calling this anywhere a lock might be held is a lie the type
/// system cannot see, so the ban-list only allows it in allowlisted files,
/// and it aborts at runtime if this thread holds a guarded lock.
template <class F>
    requires std::invocable<F&, const unlocked&>
decltype(auto) thread_root(F&& f) {
    detail::cap::require_unlocked("jaal::thread_root");
    const unlocked u = cap_access::fresh_thread();
    return f(u);
}

namespace detail::cap {
/// Is T one of the capability types? For "not Sendable" and diagnostics.
template <class T> inline constexpr bool is_capability = false;
template <class Top> inline constexpr bool is_capability<held<Top>> = true;
template <class R> inline constexpr bool is_capability<below<R>> = true;
template <> inline constexpr bool is_capability<terminal_cap> = true;

/// Does a body take a leading `const unlocked&`? Thread-entry points use
/// this to decide whether to hand one over.
template <class F, class... Rest>
concept wants_unlocked = std::is_invocable_v<F&, const unlocked&, Rest...>;
}  // namespace detail::cap

/// A type whose DESTRUCTOR waits for other threads (a pool, a worker_group,
/// anything that joins). A destructor can't be handed `unlocked`, so a
/// joining object destroyed inside a lock body would be a blocking wait
/// under a lock that no signature shows. guarded<T> therefore refuses a T
/// that owns one (kernel/guarded.hpp), and owners keep them outside locks.
/// Declared next to each such type:
///
///     template <> inline constexpr bool jaal::joins_on_destroy<my_pool> = true;
template <class T>
inline constexpr bool joins_on_destroy = false;

// A capability describes ONE thread at ONE moment. It must never reach
// another thread or outlive its frame, so it is never Sendable: no task
// argument, job argument or lock-body argument can be one.
template <class Top> inline constexpr bool sendable_opt_out<held<Top>> = true;
template <class R>   inline constexpr bool sendable_opt_out<below<R>> = true;
template <>          inline constexpr bool sendable_opt_out<terminal_cap> = true;

}  // namespace jaal
