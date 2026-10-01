#pragma once
// jaal::kernel::loop_token / loop_bound — the one-thread rule as a type.
//
// maya's real safety net is that almost everything runs on one thread: 68
// thread_locals say so in comments, and nothing enforces it. If a worker
// ever called the renderer it would silently get its own empty caches —
// a wrong answer, which is worse than a crash.
//
// loop_bound<T> makes that a type. The data is only reachable with a
// loop_token, and a token:
//   * can be minted ONLY by the kernel, or by on_loop() after it has
//     CHECKED that this really is a loop thread (see below)
//   * can't be copied or moved, so it can't be stored or captured
//   * is never handed to a task body (which is captureless anyway)
//   * has no accessor that hands a usable token to worker code
//
// So code running on a worker has no way to name loop-bound state.
//
// ── Why there is an on_loop(), and why it is not the hole it looks like ──
//
// This header used to say a `loop_token::current()` must not exist, because
// a captureless lambda can call any function, so a global accessor would
// hand worker code a token. That reasoning is right about an UNCHECKED
// accessor and it is why there still isn't one.
//
// But it left loop_bound<T> unusable. A token could only come from the
// kernel, and the kernel never passed one to update() or view() — so in
// jaal, maya and agentty combined, the number of loop_bound<T> in
// production code was zero. A guarantee nothing can adopt protects nothing:
// all 68 of maya's thread_locals and all 25 of agentty's stayed raw.
//
// on_loop(f) closes that gap without opening the one the old comment
// feared. It mints a token only after asking "is the calling thread a
// kernel loop thread?", which the kernel answers by arming an identity
// (loop_identity, below) for exactly as long as it is folding. So:
//
//   * on the loop     — f runs with a token, and loop_bound<T> is reachable.
//   * on a worker     — there is no token to be had. The call does not
//                       quietly succeed against a different thread's copy
//                       of some thread_local; it ABORTS, loudly, naming the
//                       rule. The wrong answer the header worried about
//                       becomes the crash the header said it preferred.
//
// The token still cannot be copied, moved, or returned, so nothing escapes
// f. The grade drops from A ("cannot be written") to B ("cannot be wrong at
// runtime") for the off-loop call — in exchange for a guarantee that real
// code can actually use. That trade is the whole point: a Level A rule with
// no adopters is weaker in practice than a Level B rule with 93.
//
// thread_local here is jaal-internal and allowlisted (tests/lint/allowlist.txt):
// "am I on a loop thread" is per-thread by definition, and a shared atomic
// would be both slower and wrong (it cannot answer the question for the
// thread that is asking).

#include <concepts>
#include <cstdio>
#include <cstdlib>
#include <type_traits>
#include <utility>

#include "../core/sendable.hpp"   // sendable_opt_out, for the opt-out at the bottom

namespace jaal::kernel {

class loop_key;

namespace loop_detail {

/// The single place a loop_key is born. loop_key friends ONLY this, so
/// "who may mint proof" is one grep, not a convention.
struct minter;

/// Depth of armed loop-identity scopes on THIS thread. Non-zero means the
/// kernel is folding here, so loop-bound state belongs to us.
///
/// A depth rather than a bool because a kernel may be started from inside
/// another kernel's fold (a child program, a nested run in a test): the
/// inner arm must not disarm the outer one on the way out.
inline unsigned& armed_depth() noexcept {
    thread_local unsigned d = 0;
    return d;
}

[[nodiscard]] inline bool on_loop_thread() noexcept { return armed_depth() != 0; }

[[noreturn]] inline void off_loop_abort(const char* what) noexcept {
    // Deliberately not an exception. This is a programming error that has
    // already happened — the caller is a thread that must never have been
    // able to see this state — and an exception could be swallowed by a
    // catch(...) in the very worker that broke the rule.
    std::fprintf(stderr,
                 "jaal: %s called off the loop thread.\n"
                 "      loop_bound<T> is state only the kernel's loop thread may "
                 "touch; reaching it from a worker would return a different "
                 "thread's value.\n"
                 "      Send a message to the loop instead (Sink::send), or hold "
                 "the data in jaal::guarded<T> if it is genuinely shared.\n",
                 what);
    std::abort();
}

}  // namespace loop_detail

/// The kernel's key for minting tokens.
///
/// The constructor is PRIVATE. It used to be public-and-explicit, which
/// stopped `loop_token t{{}}` (the case the compile-fail test pinned) but
/// not `loop_token t{loop_key{}}` — two more characters, and any worker had
/// forged proof. `explicit` defeats brace elision, not naming.
class loop_key {
private:
    explicit loop_key() = default;
    friend struct loop_detail::minter;
};

namespace loop_detail {
struct minter {
    [[nodiscard]] static loop_key key() noexcept { return loop_key{}; }
};
}  // namespace loop_detail

/// Proof that the holder is running on the loop thread.
class loop_token {
public:
    explicit loop_token(loop_key) noexcept {}

    loop_token(const loop_token&)            = delete;
    loop_token(loop_token&&)                 = delete;
    loop_token& operator=(const loop_token&) = delete;
    loop_token& operator=(loop_token&&)      = delete;
};

/// Arms the calling thread as a loop thread for its lifetime. The kernel
/// holds one around its fold; nothing else should need to.
///
/// RAII so an exception escaping a fold still disarms: an aborted step that
/// left the identity armed would let the next user of this thread — a pool
/// worker, once the kernel is gone — pass the on_loop() check.
class loop_identity {
public:
    loop_identity() noexcept { ++loop_detail::armed_depth(); }
    ~loop_identity() { --loop_detail::armed_depth(); }

