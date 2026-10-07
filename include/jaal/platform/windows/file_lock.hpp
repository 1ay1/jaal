#pragma once
// jaal::platform::windows_file_lock — a cross-process critical section, on
// LockFileEx.
//
// The shape matches the POSIX backend, the ownership rules do not, and the
// difference is the one a host has to know:
//
//   * A Windows byte-range lock is owned by the HANDLE, not the process. So
//     two threads of one process genuinely contend here, where under fcntl
//     they both walk straight through. That is the nicer semantic, and a
//     host must NOT rely on it: the concept promises only the cross-process
//     guarantee, and code that leans on the Windows behaviour breaks on
//     Linux in a way no test on Windows can see. Pair with jaal::guarded on
//     both platforms.
//
//   * There is no EINTR. LockFileEx either waits or fails, so acquire() has
//     no retry loop.
//
//   * A crashed holder's locks are released by the kernel when the process's
//     handles are closed, same as POSIX. Neither platform strands the lock.
//
// LOCKFILE_FAIL_IMMEDIATELY is the try_acquire flag; its busy answer is
// ERROR_LOCK_VIOLATION, which is mapped to nullopt rather than an error for
// the same reason EAGAIN is on POSIX — "busy" is an outcome a caller acts on,
// "this volume cannot lock" is a fault it reports.

#include <memory>
#include <optional>
#include <string>

#include "../../core/error.hpp"
#include "../file_lock.hpp"

namespace jaal::platform {

class windows_file_lock {
  public:
    windows_file_lock() noexcept;
    windows_file_lock(windows_file_lock&&) noexcept;
    windows_file_lock& operator=(windows_file_lock&&) noexcept;
    windows_file_lock(const windows_file_lock&)            = delete;
    windows_file_lock& operator=(const windows_file_lock&) = delete;
    ~windows_file_lock();

    [[nodiscard]] static result<windows_file_lock> acquire(
        const std::string& target, lock_mode m = lock_mode::exclusive);
    [[nodiscard]] static result<std::optional<windows_file_lock>> try_acquire(
        const std::string& target, lock_mode m = lock_mode::exclusive);

    [[nodiscard]] bool held() const noexcept;
    void               release() noexcept;
    [[nodiscard]] const std::string& path() const noexcept;

  private:
    struct impl;
    std::unique_ptr<impl> p_;
};

static_assert(FileLock<windows_file_lock>,
              "the windows backend must satisfy the same concept as the "
              "others, or the one-suite-every-backend contract is a fiction");

}  // namespace jaal::platform
