#pragma once
// jaal::platform::owned_handle / borrowed_handle — an OS handle as a linear
// resource.
//
// Every library that talks to the OS grows its own version of this: loom has
// one for Wayland's SCM_RIGHTS descriptors, heddle has another for the GPU,
// shed passes bare ints around. They do not convert, so anything built out of
// several of them spends its time translating, and each translation is a
// place to close a descriptor twice.
//
// The C type is `int` (or `void*` on Windows). It owns nothing, converts from
// anything, and makes double-close and use-after-close invisible. Here the
// owner is move-only and closes exactly once; a borrow can be passed and
// stored but never closed. Which one an API takes is then part of its type:
//
//   result<void> attach(borrowed_handle h);   // I only read it
//   owned_handle take();                      // it's yours now, you close it
//
// NO OS headers: `native_handle` is the same typedef the reactor already
// uses, and the one close() call lives in src/platform/<os>/. So this header
// is as cheap to include as <cstdint>, which is what makes it usable as the
// shared vocabulary type.

#include <cstdint>
#include <utility>

#include "../core/error.hpp"

namespace jaal::platform {

// ── the native type ─────────────────────────────────────────────────────
//
// int on POSIX, HANDLE (void*) on Windows: the same type each reactor
// already declares as `Reactor::handle`, so a handle can be watched without
// a cast. Spelled here without <windows.h>, which is the point.
#if defined(_WIN32)
using native_handle = void*;
inline constexpr native_handle invalid_handle = nullptr;
#else
using native_handle = int;
inline constexpr native_handle invalid_handle = -1;
#endif

[[nodiscard]] constexpr bool is_valid(native_handle h) noexcept {
#if defined(_WIN32)
    // INVALID_HANDLE_VALUE is (HANDLE)-1 as well as null; both mean "no".
    return h != nullptr && h != reinterpret_cast<native_handle>(-1);
#else
    return h >= 0;
#endif
}

/// Close one handle. Implemented per OS in src/platform/<os>/handle.cpp.
/// Never fails in a way a caller can act on: a close error means the
/// descriptor is gone either way, and retrying is how you close someone
/// else's.
void close_handle(native_handle h) noexcept;

/// Duplicate a handle, close-on-exec. For the rare case where a descriptor
/// genuinely needs two owners (a client that keeps the fd it also sent).
[[nodiscard]] result<native_handle> duplicate_handle(native_handle h) noexcept;

// ── borrowed_handle ─────────────────────────────────────────────────────

/// A handle someone else owns. Copyable, comparable, and with no way to
/// close it: the operation that would be a bug is simply absent.
class borrowed_handle {
public:
    borrowed_handle() = default;
    constexpr explicit borrowed_handle(native_handle h) noexcept : h_(h) {}

    [[nodiscard]] constexpr native_handle get() const noexcept { return h_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return is_valid(h_); }
    constexpr explicit operator bool() const noexcept { return valid(); }

    friend constexpr bool operator==(borrowed_handle, borrowed_handle) = default;

private:
    native_handle h_ = invalid_handle;
};

// ── owned_handle ────────────────────────────────────────────────────────

/// The owner: move-only, closed exactly once, in its destructor.
///
/// Move-only is the whole guarantee. A copy would mean two closes of one
/// number, and the second one lands on whatever the OS handed out in the
/// meantime — a bug that shows up as corruption somewhere else entirely.
class owned_handle {
public:
    owned_handle() = default;

    /// Adopt a raw handle. Explicit and greppable: this is the one place
    /// ownership is claimed out of thin air, so it is where you look when a
    /// handle is closed twice.
    constexpr explicit owned_handle(native_handle h) noexcept : h_(h) {}

    owned_handle(owned_handle&& o) noexcept
        : h_(std::exchange(o.h_, invalid_handle)) {}

    owned_handle& operator=(owned_handle&& o) noexcept {
        if (this != &o) {
            reset();
            h_ = std::exchange(o.h_, invalid_handle);
        }
        return *this;
    }

    owned_handle(const owned_handle&) = delete;
    owned_handle& operator=(const owned_handle&) = delete;

    ~owned_handle() { reset(); }

    /// Lend it out. The borrow cannot close it, which is the bug that
    /// actually happens; it can still dangle, which is the ordinary C++
    /// caveat and is why borrows are not stored in long-lived structs.
    [[nodiscard]] constexpr borrowed_handle borrow() const noexcept {
        return borrowed_handle{h_};
    }

    [[nodiscard]] constexpr native_handle get() const noexcept { return h_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return is_valid(h_); }
    constexpr explicit operator bool() const noexcept { return valid(); }

    /// Give the handle away. After this, closing it is the caller's job —
    /// [[nodiscard]] because dropping the result leaks it.
    [[nodiscard]] constexpr native_handle release() noexcept {
        return std::exchange(h_, invalid_handle);
    }

    /// Close now, rather than at the end of the scope.
    void reset() noexcept {
        if (is_valid(h_)) close_handle(h_);
        h_ = invalid_handle;
    }

    /// Two owners for one thing the OS is willing to duplicate.
    [[nodiscard]] result<owned_handle> duplicate() const noexcept {
        if (!valid()) return owned_handle{};
        auto d = duplicate_handle(h_);
        if (!d) return std::unexpected(d.error());
        return owned_handle{*d};
    }

    friend constexpr bool operator==(const owned_handle& a,
                                     const owned_handle& b) noexcept {
        return a.h_ == b.h_;
    }

private:
    native_handle h_ = invalid_handle;
};

}  // namespace jaal::platform

namespace jaal {
using platform::borrowed_handle;
using platform::owned_handle;
}  // namespace jaal
