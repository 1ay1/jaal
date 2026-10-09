#pragma once
// jaal::kernel::worker_group — background jobs tied to one object's lifetime.
//
// The shape pool and scope leave out: a long-lived object owns workers that
// write into its collaborators, so its stop() must be a HARD BARRIER. When
// stop() returns, no job is running, and the owner may free what they used.
//
// pool::shutdown is bounded on purpose: it asks, waits out a grace, then
// abandons. That is safe for state a worker co-owns. It is not safe when a job
// writes into something a third party is about to free, and co-owning does not
// help when the thing that would dangle belongs to someone else. So this waits
// as long as it takes. A job that hangs forever hangs stop(); give jobs their
// own timeouts.
//
//     jaal::kernel::worker_group g;
//     g.post([&](std::stop_token st) { pump(st); });
//     ...
//     g.stop();   // stop requested on every job, then waits for all of them
//
// Admission: post() after stop() is dropped, decided under the pool's lock,
// so no job slips in between "still open?" and "counted".

#include <jaal/kernel/pool.hpp>

#include <utility>

namespace jaal::kernel {

class worker_group {
  public:
    /// `max_workers` caps concurrent jobs. Jobs are isolated (a thread each),
    /// because they typically block on IO; one is right for a group that
    /// serializes its own work.
    explicit worker_group(unsigned max_workers = 1,
                          pool::error_fn on_error = {})
        : pool_(max_workers, std::move(on_error)) {}

    worker_group(const worker_group&)            = delete;
    worker_group& operator=(const worker_group&) = delete;

    /// Joins on destruction, so an owner that forgets stop() is still safe.
    ~worker_group() { stop(); }

    /// Run `j` on a thread of its own. Dropped once the group is stopped.
    void post(pool::job j) { pool_.post_isolated(std::move(j)); }

    /// Barrier: when this returns, no job posted here is running. Idempotent.
    void stop() noexcept { (void)pool_.shutdown(pool::no_deadline); }

  private:
    pool pool_;
};

}  // namespace jaal::kernel
