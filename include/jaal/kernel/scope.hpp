#pragma once
// jaal::scope: structured concurrency for helper threads.
//
// Some blocking jobs need helpers: drain stdout and stderr at once, run a
// watchdog beside a request, split a scan across cores. scope runs a block,
// lets it start helpers, and joins every one before it returns:
//
//   const auto [lines, words] = jaal::scope(cap, [&](jaal::nursery& n) {
//       auto a = n.spawn([](std::string t) { return std::ranges::count(t, '\n'); }, text);
//       auto b = n.spawn([](std::string t) { return count_words(t); }, text);
//       return std::pair{a.join(), b.join()};
//   });
//
// A helper follows the task rule (core/fx.hpp, D45): the body is
// CAPTURELESS and everything it uses is passed after it, by value, and
// must be Sendable. State the helpers and the owner share goes in a
// co_owned<T>, and T must be Sync. Each rule is a compile error:
//
//   * no [&], no `this`, no captured pointer: a helper can't reach a local,
//     so it can't outlive one (the hole a capture-ordering rule left open:
//     a pointer stored into a captured slot after the handle was made)
//   * a Sendable argument holds no pointer, reference or view into memory
//     another thread frees
//   * a Sync co_owned<T> is reference counted (nobody frees it under a
//     helper) and every field is const, atomic or guarded (no data race)
//   * nursery, handle<T> and the capabilities are not Sendable, so a helper
//     can't be handed one: it can't spawn, join a sibling, or wait on the
//     owner. The only waits are the owner waiting on its own helpers, so the
//     wait graph is a tree and can't deadlock.
//
// The BLOCK may still capture [&]: it runs on the owner's thread, inside
// scope(), and is not a helper. For a data-parallel loop over borrowed data,
// parallel_for(cap, n, width, body) borrows `body` for one call and joins
// every helper before it returns, so that borrow can't outlive it either.
//
// Lifetime:
//   * scope() joins EVERY helper before it returns: normal return, early
//     return, or exception. A handle<T> also joins its helper when it is
//     destroyed, so `(void)n.spawn(...)` waits at the end of that statement;
//     spawn_n starts helpers that run side by side.
//   * nursery and handle<T> can't be copied or moved, and there's no detach.
//   * cancellation flows down: the parent's stop_token stops the nursery,
//     and a helper may take a std::stop_token.
//   * if a helper throws, its siblings are asked to stop, all are joined,
//     and the FIRST exception is rethrown from scope() (or from join(), if
//     the block joins that helper itself).
//
// The one thing no type can see: a captureless helper can still touch a
// global or a static. jaal's banlist lint flags mutable statics.

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "../core/capability.hpp"
#include "../core/co_owned.hpp"
#include "../core/fx.hpp"
#include "../core/sendable.hpp"
#include "waits.hpp"

