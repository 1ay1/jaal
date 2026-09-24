#pragma once
// jaal::kernel::mailbox — messages from anywhere into the loop.
//
// Multi-producer (workers, timers, hosts), single-consumer (the loop).
//
// Wake protocol, from maya's BackgroundQueue:
//   * post() takes the lock, pushes, and signals the waker ONLY on the
//     empty → non-empty transition. If it was already non-empty, whoever
//     made it non-empty already signalled. So wakes coalesce and none is
//     lost.
//   * drain() swaps the queue out under the same lock.
//   * If the waker can't be created, the loop still drains every turn:
//     slower, never lost.
//
// Lifetime, also from maya (where getting it wrong meant a worker
// destroying its own queue and joining itself):
//   * the kernel holds the ONLY shared_ptr; sinks hold weak_ptr
//   * so the mailbox is always destroyed on the loop thread
//   * close() makes every later post() fail, so a task that outlives the
//     loop just gets `false`
//
// Origins: every message remembers where it came from. Origin 0 is "no
// particular source" (tasks, hosts). A stream run gets its own origin, and
// when the loop stops that run it RETIRES the origin: from then on drain()
// drops every message from it, including ones already sitting in the queue.
//
// That last part is the point. Checking "is my stream still live?" on the
// SENDER's side can't be made correct: a message that passed the check and
// was queued before the stop is still in the queue after it, and the loop
// would fold a stale message. Liveness is loop state, so it's checked on
// the loop, at the one place every message passes: drain. No window.
// (tests/kernel/stream_stop_test.cpp reproduces the old window
// deterministically.)
//
// Capacity (optional). Unbounded by default. With a capacity, a producer
// faster than the loop can't grow memory without bound, and what happens
// when it's full is a choice the program makes:
//
//   overflow::block        the sender waits for room (backpressure). A
//                          blocked sender is released with `false` when the
//                          mailbox closes, so shutdown can't hang on it.
//                          The LOOP thread itself never blocks: posting to
//                          your own full mailbox would wait for a drain that
//                          only you can do. On the loop thread a full block
//                          mailbox returns false instead (and counts it).
//   overflow::drop_newest  the new message is refused; send() returns false.
//   overflow::drop_oldest  the oldest queued message is discarded to make
//                          room; send() returns true. For "latest value
//                          wins" streams (progress, telemetry).
//
// Counters (dropped, blocked, high-water) are kept so a program can see
// when it's overloaded instead of guessing.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../core/sendable.hpp"
#include "../core/sink.hpp"
#include "waits.hpp"

namespace jaal::kernel {

enum class overflow : std::uint8_t { block, drop_newest, drop_oldest };

/// Where a message came from. 0 = no particular source; a stream run gets
/// its own, so the loop can drop what it sent after stopping it.
using origin_id = std::uint64_t;

struct mailbox_options {
    std::size_t capacity = 0;                 // 0 = unbounded
    overflow    on_full  = overflow::block;
};

struct mailbox_stats {
    std::uint64_t dropped    = 0;             // refused (newest) or evicted (oldest)
    std::uint64_t blocked    = 0;             // sends that had to wait for room
    std::uint64_t loop_full  = 0;             // blocking posts refused: the loop waits on the sender
    std::uint64_t retired    = 0;             // dropped at drain: their stream had stopped
    std::size_t   high_water = 0;             // most messages ever queued at once
};

template <Sendable Msg>
class mailbox final : public detail::mailbox_iface<Msg> {
public:
    using msg_type = Msg;

    explicit mailbox(std::function<void()> wake = {}, mailbox_options opt = {})
        : wake_(std::move(wake)), opt_(opt), loop_(std::this_thread::get_id()) {}

    mailbox(const mailbox&)            = delete;
    mailbox& operator=(const mailbox&) = delete;

    /// Thread-safe. False when closed, or when full under drop_newest (or
    /// block, on the loop thread).
    ///
    /// The wake happens UNDER the lock. With a real reactor behind wake_,
    /// waking after unlocking races shutdown: a detached task could be
    /// between unlock and wake when the kernel closes the mailbox and the
    /// reactor is destroyed, and its wake would write into a closed fd.
    /// Under the lock, close() waits for any post in progress.
    bool post(Msg m) override { return post_from(0, std::move(m)); }

