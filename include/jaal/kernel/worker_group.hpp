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
//     g.post([](std::stop_token st, jaal::co_owned<Pump> p) { p->run(st); }, pump);
//     ...
//     g.stop(cap);   // stop requested on every job, then waits for all of them
//
// Jobs follow pool's rule: a captureless body, Sendable arguments, and an
// optional leading `const jaal::unlocked&` (a job's thread holds no lock).
//
// stop() waits for other threads, which may need any lock the caller holds,
// so it takes `unlocked`: stopping under a lock does not compile.
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
    /// A destructor can't be handed a capability; it runs where the owner
    /// dies, and an owner holding the group inside a lock body is the one
    /// shape this can't see. Call stop(cap) explicitly to have it checked.
    ~worker_group() { (void)pool_.shutdown_noexcept(pool::no_deadline); }

    /// Run `body([unlocked,] stop_token, args...)` on a thread of its own.
    /// Dropped once the group is stopped.
    template <class F, class... Args>
    void post(F body, Args... args) { pool_.post_isolated(body, std::move(args)...); }

    /// Barrier: when this returns, no job posted here is running. Idempotent.
    void stop(const unlocked&) noexcept { (void)pool_.shutdown_noexcept(pool::no_deadline); }

  private:
    pool pool_;
};

}  // namespace jaal::kernel

// ~worker_group waits for every job: never owned by a guarded<T>.
template <> inline constexpr bool jaal::joins_on_destroy<jaal::kernel::worker_group> = true;