namespace jaal {

class nursery;

template <class F>
    requires std::invocable<const F&, std::size_t>
void parallel_for(const unlocked& cap, std::size_t n, std::size_t width, const F& body);

namespace detail::scope {

// One helper's result: a value or an exception, written by the helper thread
// and read by join() after the thread is joined (the join is the
// synchronisation, so the slot needs no atomics).
template <class T>
struct slot {
    std::optional<T>   value;
    std::exception_ptr error;
};
template <>
struct slot<void> {
    bool               done = false;
    std::exception_ptr error;
};

// One helper as the nursery owns it. Lives in a std::list: stable address.
struct entry {
    std::jthread                               thread;
    std::unique_ptr<void, void (*)(void*)>     slot{nullptr, [](void*) {}};
    std::atomic<bool>                          observed{false};
    std::exception_ptr (*take_error)(void*)    = nullptr;
};

// Wait for a helper from a destructor. If the frame is unwinding, ask the
// nursery to stop first: a helper waiting for stop would otherwise hang it.
inline void join_on_destroy(entry& e, std::stop_source& stop, int unwinding_at_start) noexcept {
    if (!e.thread.joinable()) return;
    if (std::uncaught_exceptions() > unwinding_at_start) stop.request_stop();
    e.thread.join();
}

// The call shapes a helper body may have, most specific first. `Lead` is
// what spawn_n passes before the arguments (the index), or nothing.
enum class shape : std::uint8_t { none, cap_stop, cap, stop, plain };

template <class B, class... Args>
consteval shape shape_of() {
    if constexpr (std::is_invocable_v<B&, const unlocked&, std::stop_token, Args...>)
        return shape::cap_stop;
    else if constexpr (std::is_invocable_v<B&, const unlocked&, Args...>)
        return shape::cap;
    else if constexpr (std::is_invocable_v<B&, std::stop_token, Args...>)
        return shape::stop;
    else if constexpr (std::is_invocable_v<B&, Args...>)
        return shape::plain;
    else
        return shape::none;
}

template <class B, class... Args>
concept callable_helper = shape_of<B, Args...>() != shape::none;

// Call a captureless body. An empty lambda is rebuilt from its type; a
// function pointer is carried as data.
template <class B>
decltype(auto) body_of(const B& carried) {
    if constexpr (detail_task::fn_ptr<B>) return carried;
    else                                  return B{};
}

template <class B, class... Args>
decltype(auto) call(const B& carried, std::stop_token st, Args&&... args) {
    auto&& f = body_of(carried);
    constexpr shape s = shape_of<B, Args...>();
    if constexpr (s == shape::cap_stop) {
        const unlocked u = cap_access::fresh_thread();
        return f(u, std::move(st), std::forward<Args>(args)...);
    } else if constexpr (s == shape::cap) {
        const unlocked u = cap_access::fresh_thread();
        return f(u, std::forward<Args>(args)...);
    } else if constexpr (s == shape::stop) {
        return f(std::move(st), std::forward<Args>(args)...);
    } else {
        return f(std::forward<Args>(args)...);
    }
}

template <class B, class... Args>
using result_t = std::remove_cvref_t<decltype(call<B>(std::declval<const B&>(),
                                                      std::stop_token{},
                                                      std::declval<Args>()...))>;

// parallel_for's borrow of the caller's body. Sendable by opt-in, and only
// jaal::parallel_for can make one (private constructor): it joins every
// helper before it returns, so the borrow can't outlive the body. User code
// can't build a loan, so it can't use this to smuggle a pointer.
template <class T>
class loan {
public:
    const T* operator->() const noexcept { return &v_; }
    std::size_t n;
private:
    template <class F>
        requires std::invocable<const F&, std::size_t>
    friend void ::jaal::parallel_for(const unlocked&, std::size_t, std::size_t, const F&);
    loan(T v, std::size_t count) noexcept : n(count), v_(v) {}
    T v_;
};

// What spawn() returns when check() rejected the helper: nothing runs.
struct rejected {};

// The rules, one message each, in the order a reader should fix them.
// Plain static_asserts (not a requires-clause) so the error is the sentence.
template <class B, class... Args>
consteval void check() {
    static_assert(detail_task::captureless<B>,
                  "jaal: a scope helper must not capture anything; pass what it needs as "
                  "arguments after the body (state shared with the owner goes in a "
                  "jaal::co_owned<T>)");
    static_assert((Sendable<Args> && ...),
                  "jaal: a scope helper argument is not Sendable; pass owned values, or a "
                  "jaal::co_owned<T> for state the owner shares");
    if constexpr (detail_task::captureless<B> && (Sendable<Args> && ...))
        static_assert(callable_helper<B, Args...>,
                      "jaal: a scope helper must be callable as "
                      "([const jaal::unlocked&,] [std::stop_token,] args...) with the "
                      "arguments given after it");
}

}  // namespace detail::scope

/// A running helper. join() waits for it and returns its value (or rethrows
/// its exception). The destructor joins it if join() wasn't called. Can't be
/// copied, moved, or handed to another thread.
template <class T>
class [[nodiscard]] handle {
public:
    handle(const handle&)            = delete;
    handle& operator=(const handle&) = delete;
    handle(handle&&)                 = delete;
    handle& operator=(handle&&)      = delete;

