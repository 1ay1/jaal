#pragma once
// jaal::scope — structured concurrency for helper threads.
//
// Some blocking jobs need helpers: read stdout and stderr at once, run a
// watchdog beside a request. agentty does this with raw std::thread and
// [&] captures, which is only safe if every helper is joined before the
// function returns. scope makes that the only possible shape:
//
//   auto [out, err] = jaal::scope(st, [&](jaal::nursery& n) {
//       auto a = n.spawn([&] { return drain(proc.stdout_fd()); });
//       auto b = n.spawn([&] { return drain(proc.stderr_fd()); });
//       return std::pair{a.join(), b.join()};
//   });
//   // every helper has finished by this line, even if something threw
//
// Rules:
//   * scope() joins EVERY helper before it returns: normal return, early
//     return, or exception. So [&] captures of locals are safe: nothing
//     spawned can outlive the frame that owns what it borrowed. This is
//     the one place in jaal where helpers may capture by reference.
//   * nursery and handle<T> can't be copied or moved, and there's no
//     detach. A helper can't be handed off to outlive the scope.
//   * cancellation flows down: the parent's stop_token stops the nursery,
//     and a helper may take a std::stop_token as its first argument.
//   * if a helper throws, its siblings are asked to stop, all are joined,
//     and the FIRST exception is rethrown from scope() (or from join(),
//     if the block joins that helper itself).
//   * ONLY THE THREAD THAT OPENED THE SCOPE may spawn() or join(). A helper
//     can reach the nursery or a sibling's handle through `[&]`, and that
//     is exactly how two bugs are written:
//       - helper A joins B while B joins A: each waits for the other
//         forever (a deadlock with no lock in sight)
//       - a helper spawns while the owner spawns: two threads mutate the
//         nursery's list at once (a data race)
//     With owner-only spawn/join, the only waits are the owner waiting on
//     its own helpers, so the wait graph is a tree and has no cycle, and
//     the list has one writer. Checked on every call, in every build: a
//     helper that tries gets scope_misuse (thrown in the helper, so it
//     reaches scope() like any helper error). It's a runtime check because
//     C++ can't forbid `[&]` from reaching `n` without also forbidding the
//     borrowing scope exists for.
//
// What C++ can't stop, stated plainly:
//   * storing `&n` somewhere that outlives the block, then spawning on it
//     later. The nursery is gone by then; that's a use-after-scope.
//   * two helpers writing the same captured local: a data race. Return
//     values through join() instead. TSan catches the violations; the
//     scope_test soak runs under it.

#include <atomic>
#include <concepts>
#include <cstddef>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>

#include "waits.hpp"

namespace jaal {

/// A helper tried to spawn() or join(): only the scope's own thread may.
/// (See "ONLY THE THREAD THAT OPENED THE SCOPE" above for why.)
struct scope_misuse : std::logic_error {
    explicit scope_misuse(const char* what)
        : std::logic_error(std::string("jaal: ") + what
                           + " from a helper thread; only the thread that opened the "
                             "scope may (a helper waiting on a sibling can deadlock)") {}
};

class nursery;

namespace detail::scope {

// One helper's result slot: a value or an exception, written by the helper
// thread and read by join() after the thread is joined (the join is the
// synchronisation, so no atomics are needed on the slot itself).
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

}  // namespace detail::scope

/// A running helper. join() waits for it and returns its value (or rethrows
/// its exception). Can't be copied or moved; lives inside the scope.
template <class T>
class handle {
public:
    handle(const handle&)            = delete;
    handle& operator=(const handle&) = delete;
    handle(handle&&)                 = delete;
    handle& operator=(handle&&)      = delete;

    /// Wait for the helper and take its result. Call at most once, from
    /// the scope's own thread.
    T join() {
        if (std::this_thread::get_id() != owner_) throw scope_misuse("handle::join()");
        if (taken_) throw std::logic_error("jaal: handle::join() called twice");
        taken_ = true;
        observed_->store(true, std::memory_order_relaxed);
        if (thread_->joinable()) thread_->join();
        if (slot_->error) {
            auto e = std::exchange(slot_->error, nullptr);
            std::rethrow_exception(e);
        }
        if constexpr (!std::is_void_v<T>) return std::move(*slot_->value);
    }

private:
    friend class nursery;
    handle(std::jthread* t, detail::scope::slot<T>* s, std::atomic<bool>* observed,
           std::thread::id owner)
        : thread_(t), slot_(s), observed_(observed), owner_(owner) {}

