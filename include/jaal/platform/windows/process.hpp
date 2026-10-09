#pragma once
// jaal::platform::windows_process — CreateProcessW, with the exit made
// watchable and the tree held in a job object.
//
// The exit handle is the process handle itself: an NT waitable that becomes
// signalled when the process ends and STAYS signalled, which is exactly the
// level-triggered rule in process.hpp. wait_reactor::watch() takes it as is.
//
// Output is an anonymous pipe. The reactor recognises pipe handles and polls
// them with PeekNamedPipe; a failed peek (the writer closed) is hangup.
// Read it with read_some(), which never blocks: it reads only what
// PeekNamedPipe says is there.
//
// The tree is a job object with KILL_ON_JOB_CLOSE. The child is created
// suspended, assigned to the job, then resumed, so it cannot start a
// grandchild outside the job. stop(_, tree) is TerminateJobObject, which a
// grandchild cannot escape, so tree_is_exact() is true.
//
// graceful has no portable meaning for an arbitrary Windows program: there
// is no SIGTERM, and CTRL_BREAK reaches only console children sharing our
// console, which a CREATE_NO_WINDOW child does not. So graceful is treated
// as forceful and a caller's ask-then-insist sequence still ends the child.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "../process.hpp"

namespace jaal::platform {

class windows_process {
  public:
    using handle = native_handle;

    windows_process(windows_process&&) noexcept;
    windows_process& operator=(windows_process&&) noexcept;
    windows_process(const windows_process&)            = delete;
    windows_process& operator=(const windows_process&) = delete;
    /// Kills the job if the child is still running: an owner that drops a
    /// live child is a bug, and a leaked tree is worse than a killed one.
    ~windows_process();

    [[nodiscard]] static result<windows_process> spawn(const process_spec& spec);

    [[nodiscard]] borrowed_handle exit_handle() const noexcept;
    [[nodiscard]] std::optional<borrowed_handle> stdout_handle() const;
    [[nodiscard]] std::optional<borrowed_handle> stderr_handle() const;
    [[nodiscard]] std::optional<borrowed_handle> stdin_handle() const;

    /// Give the child EOF on stdin.
    void close_stdin() noexcept;

    [[nodiscard]] result<void> stop(stop_mode mode, stop_scope scope);
    [[nodiscard]] result<exit_status> reap();

    [[nodiscard]] bool tree_is_exact() const noexcept { return true; }
    [[nodiscard]] std::uint64_t id() const noexcept;

  private:
    windows_process();
    struct impl;
    std::unique_ptr<impl> p_;
};

static_assert(Process<windows_process>,
              "the windows backend must satisfy the same concept as posix and sim");

/// Read what is in a pipe without blocking. Returns the byte count; 0 with
/// `eof` set means the writer closed. Never waits.
[[nodiscard]] std::size_t read_some(borrowed_handle h, char* buf, std::size_t cap,
                                    bool& eof) noexcept;

/// Write without blocking past what the pipe takes. Returns bytes written;
/// `closed` is set when the reader has gone.
[[nodiscard]] std::size_t write_some(borrowed_handle h, const char* buf,
                                     std::size_t len, bool& closed) noexcept;

}  // namespace jaal::platform
