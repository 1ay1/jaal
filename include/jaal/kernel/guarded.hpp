#pragma once
// jaal::guarded<T> — when there has to be a lock, make the unsafe forms
// impossible to write.
//
//   jaal::guarded<std::map<std::string, int>> cache;
//   cache.with([](auto& m, std::string k) { ++m[k]; }, key);         // exclusive
//   int n = cache.read([](const auto& m) { return int(m.size()); });   // shared
//
// Rules, each enforced by the compiler:
//
//   * The data is ONLY reachable while the lock is held. There's no get(),
//     no raw mutex, nothing to forget to lock.
//
//   * Nothing escapes the lock. The function's result must be Sendable,
//     which rules out references, pointers and views into T. You get a copy
//     out, never a handle in.
//
//   * NO LOCK CAN BE TAKEN WHILE ANOTHER IS HELD. Every lock-order deadlock
//     starts with a thread holding lock A while taking lock B. Inside
//     with()/read(), code can only reach what it was GIVEN: the function is
//     captureless, and its extra arguments must be Sendable. A guarded<U>
//     (it owns a mutex), a pointer or reference to one, or a Sink with a
//     blocking mailbox behind it... none of those can be passed in:
//       - a capture is rejected (the function must be captureless)
//       - guarded<U> is not Sendable, and neither is a pointer, reference
//         or reference_wrapper to anything
//     So the second lock has no name inside the first. The deadlock can't
//     be written, rather than being caught at runtime.
//
//     The one way round it is a global or a static (a captureless function
//     can still name those). The ban-list (tests/lint) rejects mutable
//     statics and globals outside jaal's own files, which is how the same
//     hole is closed for task bodies.
//
// Prefer not to need this. The first answer to shared state is an owner
// you send messages to; guarded<T> is for the few places a lock really is
// simpler (a cache many workers read).

#include <concepts>
#include <mutex>
#include <shared_mutex>
#include <type_traits>
#include <utility>

#include "../core/sendable.hpp"

namespace jaal {

namespace detail::guard {

// Void results are fine; anything else must be safe to hand out.
template <class R>
concept escapable = std::is_void_v<R> || Sendable<std::remove_cv_t<R>>;

// No captures: an empty, default-constructible callable is a lambda with an
// empty capture list (or a stateless function object). That's what makes
// "the function can only reach its arguments" true.
template <class F>
concept captureless = std::is_empty_v<std::remove_cvref_t<F>>
                   && std::default_initializable<std::remove_cvref_t<F>>;

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

    /// Exclusive access: f(T&, args...). f is captureless; args are moved
    /// in and must be Sendable; the result must not point into T.
    template <class F, class... Args>
    auto with(F f, Args... args) -> std::invoke_result_t<F&, T&, Args&&...> {
        check<F, T&, Args...>();
        std::unique_lock lk(m_);
        return f(value_, std::move(args)...);
    }

    /// Shared access: f(const T&, args...). Many readers at once, no
    /// writer. Same rules as with().
    template <class F, class... Args>
    auto read(F f, Args... args) const -> std::invoke_result_t<F&, const T&, Args&&...> {
        check<F, const T&, Args...>();
        std::shared_lock lk(m_);
        return f(std::as_const(value_), std::move(args)...);
    }

private:
    // One place, one message per rule, in the order a reader should fix
    // them. static_assert inside the body (rather than a requires-clause)
    // so the error is the sentence, not "no matching function".
    template <class F, class Ref, class... Args>
    static consteval void check() {
        static_assert(detail::guard::captureless<F>,
                      "jaal: guarded<T>::with/read takes a CAPTURELESS function; pass what it "
                      "needs as arguments after it. (A capture could name a second lock, and "
                      "holding two locks is how deadlocks start.)");
        static_assert((Sendable<Args> && ...),
                      "jaal: an argument to guarded<T>::with/read is not Sendable. A pointer, "
                      "reference or guarded<U> could reach a second lock while this one is "
                      "held; pass owned values.");
        if constexpr (detail::guard::captureless<F> && (Sendable<Args> && ...)) {
            static_assert(std::invocable<F&, Ref, Args&&...>,
                          "jaal: guarded<T>::with/read: the function can't be called as "
                          "f(T&, args...) (read: f(const T&, args...))");
            if constexpr (std::invocable<F&, Ref, Args&&...>)
                static_assert(detail::guard::escapable<std::invoke_result_t<F&, Ref, Args&&...>>,
                              "jaal: guarded<T>::with/read returns something that points into "
                              "the locked data (a reference, pointer or view); return a copy "
                              "instead");
        }
    }

    mutable std::shared_mutex m_;
    T                         value_{};
};

// A guarded<T> owns a lock; it never crosses into another lock's body.
template <class T> inline constexpr bool sendable_opt_out<guarded<T>> = true;

}  // namespace jaal