    /// post(), tagged with where it came from (see "Origins" above). A
    /// message from an origin already retired is refused here too, which
    /// saves queueing it only to drop it at drain.
    bool post_from(origin_id from, Msg m) {
        std::unique_lock lk(m_);
        if (closed_) return false;
        if (from != 0 && is_retired(from)) return false;
        if (full()) {
            switch (opt_.on_full) {
                case overflow::drop_newest:
                    ++stats_.dropped;
                    return false;
                case overflow::drop_oldest:
                    q_.pop_front();
                    ++stats_.dropped;
                    break;
                case overflow::block:
                    // Waiting for the loop to drain is a deadlock when the
                    // loop is (transitively) waiting on THIS thread: the loop
                    // itself, or a scope helper the loop is joining. Refuse
                    // instead of waiting forever (kernel/waits.hpp).
                    if (std::this_thread::get_id() == loop_
                        || waits_detail::waited_on_by(loop_)) {
                        ++stats_.loop_full;
                        return false;
                    }
                    ++stats_.blocked;
                    room_.wait(lk, [&] { return closed_ || !full(); });
                    if (closed_) return false;   // released by shutdown
                    // It may have been retired while we waited.
                    if (from != 0 && is_retired(from)) return false;
                    break;
            }
        }
        const bool first = q_.empty();
        q_.push_back({from, std::move(m)});
        nonempty_.store(true, std::memory_order_release);
        if (q_.size() > stats_.high_water) stats_.high_water = q_.size();
        if (first && wake_) wake_();             // only on empty → non-empty
        return true;
    }

    /// Loop thread only. Takes everything queued; `out` is reused so a
    /// steady stream doesn't allocate per turn. Messages from a retired
    /// origin are dropped here, whenever they were queued.
    ///
    /// Hot path: most steps find the mailbox empty. `nonempty_` is set
    /// (release) under the lock when a message is pushed and cleared under
    /// the lock when drained, so a false read means "empty as of the last
    /// drain" and the lock is skipped. A message pushed concurrently is seen
    /// on the next step, and it also woke the loop, so it isn't delayed.
    void drain(std::vector<Msg>& out) {
        std::vector<entry> tmp;
        drain_tagged(tmp);
        out.clear();
        out.reserve(tmp.size());
        for (auto& e : tmp) out.push_back(std::move(e.msg));
    }

    /// drain(), keeping each message's origin. The kernel carries the origin
    /// up to the moment it folds the message and checks it AGAIN there: a
    /// stream can be retired after its message was drained but before it
    /// was folded (a batch [Rekey, Item] retires Item's stream at Rekey).
    struct entry {
        origin_id from;
        Msg       msg;
    };
    void drain_tagged(std::vector<entry>& out) {
        out.clear();
        if (!nonempty_.load(std::memory_order_acquire)) return;
        {
            std::lock_guard lk(m_);
            nonempty_.store(false, std::memory_order_relaxed);
            if (q_.empty()) return;
            out.reserve(q_.size());
            for (auto& e : q_) {
                if (e.from != 0 && is_retired(e.from)) { ++stats_.retired; continue; }
                out.push_back(std::move(e));
            }
            q_.clear();
        }
        if (opt_.capacity != 0) room_.notify_all();   // blocked senders: go
    }

    /// A new origin, live until retire(). Loop thread.
    [[nodiscard]] origin_id open_origin() {
        std::lock_guard lk(m_);
        const origin_id id = ++last_origin_;
        live_.insert(id);
        return id;
    }

    /// Stop accepting from `id`, and drop what it already queued (at the
    /// next drain). Loop thread. Takes the lock, so a post in progress
    /// either finished before this (and drain drops it) or sees it retired.
    void retire(origin_id id) {
        {
            std::lock_guard lk(m_);
            live_.erase(id);
        }
        // A sender blocked on a full queue re-checks: it's retired now.
        if (opt_.capacity != 0) room_.notify_all();
    }

