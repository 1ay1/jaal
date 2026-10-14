#pragma once
// jaal::platform::wait_reactor — the Windows reactor.
//
// Waits on NT waitable handles (events, processes, console input) with
// WaitForMultipleObjects, plus a manual-reset event as the waker.
//
// Declared with NO <windows.h>: handles are `void*`, exactly what HANDLE is.
// Implemented in src/platform/windows/.
//
// Carried over from maya's win32 platform code, each one a rule here:
//   * the waker is a MANUAL-reset event, reset only when the reactor drains
//     it. Auto-reset would clear it inside WaitForMultipleObjects, and a
//     wake racing the mailbox's empty-queue check could be lost.
//   * WaitForMultipleObjects reports only the LOWEST signalled index, so
//     after it returns every handle is probed with a zero wait.
//   * the timeout is clamped into DWORD with INFINITE for anything too big.
//     A raw cast of a 64-bit count wraps to a small value (a busy poll).
//   * a byte-mode NAMED PIPE is not a readiness object: waiting on one does
//     not block until data arrives. This is why stdin under mintty/MSYS2
//     busy-spun in maya. A pipe watched for reading gets a small thread
//     parked in a zero-byte ReadFile (it consumes nothing); when that
//     returns, an event the reactor waits on is set. So an idle pipe costs
//     no wakeups. A failed peek is reported as hangup.
//   * interest is honoured: a console or file is always writable, a pipe is
//     writable while it has free space (polled only while that's wanted and
//     the pipe is full, since nothing signals space freeing up), and a plain
//     waitable reports whatever was asked for when it's signalled.
//   * WaitForMultipleObjects takes at most 64 handles. watch() refuses the
//     65th with an error instead of failing every later wait.

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

#include "../../core/error.hpp"
#include "../concepts.hpp"

namespace jaal::platform {

class wait_reactor {
    struct state;

public:
    using handle = void*;                       // HANDLE

    class waker_ref {
    public:
        void wake() const noexcept;             // SetEvent: thread-safe
    private:
        friend class wait_reactor;
        explicit waker_ref(void* ev) noexcept : ev_(ev) {}
        void* ev_ = nullptr;
    };

    class registration {
    public:
        registration() noexcept = default;
        registration(registration&& o) noexcept;
        registration& operator=(registration&& o) noexcept;
        registration(const registration&)            = delete;
        registration& operator=(const registration&) = delete;
        ~registration();

        /// Change what this handle waits for, keeping its token. For a
        /// socket host: add write when a send blocks, drop it when the
        /// buffer drains. A default-constructed (or moved-from)
        /// registration returns std::errc::invalid_argument.
        [[nodiscard]] result<void> modify(interest what);
    private:
        friend class wait_reactor;
        registration(std::weak_ptr<state> s, std::uint32_t slot) noexcept
            : s_(std::move(s)), slot_(slot) {}
        void release() noexcept;
        std::weak_ptr<state> s_;
        std::uint32_t        slot_ = 0;
    };

    /// Max handles, the WaitForMultipleObjects limit minus the waker.
    static constexpr std::size_t max_watched = 63;

    [[nodiscard]] static result<wait_reactor> create();

    wait_reactor(wait_reactor&&) noexcept;
    wait_reactor& operator=(wait_reactor&&) noexcept;
    wait_reactor(const wait_reactor&)            = delete;
    wait_reactor& operator=(const wait_reactor&) = delete;
    ~wait_reactor();

    /// Watch a handle. The kind is asked of the OS: a pipe gets a reader
    /// thread (see above), a console or file reports writable at once, and
    /// anything else (event, process, console input) is waited on directly.
    [[nodiscard]] result<registration> watch(handle h, interest what, std::uint64_t token);

    /// Force the pipe path for read interest. watch() finds pipes by itself.
    [[nodiscard]] result<registration> watch_pipe(handle h, std::uint64_t token);

    [[nodiscard]] result<wait_result> wait(std::optional<std::chrono::milliseconds> timeout);
    [[nodiscard]] waker_ref           waker() const noexcept;
    [[nodiscard]] std::size_t         watched() const noexcept;

    /// How many times wait() has blocked in the OS, over this reactor's
    /// life. For tests: an idle wait is one call, not a poll loop.
    [[nodiscard]] std::uint64_t       os_waits() const noexcept;

private:
    explicit wait_reactor(std::shared_ptr<state> s) noexcept;
    static void unwatch(state& s, std::uint32_t slot) noexcept;
    std::shared_ptr<state> s_;
};

static_assert(Reactor<wait_reactor>);

}  // namespace jaal::platform
