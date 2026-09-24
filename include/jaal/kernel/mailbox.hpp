#pragma once
// jaal::kernel::mailbox — messages from anywhere into the loop.
//
// Multi-producer (workers, timers, hosts), single-consumer (the loop).
//
// Wake protocol, from maya's BackgroundQueue:
//   * post() takes the lock, pushes, and signals the waker ONLY on the
//     empty → non-empty transition. If it was already non-empty, whoever
//     made it non-empty already signalled (or will, before releasing the
//     lock). So wakes coalesce and none is lost.
//   * drain() swaps the vector out under the same lock. If any message is
//     left in the mailbox, exactly one wake is pending.
//   * If the waker can't be created, the loop still drains every turn:
//     slower, never lost.
//
// Lifetime, also from maya (where getting it wrong meant a worker
// destroying its own queue and joining itself):
//   * the kernel holds the ONLY shared_ptr; sinks hold weak_ptr
//   * so the mailbox is always destroyed on the loop thread
//   * close() makes every later post() fail, so a task that outlives the
//     loop just gets `false`

#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "../core/sendable.hpp"
#include "../core/sink.hpp"

namespace jaal::kernel {

/// What the mailbox needs from a waker: a thread-safe nudge.
template <class W>
concept WakeSignal = requires(const W& w) {
    { w.wake() } noexcept;
};

/// A waker that does nothing (the loop polls instead).
struct no_wake {
    void wake() const noexcept {}
};

template <Sendable Msg>
class mailbox final : public detail::mailbox_iface<Msg> {
public:
    using msg_type = Msg;

    explicit mailbox(std::function<void()> wake = {}) noexcept : wake_(std::move(wake)) {}

    mailbox(const mailbox&)            = delete;
    mailbox& operator=(const mailbox&) = delete;

    /// Thread-safe. False once closed.
    ///
    /// The wake happens UNDER the lock. With a real reactor behind wake_,
    /// waking after unlocking races shutdown: a detached (isolated) task
    /// could be between unlock and wake when the kernel closes the mailbox
    /// and the reactor is destroyed, and its wake would write into a
    /// closed fd the kernel may already have handed to something else.
    /// Under the lock, close() waits for any post in progress, and every
    /// post after close() returns before waking. A wake is one short
    /// syscall and takes no other lock, so holding m_ across it is cheap
    /// and can't deadlock.
    bool post(Msg m) override {
        std::lock_guard lk(m_);
        if (closed_) return false;
        const bool first = q_.empty();
        q_.push_back(std::move(m));
        if (first && wake_) wake_();      // only on empty → non-empty
        return true;
    }

    /// Loop thread only. Takes everything queued; `out` is reused so a
    /// steady stream doesn't allocate per turn.
    void drain(std::vector<Msg>& out) {
        out.clear();
        std::lock_guard lk(m_);
        out.swap(q_);
    }

    [[nodiscard]] bool empty() const {
        std::lock_guard lk(m_);
        return q_.empty();
    }

    /// Refuse further posts. Called by the kernel during shutdown, on the
    /// loop thread, before the mailbox is destroyed.
    void close() {
        std::lock_guard lk(m_);
        closed_ = true;
        q_.clear();
    }

    [[nodiscard]] bool closed() const {
        std::lock_guard lk(m_);
        return closed_;
    }

private:
    mutable std::mutex    m_;
    std::vector<Msg>      q_;
    std::function<void()> wake_;
    bool                  closed_ = false;
};

/// Owns a mailbox and hands out sinks to it. The kernel holds one of these.
template <Sendable Msg>
class inbox {
public:
    explicit inbox(std::function<void()> wake = {})
        : box_(std::make_shared<mailbox<Msg>>(std::move(wake))) {}

    inbox(const inbox&)            = delete;
    inbox& operator=(const inbox&) = delete;
    inbox(inbox&&) noexcept        = default;
    inbox& operator=(inbox&&) noexcept = default;

    ~inbox() {
        if (box_) box_->close();
    }

    [[nodiscard]] Sink<Msg> sink() const {
        return sink_access::make<Msg>(
            std::weak_ptr<detail::mailbox_iface<Msg>>(box_));
    }

    void drain(std::vector<Msg>& out)  { box_->drain(out); }
    [[nodiscard]] bool empty() const   { return box_->empty(); }
    void close()                       { box_->close(); }

    /// Post directly (the loop's own re-entrant sends).
    bool post(Msg m) { return box_->post(std::move(m)); }

private:
    std::shared_ptr<mailbox<Msg>> box_;
};

}  // namespace jaal::kernel
