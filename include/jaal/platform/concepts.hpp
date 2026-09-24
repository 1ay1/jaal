#pragma once
// jaal::platform — the capability concepts every backend implements.
//
// A platform is a bundle of capabilities (docs/design.md §6). Each is a concept
// here; each has several backends (posix/, linux/, darwin/, windows/, sim/);
// every backend passes the same conformance suite
// (tests/platform/conformance.cpp).
//
// This header has NO OS includes. Backends declare their types with plain
// handle integers/pointers; the OS headers live only in src/platform/<os>/.
//
// Lessons carried over from maya's platform code, each a rule here:
//   * a wait timeout is clamped into the syscall's range before the cast:
//     int(64-bit ms) can wrap negative, and poll() reads -1 as "forever"
//   * wake handles coalesce: many wake() before one drain() is one wakeup
//   * a closed/hung-up handle is REPORTED, never a silent spin on read()=0
//   * EINTR never ends a wait early; the backend retries with the time left

#include <chrono>
#include <concepts>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

#include "../core/error.hpp"
#include "clock.hpp"

namespace jaal::platform {

// ── interests and readiness ─────────────────────────────────────────────
enum class interest : std::uint8_t { read = 1, write = 2, read_write = 3 };

[[nodiscard]] constexpr bool wants_read(interest i) noexcept {
    return (static_cast<std::uint8_t>(i) & 1u) != 0;
}
[[nodiscard]] constexpr bool wants_write(interest i) noexcept {
    return (static_cast<std::uint8_t>(i) & 2u) != 0;
}

/// What happened to one watched handle.
struct readiness {
    std::uint64_t token    = 0;      // what the caller registered it with
    bool          readable = false;
    bool          writable = false;
    bool          hangup   = false;  // peer closed / terminal gone
    bool          error    = false;
};

/// The result of one wait().
struct wait_result {
    bool         woken   = false;    // the waker fired (drained for you)
    bool         timeout = false;    // the deadline passed, nothing ready
    std::uint8_t count   = 0;        // how many entries of `ready` are set
    readiness    ready[16]{};        // bounded: callers loop if they need more
};

// ── Reactor ─────────────────────────────────────────────────────────────
//
// Waits on registered handles plus its own waker until something is ready,
// the waker fires, or the deadline passes.
//
//   watch(handle, interest, token)  → registration (RAII: dropping it unwatches)
//   reg.modify(interest)            → change what that handle waits for
//   wake()                          → thread-safe, async-signal-safe, coalescing
//   wait(timeout_ms)                → wait_result
//
// timeout: nullopt = wait indefinitely; 0 = poll without blocking.
//
// modify() is on the registration because that's what owns the handle's
// place in the reactor. It's the difference between a toy and a real socket
// host: a write that returns EAGAIN has to start waiting for writability
// and stop again once the buffer drains, and doing that by dropping and
// re-watching costs two syscalls and loses any readiness in between. The
// token never changes (the host's map from token to connection stays put);
// only the interest does.
//
// ONE registration per handle. Watching a handle that's already watched
// fails with std::errc::file_exists on every backend. epoll can't hold one
// fd twice, and letting poll/WFMO allow it would make programs that work
// on one OS fail on another.
template <class R>
concept Reactor =
    requires {
        typename R::handle;          // native handle type (int fd, HANDLE, ...)
        typename R::registration;    // move-only; destructor unwatches
    }
    && std::movable<typename R::registration>
    && !std::copyable<typename R::registration>
    && requires(R& r, const R& cr, typename R::handle h, interest i, std::uint64_t tok,
                std::optional<std::chrono::milliseconds> t) {
        { R::create() } -> std::same_as<result<R>>;
        { r.watch(h, i, tok) } -> std::same_as<result<typename R::registration>>;
        { r.wait(t) } -> std::same_as<result<wait_result>>;
        { cr.waker() } -> std::same_as<typename R::waker_ref>;
    }
    && requires(typename R::registration& reg, interest i) {
        { reg.modify(i) } -> std::same_as<result<void>>;
    }
    && requires(const typename R::waker_ref& w) {
        { w.wake() } noexcept;
    };

}  // namespace jaal::platform
