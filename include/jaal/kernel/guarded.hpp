#pragma once
// jaal::guarded<T, R> — shared state that is only reachable under its lock,
// in an order the compiler checks.
//
//   using cache_rank = jaal::rank<40, "app.cache">;
//   jaal::guarded<std::map<std::string, int>, cache_rank> cache;
//
//   void bump(const jaal::below<cache_rank>& cap, std::string k) {
//       cache.with(cap, [](const auto& held, auto& m, std::string k) { ++m[k]; }, std::move(k));
//   }
//
// Rules, each enforced by the compiler:
//
//   * The data is ONLY reachable while the lock is held. There is no get(),
//     no raw mutex, nothing to forget to lock.
//
//   * Nothing escapes the lock. The body's result must be Sendable, which
//     rules out references, pointers and views into T.
//
//   * LOCKS ARE TAKEN IN RANK ORDER, OR NOT AT ALL. Taking the lock needs a
//     capability proving the thread holds only lower-ranked locks
//     (core/capability.hpp): `unlocked`, a `held<Q>` with Q < R, or a
//     `below<S>` with S <= R. The body receives `held<R>`, which is the ONLY
//     capability it can reach: it is captureless, and its arguments must be
//     Sendable, which capabilities never are. So a body can take a further
//     lock only if that lock ranks above R, and a leaf's body can take none.
//
//   * NO BLOCKING WAIT UNDER A LOCK. wait_with needs `unlocked`.
//
// A body may take its `held<R>` as a leading parameter or ignore it:
//
//   g.with(cap, [](T& t) { ... });                              // takes nothing else
//   g.with(cap, [](const jaal::held<R>& h, T& t) { ... });      // may take ranks > R
//
// Prefer not to need this. The first answer to shared state is an owner you
// send messages to, and a value read far more often than it changes is a
// published<T> (no lock, no rank). guarded is for the few places a lock
// really is simpler.

#include <chrono>
#include <concepts>
#include <cstddef>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <type_traits>
#include <utility>

#include "../core/capability.hpp"
#include "../core/join_free.hpp"
#include "../core/sendable.hpp"
#include "../core/sync.hpp"

namespace jaal {

namespace detail::guard {

// Void results are fine; anything else must be safe to hand out.
template <class R>
concept escapable = std::is_void_v<R> || Sendable<std::remove_cv_t<R>>;

// No captures: an empty, default-constructible callable is a lambda with an
// empty capture list (or a stateless function object). That is what makes
// "the body can only reach its arguments" true.
template <class F>
concept captureless = std::is_empty_v<std::remove_cvref_t<F>>
                   && std::default_initializable<std::remove_cvref_t<F>>;

// A body either takes the lock's capability first, or doesn't.
template <class F, class Cap, class Ref, class... Args>
concept takes_cap = std::invocable<F&, const Cap&, Ref, Args&&...>;
template <class F, class Cap, class Ref, class... Args>
concept plain_body = !takes_cap<F, Cap, Ref, Args...> && std::invocable<F&, Ref, Args&&...>;

template <class F, class Cap, class Ref, class... Args>
struct body_result;
template <class F, class Cap, class Ref, class... Args>
    requires takes_cap<F, Cap, Ref, Args...>
struct body_result<F, Cap, Ref, Args...> {
    using type = std::invoke_result_t<F&, const Cap&, Ref, Args&&...>;
};
template <class F, class Cap, class Ref, class... Args>
    requires plain_body<F, Cap, Ref, Args...>
struct body_result<F, Cap, Ref, Args...> {
    using type = std::invoke_result_t<F&, Ref, Args&&...>;
};
template <class F, class Cap, class Ref, class... Args>
using body_result_t = typename body_result<F, Cap, Ref, Args...>::type;

template <class F, class Cap, class Ref, class... Args>
decltype(auto) call(F& f, Ref ref, Args&... args) {
    if constexpr (takes_cap<F, Cap, Ref, Args...>) {
        const Cap h = cap_access::mint<typename Cap::top>();
        return f(h, static_cast<Ref>(ref), std::move(args)...);
    } else {
        return f(static_cast<Ref>(ref), std::move(args)...);
    }
}

}  // namespace detail::guard

template <class T, class R = leaf>
    requires Rank<R>
class guarded {
    // Destroying T inside a body (`v = {}`, an erase, a reset) must never
    // wait for another thread: that wait would be under this lock, and a
    // destructor can't be asked for `unlocked`.
    static_assert(JoinFree<T>,
                  "jaal: guarded<T> can't own a pool, worker_group or any type whose "
                  "destructor joins threads (jaal::joins_on_destroy): replacing or "
                  "dropping it inside a lock body would wait under the lock. Keep it "
                  "outside, and stop() it with `unlocked` first.");

public:
    using rank       = R;
    using value_type = T;
    using held_cap   = held<R>;
    using entry_cap  = below<R>;

