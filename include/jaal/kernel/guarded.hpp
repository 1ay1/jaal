#pragma once
// jaal::guarded<T> — when there has to be a lock, make the unsafe forms
// impossible to write.
//
//   jaal::guarded<std::map<std::string, int>> cache;
//   cache.with([](auto& m) { m["k"] = 1; });                  // exclusive
//   int n = cache.read([](const auto& m) { return m.size(); });   // shared
//
// Rules, each enforced:
//   * The data is ONLY reachable while the lock is held. There's no get(),
//     no raw mutex, nothing to forget to lock.
//   * Nothing escapes the lock. The lambda's result must be Sendable, which
//     rules out references, pointers and views into T. You get a copy out,
//     never a handle in.
//   * No nested locks on one thread. Lock-order deadlocks start with one
//     thread holding lock A while taking lock B. Debug builds track "this
//     thread holds a guarded lock" and throw nested_guard_error if a second
//     with()/read() starts inside the first. (Release builds skip the
//     check; it's a debugging aid, not a guarantee.)
//
// Prefer not to need this. The first answer to shared state is an owner
// you send messages to; guarded<T> is for the few places a lock really is
// simpler (a cache many workers read).

#include <concepts>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "../core/sendable.hpp"

namespace jaal {

/// Thrown (debug builds) when a guarded lock is taken while another is held
/// on the same thread.
struct nested_guard_error : std::logic_error {
    nested_guard_error()
        : std::logic_error("jaal: nested guarded<T> locks on one thread "
                           "(lock-order deadlock risk)") {}
};

namespace detail::guard {

// Void results are fine; anything else must be safe to hand out.
template <class R>
concept escapable = std::is_void_v<R> || Sendable<std::remove_cv_t<R>>;

#ifndef NDEBUG
inline thread_local int held = 0;       // jaal's own allowlisted thread_local

struct hold {
    hold() {
        if (held != 0) throw nested_guard_error{};
        ++held;
    }
    ~hold() { --held; }
    hold(const hold&)            = delete;
    hold& operator=(const hold&) = delete;
};
#else
struct hold {};
#endif

}  // namespace detail::guard

template <class T>
class guarded {
public:
    guarded() = default;

    template <class... Args>
        requires std::is_constructible_v<T, Args...>
    explicit guarded(Args&&... args) : value_(std::forward<Args>(args)...) {}

    guarded(const guarded&)            = delete;
    guarded& operator=(const guarded&) = delete;
    guarded(guarded&&)                 = delete;
    guarded& operator=(guarded&&)      = delete;

    /// Exclusive access. f's result must not point into T.
    template <std::invocable<T&> F>
        requires detail::guard::escapable<std::invoke_result_t<F, T&>>
    auto with(F&& f) -> std::invoke_result_t<F, T&> {
        [[maybe_unused]] detail::guard::hold h;          // before the lock: throws with nothing held
        std::unique_lock lk(m_);
        return std::invoke(std::forward<F>(f), value_);
    }

    /// Shared access: many readers at once, no writer. f's result must not
    /// point into T.
    template <std::invocable<const T&> F>
        requires detail::guard::escapable<std::invoke_result_t<F, const T&>>
    auto read(F&& f) const -> std::invoke_result_t<F, const T&> {
        [[maybe_unused]] detail::guard::hold h;
        std::shared_lock lk(m_);
        return std::invoke(std::forward<F>(f), std::as_const(value_));
    }

    // Same calls with a result that would point into the locked data:
    // explain instead of "no matching function".
    template <std::invocable<T&> F>
        requires (!detail::guard::escapable<std::invoke_result_t<F, T&>>)
    void with(F&&) {
        static_assert(detail::guard::escapable<std::invoke_result_t<F, T&>>,
                      "jaal: guarded<T>::with's lambda returns something that points "
                      "into the locked data (a reference, pointer or view); return a "
                      "copy instead");
    }
    template <std::invocable<const T&> F>
        requires (!detail::guard::escapable<std::invoke_result_t<F, const T&>>)
    void read(F&&) const {
        static_assert(detail::guard::escapable<std::invoke_result_t<F, const T&>>,
                      "jaal: guarded<T>::read's lambda returns something that points "
                      "into the locked data (a reference, pointer or view); return a "
                      "copy instead");
    }

private:
    mutable std::shared_mutex m_;
    T                         value_{};
};

}  // namespace jaal