    ~handle() {
        if (!taken_) detail::scope::join_on_destroy(*e_, *stop_, unwinding_);
    }

    /// Wait for the helper and take its result. At most once.
    T join() {
        if (taken_) throw std::logic_error("jaal: handle::join() called twice");
        taken_ = true;
        e_->observed.store(true, std::memory_order_relaxed);
        if (e_->thread.joinable()) e_->thread.join();
        auto* s = static_cast<detail::scope::slot<T>*>(e_->slot.get());
        if (s->error) std::rethrow_exception(std::exchange(s->error, nullptr));
        if constexpr (!std::is_void_v<T>) return std::move(*s->value);
    }

private:
    friend class nursery;
    handle(detail::scope::entry* e, std::stop_source* stop) : e_(e), stop_(stop) {}

    detail::scope::entry*  e_;
    std::stop_source*      stop_;
    int                    unwinding_ = std::uncaught_exceptions();
    bool                   taken_ = false;
};

/// Helpers started together by nursery::spawn_n. Same rule as handle: the
/// destructor joins them all.
class [[nodiscard]] helper_group {
public:
    helper_group(const helper_group&)            = delete;
    helper_group& operator=(const helper_group&) = delete;
    helper_group(helper_group&&)                 = delete;
    helper_group& operator=(helper_group&&)      = delete;

    ~helper_group() {
        if (!taken_)
            for (auto* e : entries_) detail::scope::join_on_destroy(*e, *stop_, unwinding_);
    }

    /// Wait for every helper; rethrow the first error. At most once.
    void join() {
        if (taken_) throw std::logic_error("jaal: helper_group::join() called twice");
        taken_ = true;
        for (auto* e : entries_) {
            e->observed.store(true, std::memory_order_relaxed);
            if (e->thread.joinable()) e->thread.join();
        }
        for (auto* e : entries_)
            if (auto err = e->take_error(e->slot.get())) std::rethrow_exception(err);
    }

private:
    friend class nursery;
    helper_group(std::vector<detail::scope::entry*> es, std::stop_source* stop)
        : entries_(std::move(es)), stop_(stop) {}

    std::vector<detail::scope::entry*>  entries_;
    std::stop_source*                   stop_;
    int                                 unwinding_ = std::uncaught_exceptions();
    bool                                taken_ = false;
};

/// Starts helpers that are all joined before the owning scope() returns.
/// Only scope() makes one, and it lives only inside the block.
class nursery {
public:
    nursery(const nursery&)            = delete;
    nursery& operator=(const nursery&) = delete;
    nursery(nursery&&)                 = delete;
    nursery& operator=(nursery&&)      = delete;

    /// Start a helper running body(args...). The body is captureless; it may
    /// take `const unlocked&` and/or a `std::stop_token` before the args.
    template <class Body, class... Args>
    [[nodiscard]] auto spawn(Body body, Args... args) {
        detail::scope::check<Body, Args...>();
        if constexpr (detail_task::captureless<Body> && (Sendable<Args> && ...)
                      && detail::scope::callable_helper<Body, Args...>) {
            using R = detail::scope::result_t<Body, Args...>;
            auto& e = start<R>(body, std::move(args)...);
            return handle<R>(&e, &stop_);
        } else {
            // check() already failed with the reason; return something a
            // caller can bind with `auto`, so that's the only error.
            return detail::scope::rejected{};
        }
    }

