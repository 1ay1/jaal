#pragma once
// jaal::kernel::published<T> — the current version of a shared object.
//
// A program often has ONE live instance of something many threads use: a
// connection pool, a compiled catalog, a provider handle. A writer builds a
// new one and swaps it in; readers grab the current one and keep it alive
// for as long as they use it, even across a swap.
//
// guarded<T> refuses that shape, rightly: a std::shared_ptr is a SHARED
// owner, so handing one out of a lock shares state past the lock. That is
// exactly what this pattern wants, and it is safe only when the pointee
// guards itself. published<T> is that pattern, named, with the condition
// stated where it is used:
//
//   jaal::kernel::published<Pool> pool;          // Pool synchronises itself
//   pool.publish(std::make_shared<Pool>(...));   // writer
//   if (auto p = pool.current()) p->use();       // reader, any thread
//   auto old = pool.take();                      // teardown: swap out
//
// Readers pay one atomic load (std::atomic<std::shared_ptr>), never a lock.
// A reader's handle keeps the old object alive after a swap, so a swap
// never frees anything under someone.
//
// What it does NOT do: make T thread-safe. T's own members are reached
// through the handle from many threads at once, so T must be immutable
// after publish, or synchronise its own mutable parts (its own guarded<U>,
// atomics). That is the claim a caller makes by choosing this type.

#include <atomic>
#include <memory>
#include <utility>

#include "../core/sendable.hpp"

namespace jaal::kernel {

template <class T>
class published {
public:
    published() = default;
    published(const published&)            = delete;
    published& operator=(const published&) = delete;

    /// Make `next` the current object. Readers that already hold the old
    /// one keep it until they drop it.
    void publish(std::shared_ptr<T> next) noexcept {
        slot_.store(std::move(next), std::memory_order_release);
    }

    /// Publish `next` only if `expected` is still current. Returns false
    /// (publishing nothing) when another writer got there first, so a
    /// rebuild can re-check against what that writer published.
    [[nodiscard]] bool publish_if(const std::shared_ptr<T>& expected,
                                  std::shared_ptr<T> next) noexcept {
        auto e = expected;
        return slot_.compare_exchange_strong(e, std::move(next),
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire);
    }

    /// The current object, or null if none is published.
    [[nodiscard]] std::shared_ptr<T> current() const noexcept {
        return slot_.load(std::memory_order_acquire);
    }

    /// Swap the current object out (leaving none) and return it, so the
    /// caller decides where it is destroyed.
    [[nodiscard]] std::shared_ptr<T> take() noexcept {
        return slot_.exchange(nullptr, std::memory_order_acq_rel);
    }

private:
    std::atomic<std::shared_ptr<T>> slot_;
};

}  // namespace jaal::kernel

namespace jaal {
// A published<T> is a process-wide slot, not a value to move around.
template <class T> inline constexpr bool sendable_opt_out<kernel::published<T>> = true;
}  // namespace jaal
