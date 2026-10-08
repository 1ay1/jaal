#pragma once
// jaal::kernel::stop_group — cancel and wait for work you don't schedule.
//
// pool and scope cover work jaal RUNS: it owns the thread, so it can ask and
// wait. But a program also has work running on threads it does not own: a
// subagent inside a Cmd::task_isolated, a request handler on a server's
// thread, anything a library calls back into. Shutdown still has to stop
// those and know when they're gone, and today every such site grows the same
// registry:
//
//     struct Registry {
//         std::mutex mu;
//         std::condition_variable cv;
//         std::vector<std::shared_ptr<State>> runs;   // State: cancelled, done
//     };
//     // + a register/deregister pair, a notify on the way out, a bounded
//     //   wait that re-scans every entry's `done` on each wakeup
//
// That is a stop_source plus a counted barrier, written by hand, with a
// second cancellation spelling (a `cancelled` atomic) that cannot be handed
// to anything expecting a std::stop_token.
//
// THE SHAPE
//
//     jaal::kernel::stop_group g;
//
//     // wherever the work runs, on whatever thread:
//     if (auto m = g.join()) {            // nullopt: group already stopping
//         do_work(m->token());            // a real std::stop_token
//     }                                   // member leaves on destruction
//
//     // at shutdown:
//     std::size_t stuck = g.stop_and_wait(1500ms);
//
// WHY A std::stop_token AND NOT A FLAG
//
// Because that is what the rest of the work already takes. A subagent's
// stream bridges cancellation into an HTTP token; a delay wants
// jaal::kernel::delay_for(st, d); a stop_callback can fire on it. A bare
// atomic<bool> fits none of those without a poll loop, which is exactly the
// bridge thread agentty was running for it.
//
// ADMISSION IS CLOSED BY stop_and_wait
//
// join() after stop has started returns nullopt instead of a member. Without
// that, a run that starts during shutdown registers after the snapshot is
// taken, is never asked to stop, and is never waited for — the same
// check-then-register gap pool::post_isolated closes by counting under its
// own lock. Same answer here: the count and the closed flag share one mutex.
//
// THE WAIT IS EVENT-DRIVEN
//
// The last member out notifies; stop_and_wait does not rescan a list on every
// wakeup. The count is the whole state, so the predicate is O(1).
//
// Bounded or not is the CALLER's: pass pool::no_deadline-style nullopt for a
// barrier when the members write into something about to be freed, or a
// grace when a wedged member must not hold the process open.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>

namespace jaal::kernel {

class stop_group {
    // Shared with every member, so a member that outlives the group (it was
    // abandoned at the grace) still touches only memory it co-owns — the
    // same trick pool's `core` uses.
    struct core {
        std::mutex              m;
        std::condition_variable cv;
        std::stop_source        src;
        std::size_t             live    = 0;
        bool                    closed  = false;
    };

  public:
    /// One unit of work in the group. Move-only; leaving the group is the
    /// destructor, so an early return or a throw cannot strand the count.
    class member {
      public:
        member(member&& o) noexcept : c_(std::move(o.c_)) {}
        member& operator=(member&& o) noexcept {
            if (this != &o) { leave(); c_ = std::move(o.c_); }
            return *this;
        }
        member(const member&)            = delete;
        member& operator=(const member&) = delete;
        ~member() { leave(); }

        /// Stopped when the group is. Hand this to anything that takes a
        /// std::stop_token.
        [[nodiscard]] std::stop_token token() const noexcept {
            return c_ ? c_->src.get_token() : std::stop_token{};
        }

        [[nodiscard]] bool stop_requested() const noexcept {
            return c_ && c_->src.stop_requested();
        }

      private:
        friend class stop_group;
        explicit member(std::shared_ptr<core> c) noexcept : c_(std::move(c)) {}

        void leave() noexcept {
            if (!c_) return;
            bool last;
            {
                std::lock_guard lk(c_->m);
                last = (--c_->live == 0);
            }
            // Notify outside the lock: the waiter re-takes it immediately.
            if (last) c_->cv.notify_all();
            c_.reset();
        }

        std::shared_ptr<core> c_;
    };

    stop_group() : c_(std::make_shared<core>()) {}
    stop_group(const stop_group&)            = delete;
    stop_group& operator=(const stop_group&) = delete;

    /// Enter the group. nullopt once stop_and_wait() has begun: the caller
    /// should not start the work at all, because nobody will stop or wait
    /// for it.
    [[nodiscard]] std::optional<member> join() {
        std::lock_guard lk(c_->m);
        if (c_->closed) return std::nullopt;
        ++c_->live;
        return member{c_};
    }

    /// How many members are running. For tests and diagnostics; the answer
    /// can be stale by the time it is read.
    [[nodiscard]] std::size_t live() const {
        std::lock_guard lk(c_->m);
        return c_->live;
    }

    /// Wait for every member to leave on its own, WITHOUT requesting stop.
    /// For an owner that wants a job's natural end (a reader reaching EOF)
    /// rather than cancelling it. Admission stays open.
    void wait() {
        std::unique_lock lk(c_->m);
        c_->cv.wait(lk, [&] { return c_->live == 0; });
    }

    /// Close admission, request stop on every member, and wait for them to
    /// leave. `grace` nullopt waits however long it takes; otherwise returns
    /// after the grace with the count still running (0 = all out).
    ///
    /// Idempotent: a second call closes nothing new and just waits again.
    std::size_t stop_and_wait(std::optional<std::chrono::milliseconds> grace) {
        {
            std::lock_guard lk(c_->m);
            c_->closed = true;
        }
        // Outside the lock: stop callbacks run synchronously here, and one
        // that leaves the group would deadlock on m.
        c_->src.request_stop();

        std::unique_lock lk(c_->m);
        const auto done = [&] { return c_->live == 0; };
        if (grace)
            c_->cv.wait_for(lk, *grace, done);
        else
            c_->cv.wait(lk, done);
        return c_->live;
    }

  private:
    std::shared_ptr<core> c_;
};

}  // namespace jaal::kernel