    [[nodiscard]] bool empty() const {
        std::lock_guard lk(m_);
        return q_.empty();
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lk(m_);
        return q_.size();
    }

    [[nodiscard]] mailbox_stats stats() const {
        std::lock_guard lk(m_);
        return stats_;
    }

    /// Refuse further posts and release every blocked sender with `false`.
    /// Called by the kernel during shutdown, on the loop thread.
    void close() {
        {
            std::lock_guard lk(m_);
            closed_ = true;
            q_.clear();
            live_.clear();
            nonempty_.store(false, std::memory_order_relaxed);
        }
        room_.notify_all();
    }

    [[nodiscard]] bool closed() const {
        std::lock_guard lk(m_);
        return closed_;
    }

    /// Count a message dropped after drain because its origin retired.
    void count_retired() {
        std::lock_guard lk(m_);
        ++stats_.retired;
    }

private:
    bool full() const noexcept { return opt_.capacity != 0 && q_.size() >= opt_.capacity; }

    // Origins are handed out in increasing order and never reused, so
    // "retired" is "issued, and not in live_". Called with m_ held.
    bool is_retired(origin_id id) const noexcept {
        return id <= last_origin_ && !live_.contains(id);
    }

    mutable std::mutex      m_;
    std::condition_variable room_;
    std::deque<entry>       q_;
    std::atomic<bool>       nonempty_{false};    // hint for drain's lock-free empty check
    std::function<void()>   wake_;
    mailbox_options         opt_;
    mailbox_stats           stats_{};
    std::thread::id         loop_;               // the thread that owns this mailbox
    std::unordered_set<origin_id> live_;         // open origins
    origin_id               last_origin_   = 0;
    bool                    closed_ = false;
};

/// Owns a mailbox and hands out sinks to it. The kernel holds one of these.
template <Sendable Msg>
class inbox {
public:
    explicit inbox(std::function<void()> wake = {}, mailbox_options opt = {})
        : box_(std::make_shared<mailbox<Msg>>(std::move(wake), opt)) {}

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

    /// A sink whose messages carry `from`, for a source the loop may stop.
    /// Once the loop retires `from`, everything it sent is dropped, even
    /// messages already queued. The returned pair keeps the tagging object
    /// alive: hold `owner` for as long as the source runs (the Sink itself
    /// is weak, like every Sink).
    struct tagged {
        std::shared_ptr<detail::mailbox_iface<Msg>> owner;
        Sink<Msg>                                   sink;
    };
    [[nodiscard]] tagged sink_from(origin_id from) const {
        struct tagger final : detail::mailbox_iface<Msg> {
            std::weak_ptr<mailbox<Msg>> box;
            origin_id                   from;
            tagger(std::weak_ptr<mailbox<Msg>> b, origin_id f) : box(std::move(b)), from(f) {}
            bool post(Msg m) override {
                if (auto b = box.lock()) return b->post_from(from, std::move(m));
                return false;
            }
        };
        auto t = std::make_shared<tagger>(box_, from);
        auto s = sink_access::make<Msg>(std::weak_ptr<detail::mailbox_iface<Msg>>(t));
        return {std::move(t), std::move(s)};
    }

    [[nodiscard]] origin_id open_origin()      { return box_->open_origin(); }
    void retire(origin_id id)                  { box_->retire(id); }

    void drain(std::vector<Msg>& out)          { box_->drain(out); }
    using entry = typename mailbox<Msg>::entry;
    void drain_tagged(std::vector<entry>& out) { box_->drain_tagged(out); }
    void count_retired()                       { box_->count_retired(); }
    [[nodiscard]] bool empty() const           { return box_->empty(); }
    [[nodiscard]] std::size_t size() const     { return box_->size(); }
    [[nodiscard]] mailbox_stats stats() const  { return box_->stats(); }
    void close()                               { box_->close(); }

    /// Post directly (the loop's own re-entrant sends).
    bool post(Msg m) { return box_->post(std::move(m)); }

private:
    std::shared_ptr<mailbox<Msg>> box_;
};

}  // namespace jaal::kernel
