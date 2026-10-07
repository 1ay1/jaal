#pragma once
// jaal::platform::sim_file_lock — contention you can script, with no second
// process and no kernel.
//
// WHAT THIS DOES **NOT** CLAIM TO TEST
//
// sim_process fakes the program and keeps the handles real, because a second
// handle universe would agree with itself and prove nothing about the OS. The
// same honesty is needed here, and it cuts deeper: the entire value of a file
// lock is a statement about the KERNEL arbitrating between PROCESSES. A sim
// that models that arbitration in a std::map proves that the std::map works.
//
// So this backend does not pretend to be mutual exclusion. The real checks —
// "a second process waits", "the wait ends when the holder dies", "a crash
// does not strand the lock" — only mean something against a real fork, and
// the conformance suite runs them on the POSIX backend and says so.
//
// WHAT IT IS FOR
//
// The other half: POLICY over a lock. A host has to decide what to do when
// the lock is busy, when the filesystem cannot lock at all, and when the
// lock is dropped mid-section. Those paths are the ones that ship broken,
// because reproducing them against a real kernel means racing a real process
// and usually means a sleep. Here the test just says what happens:
//
//   sim_file_lock::script().hold("x.json");        // someone else has it
//   sim_file_lock::script().fail("y.json", EROFS); // this FS cannot lock
//
// and the host's degrade path runs deterministically, in microseconds.
//
// The script is process-global and NOT thread-safe by design: a test that
// needs two threads contending wants jaal::guarded, not this.

#include <memory>
#include <optional>
#include <string>

#include "../../core/error.hpp"
#include "../file_lock.hpp"

namespace jaal::platform {

class sim_file_lock {
  public:
    /// The scripted world every sim_file_lock consults. One per process.
    class world {
      public:
        /// Pretend another process holds `target`'s lock. try_acquire returns
        /// nullopt; acquire would block forever, so it reports deadlock
        /// instead of hanging a test — a sim that blocks is a sim nobody can
        /// use in CI.
        void hold(const std::string& target);
        /// Stop pretending. Also what a "the holder exited" step looks like.
        void drop(const std::string& target);
        /// Make acquisition FAIL with this errno: a read-only filesystem, a
        /// filesystem with no lock support, an exhausted descriptor table.
        void fail(const std::string& target, int err);
        /// Forget every scripted fact. Call between tests.
        void reset();

        /// How many times `target` has been locked, for a test asserting the
        /// host took the lock at all rather than merely claiming to.
        [[nodiscard]] int acquisitions(const std::string& target) const;
    };

    [[nodiscard]] static world& script() noexcept;

    sim_file_lock() noexcept;
    sim_file_lock(sim_file_lock&&) noexcept;
    sim_file_lock& operator=(sim_file_lock&&) noexcept;
    sim_file_lock(const sim_file_lock&)            = delete;
    sim_file_lock& operator=(const sim_file_lock&) = delete;
    ~sim_file_lock();

    [[nodiscard]] static result<sim_file_lock> acquire(
        const std::string& target, lock_mode m = lock_mode::exclusive);
    [[nodiscard]] static result<std::optional<sim_file_lock>> try_acquire(
        const std::string& target, lock_mode m = lock_mode::exclusive);

    [[nodiscard]] bool held() const noexcept;
    void               release() noexcept;
    [[nodiscard]] const std::string& path() const noexcept;

  private:
    struct impl;
    std::unique_ptr<impl> p_;
};

static_assert(FileLock<sim_file_lock>,
              "the sim backend must satisfy the same concept as the real "
              "ones, or the one-suite-every-backend contract is a fiction");

}  // namespace jaal::platform
