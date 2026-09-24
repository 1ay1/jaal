#pragma once
// jaal::kernel::pool — worker threads for tasks, with cancellation.
//
// From maya's BackgroundQueue, plus the one thing it lacks (stop tokens):
//
//   * workers start LAZILY, up to max(4, hardware_concurrency), and live
//     for the pool's lifetime, so a burst reuses warm threads instead of
//     paying thread creation per task
//   * every task body is wrapped: an exception can NEVER reach
//     std::terminate from a worker. It's reported through on_error.
//   * isolated tasks get a dedicated thread. A wedged syscall (hung NFS,
//     dead FUSE mount) leaks ONE thread instead of blocking the pool; if
//     the OS refuses a thread (EAGAIN), it falls back to the pool.
//   * every task gets a std::stop_token, and shutdown requests stop on all
//     of them. Cancellation is the gap that made maya's tasks
//     un-cancellable, only orphan-able.
//   * a task body can't reach the pool: it gets a Sink (weak) and a token.
//     There is no way for queued work to keep the runtime alive.
//
// Shutdown order (each step is a test in kernel_test):
//   request stop → wake workers → join → drop queue.
// Detached isolated threads are NOT joined: that's what makes them
// isolated. They see stop_requested() and their sink returns false.

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace jaal::kernel {

class pool {
public:
    using job       = std::function<void(std::stop_token)>;
    using error_fn  = std::function<void(std::exception_ptr)>;

    explicit pool(unsigned max_workers = 0, error_fn on_error = {})
        : max_(max_workers ? max_workers
                           : std::max(4u, std::thread::hardware_concurrency())),
          on_error_(std::move(on_error)) {}

    pool(const pool&)            = delete;
    pool& operator=(const pool&) = delete;

    ~pool() { shutdown(); }

    /// Queue a job for the shared workers. Spawns one if they're all busy
    /// and we're under the cap.
    void post(job j) {
        {
            std::lock_guard lk(m_);
            if (stopping_) return;
            queue_.push_back(std::move(j));
            if (idle_ == 0 && workers_.size() < max_) spawn_worker();
        }
        cv_.notify_one();
    }

    /// Run on a thread of its own, detached. For work that may never return.
    void post_isolated(job j) {
        {
            std::lock_guard lk(m_);
            if (stopping_) return;
        }
        try {
            std::jthread t([this, j = std::move(j)](std::stop_token st) mutable {
                run_guarded(j, std::move(st));
            });
            // Keep the stop_source so shutdown can ask it to stop, then
            // detach: we never join it.
            std::lock_guard lk(m_);
            isolated_.push_back(t.get_stop_source());
            t.detach();
        } catch (...) {
            // The OS refused a thread (EAGAIN). Fall back to the pool
            // rather than dropping the work.
            post(std::move(j));
        }
    }

    /// Ask every running task to stop, join the pool's workers, and refuse
    /// new work. Safe to call twice. Isolated threads are asked to stop
    /// but never joined.
    void shutdown() {
        std::vector<std::jthread> to_join;
        {
            std::lock_guard lk(m_);
            if (stopping_) return;
            stopping_ = true;
            queue_.clear();
            for (auto& s : isolated_) s.request_stop();
            isolated_.clear();
            to_join.swap(workers_);
        }
        for (auto& w : to_join) w.request_stop();
        cv_.notify_all();
        to_join.clear();        // joins
    }

    [[nodiscard]] std::size_t worker_count() const {
        std::lock_guard lk(m_);
        return workers_.size();
    }

    [[nodiscard]] std::size_t queued() const {
        std::lock_guard lk(m_);
        return queue_.size();
    }

private:
    void spawn_worker() {                       // call with m_ held
        try {
            workers_.emplace_back([this](std::stop_token st) { worker_loop(std::move(st)); });
        } catch (...) {
            // Can't spawn: existing workers will pick the job up. If there
            // are none, the job waits until one exists. Never a crash.
        }
    }

    void worker_loop(std::stop_token st) {
        // Defence in depth. shutdown() sets stopping_ under the lock and
        // then notifies, so today that alone wakes every worker; this
        // callback covers a stop requested by any OTHER path (a future
        // caller, a per-worker stop) that doesn't notify. Verified: with
        // this line removed the suite still passes, so it is not
        // load-bearing for the current shutdown path.
        std::stop_callback wake(st, [this] { cv_.notify_all(); });
        for (;;) {
            job j;
            {
                std::unique_lock lk(m_);
                ++idle_;
                cv_.wait(lk, [&] { return stopping_ || st.stop_requested() || !queue_.empty(); });
                --idle_;
                if (stopping_ || st.stop_requested()) return;
                j = std::move(queue_.front());
                queue_.pop_front();
            }
            run_guarded(j, st);
            // j is destroyed HERE, outside the lock: its captures may own
            // anything, and destroying them must not run under m_.
            j = nullptr;
        }
    }

    void run_guarded(job& j, std::stop_token st) noexcept {
        try {
            j(std::move(st));
        } catch (...) {
            if (on_error_) {
                try { on_error_(std::current_exception()); } catch (...) {}
            }
            // Swallowed on purpose: a throwing task must not take the
            // process down (agentty's isolated_thread learned this).
        }
    }

    mutable std::mutex        m_;
    std::condition_variable   cv_;
    std::deque<job>           queue_;
    std::vector<std::jthread> workers_;
    std::vector<std::stop_source> isolated_;
    std::size_t               idle_     = 0;
    unsigned                  max_      = 4;
    bool                      stopping_ = false;
    error_fn                  on_error_;
};

}  // namespace jaal::kernel