    guarded() { warm(); }

    template <class... Args>
        requires std::is_constructible_v<T, Args...>
    explicit guarded(std::in_place_t, Args&&... args) : value_(std::forward<Args>(args)...) {
        warm();
    }

    template <class U>
        requires(std::is_constructible_v<T, U> && !std::same_as<std::remove_cvref_t<U>, std::in_place_t>
                 && !std::same_as<std::remove_cvref_t<U>, guarded>)
    explicit guarded(U&& init) : value_(std::forward<U>(init)) { warm(); }

    guarded(const guarded&)            = delete;
    guarded& operator=(const guarded&) = delete;
    guarded(guarded&&)                 = delete;
    guarded& operator=(guarded&&)      = delete;

    /// Exclusive access: body([held<R>,] T&, args...). The caller proves it
    /// holds only lower-ranked locks; the body is captureless, its arguments
    /// are moved in and Sendable, and its result does not point into T.
    template <class F, class... Args>
    auto with(const below<R>&, F f, Args... args)
        -> detail::guard::body_result_t<F, held_cap, T&, Args...> {
        check<F, T&, Args...>();
        std::unique_lock lk(m_);
        Notify on_exit{*this};   // a writer may have made someone's predicate true
        return detail::guard::call<F, held_cap, T&>(f, value_, args...);
    }

    /// Shared access: body([held<R>,] const T&, args...). Many readers at
    /// once. Same rules as with().
    template <class F, class... Args>
    auto read(const below<R>&, F f, Args... args) const
        -> detail::guard::body_result_t<F, held_cap, const T&, Args...> {
        check<F, const T&, Args...>();
        std::shared_lock lk(m_);
        return detail::guard::call<F, held_cap, const T&>(f, std::as_const(value_), args...);
    }

    /// Block until pred(const T&, const args&...) holds, then run
    /// body([held<R>,] T&, args...) under the same lock. A wait can only be
    /// for ANOTHER thread, and that thread may need any lock this one holds,
    /// so it needs `unlocked`: a wait with any lock held does not compile.
    ///
    ///     auto c = st.wait_with(cap,
    ///         [](const S& s, Id id) { return s.done.contains(id); },
    ///         [](S& s, Id id) { return take(s.done, id); }, my_id);
    template <class P, class F, class... Args>
    auto wait_with(const unlocked&, P pred, F f, Args... args)
        -> detail::guard::body_result_t<F, held_cap, T&, Args...> {
        check<F, T&, Args...>();
        check_pred<P, Args...>();
        std::unique_lock lk(m_);
        ++waiters_;
        cv_.wait(lk, [&] { return pred(std::as_const(value_), std::as_const(args)...); });
        --waiters_;
        Notify on_exit{*this};
        return detail::guard::call<F, held_cap, T&>(f, value_, args...);
    }

    /// wait_with, bounded: wait up to `d` for pred, then run the body under
    /// the same lock either way. Returns its result and whether pred held.
    template <class Rep, class Period, class P, class F, class... Args>
    auto wait_with_for(const unlocked&, std::chrono::duration<Rep, Period> d, P pred, F f,
                       Args... args)
        -> std::pair<detail::guard::body_result_t<F, held_cap, T&, Args...>, bool> {
        check<F, T&, Args...>();
        check_pred<P, Args...>();
        std::unique_lock lk(m_);
        ++waiters_;
        const bool ok = cv_.wait_for(lk, d, [&] {
            return pred(std::as_const(value_), std::as_const(args)...);
        });
        --waiters_;
        Notify on_exit{*this};
        return {detail::guard::call<F, held_cap, T&>(f, value_, args...), ok};
    }

