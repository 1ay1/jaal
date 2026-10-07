#pragma once
// jaal::platform::posix_process — fork/exec, with the exit made watchable.
//
// The whole point of this backend is that "it ended" arrives through the
// Reactor like any other readiness, so a host waits for a child, a socket and
// a timer in ONE wait() rather than bolting waitpid onto a poll loop.
//
// How the exit becomes a handle, per platform:
//
//   Linux    pidfd_open(2). Exact: it becomes readable when THE LEADER
//            exits, whatever its descendants are doing, and it cannot be
//            confused by a pid reused after the exit.
//
//   other    a "death pipe": a pipe created before fork whose write end the
//   POSIX    child inherits and never writes to. When every copy of that end
//            is closed the parent's read end reports hangup. Deliberately
//            NOT close-on-exec, or exec would look like an exit.
//
// Those two are not the same statement, and the difference is visible:
// descendants inherit the death pipe, so it closes when the whole TREE has
// let go, while a pidfd fires when the leader does. `exit_is_exact()` says
// which one you have rather than leaving a host to guess — a runner that
// waits for the leader and one that waits for the tree want different
// answers, and both are reasonable.
//
// reap() is waitpid(WNOHANG) and never blocks: the blocking wait belongs to
// the reactor, and offering a second one here would give every caller a
// second way to hang.

#include <cstdint>
#include <memory>
#include <optional>

#include "../../core/error.hpp"
#include "../handle.hpp"
#include "../process.hpp"

namespace jaal::platform {

class posix_process {
  public:
    using handle = native_handle;

    posix_process(posix_process&&) noexcept;
    posix_process& operator=(posix_process&&) noexcept;
    posix_process(const posix_process&)            = delete;
    posix_process& operator=(const posix_process&) = delete;
    ~posix_process();

    [[nodiscard]] static result<posix_process> spawn(const process_spec&);

    [[nodiscard]] borrowed_handle                exit_handle()   const;
    [[nodiscard]] std::optional<borrowed_handle> stdout_handle() const;
    [[nodiscard]] std::optional<borrowed_handle> stderr_handle() const;
    [[nodiscard]] std::optional<borrowed_handle> stdin_handle()  const;

    [[nodiscard]] result<void>        stop(stop_mode, stop_scope);
    [[nodiscard]] result<exit_status> reap();

    /// A process group is not a tree: a descendant that calls setsid() leaves
    /// it and survives the kill. Plain POSIX can only reach the group, so
    /// this is false here. A Linux host that delegated a cgroup can do
    /// better, and that belongs in a backend that knows it has one.
    [[nodiscard]] bool tree_is_exact() const noexcept;

    /// Does exit_handle() mean "the leader ended" (pidfd) or "the tree let
    /// go" (death pipe)? See the header comment.
    [[nodiscard]] bool exit_is_exact() const noexcept;

    [[nodiscard]] std::uint64_t id() const noexcept;

  private:
    posix_process();
    struct impl;
    std::unique_ptr<impl> p_;
};

static_assert(Process<posix_process>,
              "the posix backend must satisfy the same concept as sim, or "
              "the one-suite-every-backend contract is a fiction");

}  // namespace jaal::platform