    /// Start `count` helpers running body(i, args...) for i in [0, count),
    /// side by side; like spawn, `const unlocked&` and a stop token go first
    /// if the body takes them: body([cap,] [stop,] i, args...). Each helper
    /// gets its own copy of the args. The group joins them when destroyed.
    template <class Body, class... Args>
    [[nodiscard]] helper_group spawn_n(std::size_t count, Body body, Args... args) {
        detail::scope::check<Body, std::size_t, Args...>();
        std::vector<detail::scope::entry*> es;
        if constexpr (detail_task::captureless<Body> && (Sendable<Args> && ...)
                      && detail::scope::callable_helper<Body, std::size_t, Args...>) {
            static_assert((std::copy_constructible<Args> && ...),
                          "jaal: spawn_n copies its arguments into every helper");
            es.reserve(count);
            try {
                for (std::size_t i = 0; i < count; ++i)
                    es.push_back(&start<void>(body, i, Args(args)...));
            } catch (...) {
                // Some started: join them here, the group never existed.
                stop_.request_stop();
                for (auto* e : es) if (e->thread.joinable()) e->thread.join();
                throw;
            }
        }
        return helper_group(std::move(es), &stop_);
    }

    /// The nursery's stop token (parent stop, a sibling failure, or the
    /// block throwing all trigger it). Pass it to a helper as an argument if
    /// it needs one besides its own.
    [[nodiscard]] std::stop_token stop_token() const noexcept { return stop_.get_token(); }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    template <class R, class Body, class... Args>
    detail::scope::entry& start(Body body, Args... args) {
        auto& e = add<R>();
        auto* s = static_cast<detail::scope::slot<R>*>(e.slot.get());
        // The thread owns everything it runs on: the body (empty or a
        // function pointer), its own args, the slot (owned by the entry,
        // which outlives the thread), and a copy of the stop token.
        e.thread = std::jthread(
            [s, body, stop = stop_, waiting = kernel::waits_detail::for_helper(),
             args = std::tuple<Args...>(std::move(args)...)](std::stop_token) mutable {
                // This helper is waited on by its owner (and whoever waits on
                // the owner): a mailbox those threads drain won't block it.
                kernel::waits_detail::waiters() = std::move(waiting);
                try {
                    std::apply([&](Args&... a) {
                        if constexpr (std::is_void_v<R>) {
                            detail::scope::call<Body>(body, stop.get_token(), std::move(a)...);
                            s->done = true;
                        } else {
                            s->value.emplace(detail::scope::call<Body>(
                                body, stop.get_token(), std::move(a)...));
                        }
                    }, args);
                } catch (...) {
                    s->error = std::current_exception();
                    stop.request_stop();               // siblings: please stop
                }
            });
        return e;
    }

    using entry = detail::scope::entry;

    template <class R>
    entry& add() {
        auto& e = entries_.emplace_back();             // std::list: stable addresses
        e.slot = std::unique_ptr<void, void (*)(void*)>(
            new detail::scope::slot<R>{},
            [](void* p) { delete static_cast<detail::scope::slot<R>*>(p); });
        e.take_error = [](void* p) {
            return std::exchange(static_cast<detail::scope::slot<R>*>(p)->error, nullptr);
        };
        return e;
    }

    template <class B>
    friend auto scope(const unlocked&, std::stop_token parent, B&& block);

    explicit nursery(std::stop_token parent)
        : on_parent_stop_(parent, [src = stop_] mutable { src.request_stop(); }) {}

    // Join every helper; return the first error nobody observed through
    // join(). Called exactly once, by scope().
    std::exception_ptr join_all() noexcept {
        for (auto& e : entries_)
            if (e.thread.joinable()) e.thread.join();
        for (auto& e : entries_) {
            if (e.observed.load(std::memory_order_relaxed)) continue;
            if (auto err = e.take_error(e.slot.get())) return err;
        }
        return nullptr;
    }