    std::jthread*            thread_;
    detail::scope::slot<T>*  slot_;
    std::atomic<bool>*       observed_;
    std::thread::id          owner_;
    bool                     taken_ = false;
};

/// Spawns helpers that are all joined before the owning scope() returns.
class nursery {
public:
    nursery(const nursery&)            = delete;
    nursery& operator=(const nursery&) = delete;
    nursery(nursery&&)                 = delete;
    nursery& operator=(nursery&&)      = delete;

    /// Start a helper. F may take a std::stop_token as its only argument,
    /// or nothing. Returns a handle to join it by.
    template <class F>
        requires std::invocable<F&, std::stop_token> || std::invocable<F&>
    [[nodiscard]] auto spawn(F f) {
        if (std::this_thread::get_id() != owner_) throw scope_misuse("nursery::spawn()");
        using R = decltype(invoke_body(f, std::stop_token{}));
        auto& e = add<R>();
        auto* s = static_cast<detail::scope::slot<R>*>(e.slot.get());
        auto* obs = &e.observed;
        e.thread = std::jthread(
            [this, s, f = std::move(f),
             waiting = kernel::waits_detail::for_helper()](std::stop_token child) mutable {
                // This helper is waited on by its owner (and whoever waits on
                // the owner): a mailbox those threads drain won't block it.
                kernel::waits_detail::waiters() = std::move(waiting);
                try {
                    if constexpr (std::is_void_v<R>) {
                        invoke_body(f, merged(child));
                        s->done = true;
                    } else {
                        s->value.emplace(invoke_body(f, merged(child)));
                    }
                } catch (...) {
                    s->error = std::current_exception();
                    stop_.request_stop();          // siblings: please stop
                }
            });
        return handle<R>(&e.thread, s, obs, owner_);
    }

    /// The nursery's stop token (parent stop, a sibling failure, or the
    /// block throwing all trigger it).
    [[nodiscard]] std::stop_token stop_token() const noexcept { return stop_.get_token(); }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    template <class F>
    static decltype(auto) invoke_body(F& f, std::stop_token st) {
        if constexpr (std::invocable<F&, std::stop_token>) return f(std::move(st));
        else                                              return f();
    }

    // A helper stops when EITHER its own jthread is asked to stop or the
    // nursery is. The nursery token is the one that matters in practice.
    std::stop_token merged(const std::stop_token&) const { return stop_.get_token(); }

    struct entry {
        std::jthread                               thread;
        std::unique_ptr<void, void (*)(void*)>     slot{nullptr, [](void*) {}};
        std::atomic<bool>                          observed{false};
        std::exception_ptr (*take_error)(void*)    = nullptr;
    };

    template <class R>
    entry& add() {
        auto& e = entries_.emplace_back();         // std::list: stable addresses
        e.slot = std::unique_ptr<void, void (*)(void*)>(
            new detail::scope::slot<R>{},
            [](void* p) { delete static_cast<detail::scope::slot<R>*>(p); });
        e.take_error = [](void* p) {
            return std::exchange(static_cast<detail::scope::slot<R>*>(p)->error, nullptr);
        };
        return e;
    }

    template <class B>
    friend auto scope(std::stop_token parent, B&& block);

    explicit nursery(std::stop_token parent)
        : on_parent_stop_(parent, [this] { stop_.request_stop(); }) {}

    std::thread::id                             owner_ = std::this_thread::get_id();

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

/// Run `block(nursery&)`, then join every helper it spawned. Returns the
/// block's result. If the block or any helper threw, rethrows the first
/// exception AFTER everything is joined.
template <class B>
auto scope(std::stop_token parent, B&& block) {
    nursery n(std::move(parent));
    using R = std::invoke_result_t<B&, nursery&>;
    std::exception_ptr block_error;
    std::optional<std::conditional_t<std::is_void_v<R>, int, R>> result;
    try {
        if constexpr (std::is_void_v<R>) block(n);
        else                            result.emplace(block(n));
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
auto scope(B&& block) {
    return scope(std::stop_token{}, std::forward<B>(block));
}

}  // namespace jaal