    loop_identity(const loop_identity&)            = delete;
    loop_identity(loop_identity&&)                 = delete;
    loop_identity& operator=(const loop_identity&) = delete;
    loop_identity& operator=(loop_identity&&)      = delete;

    /// Mint proof directly. For the kernel's own code, which is already
    /// inside the armed region and need not re-check.
    [[nodiscard]] static loop_token token() noexcept {
        return loop_token{loop_detail::minter::key()};
    }
};

/// Is the calling thread a kernel loop thread?
///
/// For code that wants to TAKE A DIFFERENT PATH off the loop rather than
/// die — a logger, a cache that falls back to recomputing. Code that simply
/// requires the loop should call on_loop() and let it abort.
[[nodiscard]] inline bool on_loop() noexcept { return loop_detail::on_loop_thread(); }

namespace loop_detail {

// A result that may safely outlive the token.
//
// This is NOT guarded<T>'s rule, and the difference is the point. guarded<T>
// hands data to whichever thread took the lock, so its result must be
// Sendable — the value is about to cross a thread boundary. loop_bound<T>
// only ever hands data to the loop thread, because being on the loop is the
// precondition for getting a token at all. Nothing crosses a thread here.
//
// So the hazard is not a race, it is a DANGLE: a reference or pointer into
// the state stays valid only until the next mutation, and these are caches
// that evict. Returning `shared_ptr<const T>` by value is the right answer
// and must keep compiling (it owns the pointee, so eviction is harmless);
// returning `T&` or `const T*` into the cache must not.
//
// Requiring Sendable here instead would reject that correct shared_ptr
// return — shared_ptr is deliberately not Sendable — and push callers back
// to raw pointers, i.e. make the dangling bug the only expressible shape.
template <class R>
concept escapable = std::is_void_v<R>
                 || (!std::is_reference_v<R> && !std::is_pointer_v<R>);

}  // namespace loop_detail

/// Run `f` with a loop_token, or abort if this is not a loop thread.
///
/// The token lives on on_loop's stack and is neither copyable nor movable,
/// so it cannot outlive the call. `f` must not return a reference or pointer
/// into loop-bound state either — return an owning value (a copy, or a
/// shared_ptr), which the static_assert below insists on.
template <class F>
decltype(auto) on_loop(F&& f) {
    static_assert(std::invocable<F&, const loop_token&>,
                  "jaal: on_loop(f) calls f(const loop_token&); take the token as a "
                  "parameter rather than capturing one (you can't — it is immovable).");
    if constexpr (std::invocable<F&, const loop_token&>) {
        static_assert(
            loop_detail::escapable<std::invoke_result_t<F&, const loop_token&>>,
            "jaal: on_loop/loop_bound<T>::with returns a reference or pointer into "
            "loop-bound state; return an owning value instead (a copy, or a "
            "shared_ptr<const T>). The handle would dangle the moment the state "
            "is mutated or evicted — these are usually caches.");
    }
    if (!loop_detail::on_loop_thread()) loop_detail::off_loop_abort("on_loop");
    const loop_token tok = loop_identity::token();
    return std::forward<F>(f)(tok);
}

/// State only the loop thread may touch.
template <class T>
class loop_bound {
public:
    loop_bound() = default;

    template <class... Args>
        requires std::is_constructible_v<T, Args...>
    explicit loop_bound(Args&&... args) : value_(std::forward<Args>(args)...) {}

    [[nodiscard]] T&       get(const loop_token&) noexcept       { return value_; }
    [[nodiscard]] const T& get(const loop_token&) const noexcept { return value_; }

    /// Run f with the value, on the loop.
    template <class F>
    decltype(auto) with(const loop_token& t, F&& f) {
        return std::forward<F>(f)(get(t));
    }

    /// The same, checking for itself that this is the loop thread. The
    /// shape consumer code wants: no token to thread through twenty call
    /// frames, and still no way to touch the value from a worker.
    template <class F>
    decltype(auto) with(F&& f) {
        return on_loop([&](const loop_token& t) -> decltype(auto) {
            return std::forward<F>(f)(get(t));
        });
    }
    template <class F>
    decltype(auto) with(F&& f) const {
        return on_loop([&](const loop_token& t) -> decltype(auto) {
            return std::forward<F>(f)(get(t));
        });
    }

private:
    T value_{};
};

}  // namespace jaal::kernel

namespace jaal {

// A loop_bound<T> is pinned to one thread; it must never be copied into a
// task body or sent through a Sink. Saying so here means the Sendable
// machinery rejects it at the boundary instead of at first wrong answer.
template <class T>
inline constexpr bool sendable_opt_out<kernel::loop_bound<T>> = true;

// A token is proof about THIS thread. It is already non-copyable and
// non-movable, so it cannot be captured — but spelling out the opt-out
// makes the diagnostic say why rather than just "not Sendable".
template <> inline constexpr bool sendable_opt_out<kernel::loop_token> = true;

}  // namespace jaal