    std::stop_source                            stop_;
    std::list<entry>                            entries_;
    std::stop_callback<std::function<void()>>   on_parent_stop_;
};

// The nursery and its handles are this thread's: never handed to a helper,
// a task or a job. That is what keeps helpers from spawning or joining.
template <> inline constexpr bool sendable_opt_out<nursery>      = true;
template <class T> inline constexpr bool sendable_opt_out<handle<T>> = true;
template <> inline constexpr bool sendable_opt_out<helper_group> = true;
// Only parallel_for makes a loan, and it joins before the borrow ends.
template <class T> inline constexpr bool sendable_opt_in<detail::scope::loan<T>> = true;

/// Run `block(nursery&)`, then join every helper it spawned. Returns the
/// block's result. If the block or any helper threw, rethrows the first
/// exception AFTER everything is joined.
///
/// Joining waits on other threads, and a helper may need any lock the
/// caller holds, so scope() takes `unlocked`: opening a scope under a lock
/// does not compile.
template <class B>
auto scope(const unlocked&, std::stop_token parent, B&& block) {
    using R = std::invoke_result_t<B&, nursery&>;
    nursery n(std::move(parent));
    std::exception_ptr block_error;
    std::optional<std::conditional_t<std::is_void_v<R>, int, R>> result;
    try {
        if constexpr (std::is_void_v<R>) block(n);
        else                             result.emplace(block(n));
    } catch (...) {
        block_error = std::current_exception();
        n.stop_.request_stop();                    // the block failed: stop helpers
    }
    auto helper_error = n.join_all();              // ALWAYS, before leaving
    if (block_error)  std::rethrow_exception(block_error);
    if (helper_error) std::rethrow_exception(helper_error);
    if constexpr (!std::is_void_v<R>) return std::move(*result);
}

/// scope() with no parent to inherit cancellation from.
template <class B>
auto scope(const unlocked& cap, B&& block) {
    return scope(cap, std::stop_token{}, std::forward<B>(block));
}

/// Run body(i) for every i in [0, n) across `width` threads (the caller is
/// one of them) and return when all have finished. The first exception is
/// rethrown after every share has stopped.
///
/// The one place a helper may borrow: `body` is a reference, but the borrow
/// can't outlive it. parallel_for joins every helper before it returns, the
/// helpers can reach `body` only through this call's own frame, and the
/// frame is below the caller's, so `body` outlives every use. Nothing
/// escapes: no handle, no nursery, no way to keep a helper running.
/// `body` must be safe to call from several threads at once.
template <class F>
    requires std::invocable<const F&, std::size_t>
void parallel_for(const unlocked& cap, std::size_t n, std::size_t width, const F& body) {
    if (n == 0) return;
    width = std::min(std::max<std::size_t>(width, 1), n);
    if (width == 1) {
        for (std::size_t i = 0; i < n; ++i) body(i);
        return;
    }
    // Shares claim indices from one counter. The body is erased to a plain
    // function pointer over an opaque address so the helpers stay
    // captureless; the address is only used inside this call.
    struct work {
        std::atomic<std::size_t> next{0};
        std::atomic<bool>        failed{false};
    };
    struct borrowed {
        const void* body;
        void (*call)(const void*, std::size_t);
    };
    const borrowed b{&body, [](const void* p, std::size_t i) { (*static_cast<const F*>(p))(i); }};
    auto w = co_owned<work>::make();
    constexpr auto share = [](std::stop_token st, std::size_t, co_owned<work> w,
                              detail::scope::loan<borrowed> l) {
        while (!st.stop_requested() && !w->failed.load(std::memory_order_relaxed)) {
            const std::size_t i = w->next.fetch_add(1, std::memory_order_relaxed);
            if (i >= l.n) return;
            try {
                l->call(l->body, i);
            } catch (...) {
                w->failed.store(true, std::memory_order_relaxed);
                throw;
            }
        }
    };
    const detail::scope::loan<borrowed> lb{b, n};
    scope(cap, [&](nursery& nur) {
        auto rest = nur.spawn_n(width - 1, share, w, lb);
        share(nur.stop_token(), 0, w, lb);   // the caller runs a share too
        rest.join();
    });
}

}  // namespace jaal