    /// Non-blocking forms: run the body only if the lock is free right now,
    /// else return nullopt. For a reader that must never stall (a status
    /// line drawn every frame). The body must return a value.
    template <class F, class... Args>
    auto try_with(const below<R>&, F f, Args... args)
        -> std::optional<detail::guard::body_result_t<F, held_cap, T&, Args...>> {
        check<F, T&, Args...>();
        std::unique_lock lk(m_, std::try_to_lock);
        if (!lk.owns_lock()) return std::nullopt;
        Notify on_exit{*this};
        return detail::guard::call<F, held_cap, T&>(f, value_, args...);
    }

    template <class F, class... Args>
    auto try_read(const below<R>&, F f, Args... args) const
        -> std::optional<detail::guard::body_result_t<F, held_cap, const T&, Args...>> {
        check<F, const T&, Args...>();
        std::shared_lock lk(m_, std::try_to_lock);
        if (!lk.owns_lock()) return std::nullopt;
        return detail::guard::call<F, held_cap, const T&>(f, std::as_const(value_), args...);
    }

private:
    // One place, one message per rule, in the order a reader should fix
    // them. static_assert in the body (not a requires-clause) so the error
    // is the sentence, not "no matching function".
    template <class F, class Ref, class... Args>
    static consteval void check() {
        static_assert(detail::guard::captureless<F>,
                      "jaal: guarded::with/read takes a CAPTURELESS body; pass what it needs "
                      "as arguments after it. (A capture could reach a lock or a capability "
                      "the rank check can't see.)");
        static_assert((Sendable<Args> && ...),
                      "jaal: an argument to guarded::with/read is not Sendable. A pointer, "
                      "reference, guarded or capability could reach a second lock out of "
                      "order; pass owned values.");
        if constexpr (detail::guard::captureless<F> && (Sendable<Args> && ...)) {
            constexpr bool callable = detail::guard::takes_cap<F, held_cap, Ref, Args...>
                                   || detail::guard::plain_body<F, held_cap, Ref, Args...>;
            static_assert(callable,
                          "jaal: guarded::with/read: the body can't be called as "
                          "body(const jaal::held<R>&, T&, args...) or body(T&, args...) "
                          "(read: const T&)");
            if constexpr (callable)
                static_assert(detail::guard::escapable<
                                  detail::guard::body_result_t<F, held_cap, Ref, Args...>>,
                              "jaal: guarded::with/read returns something that points into "
                              "the locked data (a reference, pointer or view); return a copy");
        }
    }

    template <class P, class... Args>
    static consteval void check_pred() {
        static_assert(detail::guard::captureless<P>,
                      "jaal: guarded::wait_with takes a CAPTURELESS predicate");
        static_assert(std::is_same_v<std::invoke_result_t<P&, const T&, const Args&...>, bool>,
                      "jaal: guarded::wait_with: the predicate is bool(const T&, const Args&...)");
    }

    // Take the lock once, on the constructing thread, before anyone else can
    // see this object.
    //
    // winpthreads (MinGW/MSYS2) implements PTHREAD_RWLOCK_INITIALIZER, which
    // libstdc++'s shared_mutex uses, as a sentinel that the FIRST lock call
    // replaces with a real rwlock -- and that replacement is not thread-safe.
    // Two threads taking a brand-new shared_mutex at the same instant can
    // make one of the calls fail, which libstdc++ turns into an assertion
    // abort (or, without assertions, a lock that silently wasn't taken: a
    // data race on value_). A fault box made on the loop thread and first
    // locked by a failing pool worker and the loop at once hit exactly this.
    // Initialising here, single-threaded, removes the window on every
    // platform at the cost of one uncontended lock pair per guarded.
    void warm() noexcept {
        m_.lock();
        m_.unlock();
    }

    // Wakes wait_with() callers after any exclusive access, and only when
    // there are any: waiters_ is read under the lock, so a guarded nobody
    // waits on never pays for a notify. Waiters re-check under the lock.
    struct Notify {
        guarded& g;
        ~Notify() { if (g.waiters_ != 0) g.cv_.notify_all(); }
    };

    mutable std::shared_mutex   m_;
    std::condition_variable_any cv_;
    std::size_t                 waiters_ = 0;   // under m_
    T                           value_{};
};

// A guarded owns a mutex and is a process-wide slot: it never moves between
// threads, and nothing can be passed into a lock body that reaches one.
template <class T, class R> inline constexpr bool sendable_opt_out<guarded<T, R>> = true;
// Every access to the value goes through the lock.
template <class T, class R> inline constexpr bool sync_opt_in<guarded<T, R>> = true;

}  // namespace jaal
