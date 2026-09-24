#pragma once
// jaal::kernel::pool — worker threads for tasks, with cancellation and a
// shutdown that can't hang.
//
// From maya's BackgroundQueue, plus what it lacks:
//
//   * workers start LAZILY, up to max(4, hardware_concurrency), and live
//     for the pool's lifetime, so a burst reuses warm threads instead of
//     paying thread creation per task
//   * every task body is wrapped: an exception can NEVER reach
//     std::terminate from a worker. It goes to the error callback, which
//     the kernel turns into a reported fault.
//   * isolated tasks get a dedicated thread. A wedged syscall (hung NFS,
//     dead FUSE mount) leaks ONE thread instead of blocking the pool; if
//     the OS refuses a thread (EAGAIN), it falls back to the pool.
//   * every task gets a std::stop_token, and shutdown requests stop on all
//     of them.
//   * shutdown is BOUNDED. A worker that doesn't return within the grace
//     period is abandoned (detached) instead of blocking forever.
//
// Lifetime, the part that makes abandoning safe:
//   Everything a worker touches (the queue, the lock, the counters) lives
//   in a shared `core` that each worker co-owns through a shared_ptr. The
//   pool object holds one reference; each worker holds one. So an abandoned
//   worker that finally returns after the pool is gone touches only memory
//   it still owns. Without this, detaching would trade a hang for a
//   use-after-free.

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace jaal::kernel {

class pool {
public:
    using job      = std::function<void(std::stop_token)>;
    using error_fn = std::function<void(std::exception_ptr)>;

    explicit pool(unsigned max_workers = 0, error_fn on_error = {})
        : c_(std::make_shared<core>()) {
        c_->max = max_workers ? max_workers
                              : std::max(4u, std::thread::hardware_concurrency());
        c_->on_error = std::move(on_error);
    }

    pool(const pool&)            = delete;
    pool& operator=(const pool&) = delete;

    ~pool() { shutdown(); }

    /// Queue a job for the shared workers. Spawns one if they're all busy
    /// and we're under the cap.
    void post(job j) {
        {
            std::lock_guard lk(c_->m);
            if (c_->stopping) return;
            c_->queue.push_back(std::move(j));
            if (c_->idle == 0 && workers_.size() < c_->max) spawn_worker();
        }
        c_->cv.notify_one();
    }

    /// Run on a thread of its own, detached. For work that may never return.
    void post_isolated(job j) {
        {
            std::lock_guard lk(c_->m);
            if (c_->stopping) return;
        }
        try {
            // The thread co-owns the core, so it can report an error even
            // if it outlives the pool.
            std::jthread t([c = c_, j = std::move(j)](std::stop_token st) mutable {
                run_guarded(*c, j, std::move(st));
            });
            std::lock_guard lk(c_->m);
            c_->isolated.push_back(t.get_stop_source());
            t.detach();
        } catch (...) {
            post(std::move(j));      // the OS refused a thread: use the pool
        }
    }

    /// Ask every running task to stop, wait up to `grace` for the pool's
    /// workers to return, and refuse new work. Returns how many workers were
    /// ABANDONED (still running after the grace; detached, safely, see the
    /// lifetime note above). Safe to call twice. Isolated threads are asked
    /// to stop but never waited for.
    std::size_t shutdown(std::chrono::milliseconds grace = std::chrono::seconds(2)) {
        std::vector<std::jthread> workers;
        {
            std::lock_guard lk(c_->m);
            if (c_->stopping) return 0;
            c_->stopping = true;
            c_->queue.clear();
            for (auto& s : c_->isolated) s.request_stop();
            c_->isolated.clear();
            workers.swap(workers_);
        }
        for (auto& w : workers) w.request_stop();
        c_->cv.notify_all();

        // Wait for every worker to leave its loop, or for the grace to end.
        const std::size_t n = workers.size();
        bool all_out;
        {
            std::unique_lock lk(c_->m);
            all_out = c_->exited_cv.wait_for(lk, grace, [&] { return c_->exited >= n; });
        }
        if (all_out) {
            workers.clear();                 // every one has left: join is instant
            return 0;
        }
        std::size_t abandoned;
        {
            std::lock_guard lk(c_->m);
            abandoned = n - std::min(c_->exited, n);
        }
        // Some are stuck. Detach all: joining even the finished ones one by
        // one would risk blocking on a stuck one first. They co-own `core`.
        for (auto& w : workers) w.detach();
        return abandoned;
    }

    [[nodiscard]] std::size_t worker_count() const {
        std::lock_guard lk(c_->m);
        return workers_.size();
    }

    [[nodiscard]] std::size_t queued() const {
        std::lock_guard lk(c_->m);
        return c_->queue.size();
    }

private:
    // Everything a worker touches. Co-owned by the pool and every worker.
    struct core {
        std::mutex                    m;
        std::condition_variable       cv;
        std::condition_variable       exited_cv;
        std::deque<job>               queue;
        std::vector<std::stop_source> isolated;
        std::size_t                   idle     = 0;
        std::size_t                   exited   = 0;
        unsigned                      max      = 4;
        bool                          stopping = false;
        error_fn                      on_error;
    };

    void spawn_worker() {                           // call with c_->m held
        try {
            workers_.emplace_back([c = c_](std::stop_token st) { worker_loop(c, std::move(st)); });
        } catch (...) {
            // Can't spawn: existing workers pick the job up. Never a crash.
        }
    }

    static void worker_loop(std::shared_ptr<core> c, std::stop_token st) {
        // A stop request made by any path must wake a worker asleep in
        // wait(); shutdown() also notifies, this covers every other path.
        std::stop_callback wake(st, [raw = c.get()] { raw->cv.notify_all(); });
        for (;;) {
            job j;
            {
                std::unique_lock lk(c->m);
                ++c->idle;
                c->cv.wait(lk, [&] { return c->stopping || st.stop_requested() || !c->queue.empty(); });
                --c->idle;
                if (c->stopping || st.stop_requested()) break;
                j = std::move(c->queue.front());
                c->queue.pop_front();
            }
            run_guarded(*c, j, st);
            j = nullptr;                // destroy captures outside the lock
        }
        {
            std::lock_guard lk(c->m);
            ++c->exited;
        }
        c->exited_cv.notify_all();
    }

    static void run_guarded(core& c, job& j, std::stop_token st) noexcept {
        try {
            j(std::move(st));
        } catch (...) {
            error_fn cb;
            {
                std::lock_guard lk(c.m);
                cb = c.on_error;
            }
            if (cb) {
                try { cb(std::current_exception()); } catch (...) {}
            }
            // Never rethrown: a throwing task must not take the process
            // down (agentty's isolated_thread learned this).
        }
    }

    std::shared_ptr<core>     c_;
    std::vector<std::jthread> workers_;
};

}  // namespace jaal::kernel
