#pragma once
// jaal::snapshot<T> — a value read far more often than it changes, with no
// lock anywhere.
//
// Many "locks" in a program are not protecting a critical section at all:
// they protect a value that a writer replaces now and then and every other
// thread reads (the active settings, a catalog of facts, the UI prefs a
// view projects). A guarded<T> for those works, but it puts a lock -- and so
// a rank, and a capability parameter -- on every reader, including code that
// runs every frame and code that must never be handed a capability (a pure
// view). A snapshot has none of that:
//
//   jaal::snapshot<Prefs> prefs;                         // starts as Prefs{}
//   Prefs p = *prefs.load();                             // reader: one atomic load
//   prefs.store(next);                                   // writer: replace
//   prefs.update([](const Prefs& old, Prefs n) {         // read-modify-write:
//       Prefs out = old; out.x = n.x; return out;        //   compare-and-swap loop,
//   }, n);                                               //   retried on a race
//
// Readers get a jaal::shared<T>: an immutable, reference-counted value that
// stays alive as long as they hold it, even across later stores. Nothing is
// ever mutated in place, so a reader can't see a half-written value and a
// writer can't be blocked by a reader.
//
// Rules, enforced by the type:
//   * T is Shareable (Sendable and Frozen): a snapshot is shared between
//     threads, so nothing in it may be borrowed or mutable through const.
//   * update()'s function is captureless, its arguments Sendable, and it
//     builds a NEW value from the old one -- it may run more than once if
//     another writer gets in first, so it must not have side effects. It
//     can't take a lock: it is never handed a capability.
//
// No lock, so no rank, no capability, and no deadlock: a snapshot can be
// read from inside any lock body, from a view, from a signal-free hot path.

#include <atomic>
#include <concepts>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>

#include "../core/sendable.hpp"
#include "../core/shared.hpp"
#include "../core/sync.hpp"
#include "published.hpp"

namespace jaal {

template <class T>
    requires Shareable<T> && std::default_initializable<T>
class snapshot {
public:
    snapshot() { slot_.publish(make(T{})); }

    template <class U>
        requires(std::constructible_from<T, U> && !std::same_as<std::remove_cvref_t<U>, snapshot>)
    explicit snapshot(U&& init) { slot_.publish(make(T(std::forward<U>(init)))); }

    snapshot(const snapshot&)            = delete;
    snapshot& operator=(const snapshot&) = delete;

    /// The current value. Never null; stays valid while held.
    [[nodiscard]] shared<T> load() const noexcept { return *cur(); }

    /// A copy of the current value, for callers that want a T.
    [[nodiscard]] T get() const { return *load(); }

    /// Replace the value.
    void store(T next) { (void)slot_.replace(box(std::move(next))); }

    /// Read-modify-write without a lock: f(const T& old, args...) -> T is
    /// retried until no other writer intervened. Returns the value stored.
    template <class F, class... Args>
    shared<T> update(F f, Args... args) {
        static_assert(std::is_empty_v<F> && std::default_initializable<F>,
                      "jaal::snapshot::update takes a CAPTURELESS function; it may run "
                      "more than once, so pass inputs as arguments after it");
        static_assert((Sendable<Args> && ...),
                      "jaal::snapshot::update: an argument is not Sendable");
        static_assert(std::is_same_v<std::remove_cvref_t<std::invoke_result_t<F&, const T&, const Args&...>>, T>,
                      "jaal::snapshot::update: the function is T(const T& old, const Args&...)");
        for (;;) {
            const std::shared_ptr<cell> old = cur();
            const T& before = old->get();
            std::shared_ptr<cell> next = box(f(before, std::as_const(args)...));
            if (slot_.publish_if(old, next)) return *next;
        }
    }

private:
    using cell = shared<T>;   // the published object is the shared handle itself

    static std::shared_ptr<cell> box(T v) {
        return std::make_shared<cell>(cell::make(std::move(v)));
    }
    static std::shared_ptr<cell> make(T v) { return box(std::move(v)); }

    std::shared_ptr<cell> cur() const noexcept { return slot_.current(); }

    kernel::published<cell> slot_;
};

// A process-wide slot, not a value to move around; every access is atomic.
template <class T> inline constexpr bool sendable_opt_out<snapshot<T>> = true;
template <class T> inline constexpr bool sync_opt_in<snapshot<T>>      = true;

}  // namespace jaal
