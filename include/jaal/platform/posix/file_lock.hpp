#pragma once
// jaal::platform::posix_file_lock — a cross-process critical section, on
// fcntl(2) record locks.
//
// WHY fcntl AND NOT flock
//
// Both exist and they are not the same lock. The choice matters here:
//
//   * fcntl(F_SETLKW) locks are attached to the (process, file) pair. They
//     work over NFS, which flock historically did not, and they are what
//     POSIX specifies — so a lock taken by this backend is honoured by any
//     other POSIX program using record locks on the same sidecar.
//
//   * flock(2) locks are attached to the OPEN FILE DESCRIPTION. They survive
//     dup() and fork() as the SAME lock, which sounds better and is worse
//     for us: a forked child would silently share the parent's critical
//     section instead of waiting for it.
//
// The cost of fcntl is the sharp edge the concept header warns about: the
// lock is owned by the PROCESS, so two threads of one process both get it,
// and a close() of any descriptor onto that file drops it for everybody. We
// accept both and state them, rather than picking flock and quietly getting
// the fork semantics wrong.
//
// WHY THE DESCRIPTOR IS OPENED O_CLOEXEC
//
// A child that inherits this descriptor inherits nothing about the lock
// (fcntl locks are not inherited across fork, and exec drops the fd), but an
// inherited descriptor keeps the SIDECAR open, and that is enough to break
// the close()-releases rule above in a way nobody can see: the parent
// releases, the child still holds the file open, and the next acquire in the
// parent races its own stale state. O_CLOEXEC removes the question.
//
// EINTR
//
// F_SETLKW returns EINTR when a signal arrives, with no information about
// how long it waited. A signal is not an answer, so acquire() retries. This
// is why acquire() has no timeout: a deadline would have to be re-derived
// across an unknown number of interruptions, and a caller who wants one is
// better served by try_acquire() plus its own clock, which is testable.

#include <memory>
#include <optional>
#include <string>

#include "../../core/error.hpp"
#include "../file_lock.hpp"

namespace jaal::platform {

class posix_file_lock {
  public:
    posix_file_lock() noexcept;
    posix_file_lock(posix_file_lock&&) noexcept;
    posix_file_lock& operator=(posix_file_lock&&) noexcept;
    posix_file_lock(const posix_file_lock&)            = delete;
    posix_file_lock& operator=(const posix_file_lock&) = delete;
    ~posix_file_lock();

    /// Lock the sidecar for `target` (see lock_sidecar_for). Blocks.
    [[nodiscard]] static result<posix_file_lock> acquire(
        const std::string& target, lock_mode m = lock_mode::exclusive);

    /// Non-blocking. nullopt = held by another process.
    [[nodiscard]] static result<std::optional<posix_file_lock>> try_acquire(
        const std::string& target, lock_mode m = lock_mode::exclusive);

    [[nodiscard]] bool held() const noexcept;
    void               release() noexcept;
    [[nodiscard]] const std::string& path() const noexcept;

  private:
    struct impl;
    std::unique_ptr<impl> p_;
};

static_assert(FileLock<posix_file_lock>,
              "the posix backend must satisfy the same concept as sim, or "
              "the one-suite-every-backend contract is a fiction");

}  // namespace jaal::platform
