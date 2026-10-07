// src/platform/windows/file_lock.cpp — see
// include/jaal/platform/windows/file_lock.hpp

#include "jaal/platform/windows/file_lock.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <utility>

namespace jaal::platform {

struct windows_file_lock::impl {
    HANDLE      h    = INVALID_HANDLE_VALUE;
    bool        held = false;
    std::string sidecar;

    ~impl() {
        if (h != INVALID_HANDLE_VALUE) {
            if (held) {
                OVERLAPPED ov{};
                (void)::UnlockFileEx(h, 0, 1, 0, &ov);
            }
            (void)::CloseHandle(h);
        }
    }
};

namespace {

// FILE_SHARE_READ|WRITE so other instances can OPEN the sidecar — the
// exclusion must come from the byte-range lock, not from the open. Without
// the share flags the second process fails at CreateFile and never reaches
// LockFileEx, which looks like a lock but reports the wrong error and never
// waits.
result<HANDLE> open_sidecar(const std::string& sidecar) {
    const HANDLE h = ::CreateFileA(
        sidecar.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return std::unexpected(error{std::errc::io_error,
                                     static_cast<std::int32_t>(::GetLastError()),
                                     "jaal: cannot open lock sidecar"});
    return h;
}

result<void> lock_region(HANDLE h, lock_mode m, bool immediate) {
    DWORD flags = (m == lock_mode::exclusive) ? LOCKFILE_EXCLUSIVE_LOCK : 0u;
    if (immediate) flags |= LOCKFILE_FAIL_IMMEDIATELY;
    OVERLAPPED ov{};
    if (::LockFileEx(h, flags, 0, 1, 0, &ov)) return {};
    return std::unexpected(error{std::errc::io_error,
                                 static_cast<std::int32_t>(::GetLastError()),
                                 "jaal: cannot lock sidecar"});
}

}  // namespace

windows_file_lock::windows_file_lock() noexcept : p_(nullptr) {}
windows_file_lock::windows_file_lock(windows_file_lock&&) noexcept = default;
windows_file_lock& windows_file_lock::operator=(windows_file_lock&&) noexcept =
    default;
windows_file_lock::~windows_file_lock() = default;

result<windows_file_lock> windows_file_lock::acquire(const std::string& target,
                                                     lock_mode          m) {
    const std::string sidecar = lock_sidecar_for(target);
    auto              h       = open_sidecar(sidecar);
    if (!h) return std::unexpected(h.error());

    auto p     = std::make_unique<impl>();
    p->h       = *h;
    p->sidecar = sidecar;

    auto locked = lock_region(p->h, m, /*immediate=*/false);
    if (!locked) return std::unexpected(locked.error());

    p->held = true;
    windows_file_lock out;
    out.p_ = std::move(p);
    return out;
}

result<std::optional<windows_file_lock>> windows_file_lock::try_acquire(
    const std::string& target, lock_mode m) {
    const std::string sidecar = lock_sidecar_for(target);
    auto              h       = open_sidecar(sidecar);
    if (!h) return std::unexpected(h.error());

    auto p     = std::make_unique<impl>();
    p->h       = *h;
    p->sidecar = sidecar;

    auto locked = lock_region(p->h, m, /*immediate=*/true);
    if (!locked) {
        // Busy is an outcome; anything else is this volume saying it cannot
        // lock, which the caller must be able to tell apart.
        const DWORD e = static_cast<DWORD>(locked.error().native);
        if (e == ERROR_LOCK_VIOLATION || e == ERROR_IO_PENDING)
            return std::optional<windows_file_lock>{};
        return std::unexpected(locked.error());
    }

    p->held = true;
    windows_file_lock out;
    out.p_ = std::move(p);
    return std::optional<windows_file_lock>{std::move(out)};
}

bool windows_file_lock::held() const noexcept { return p_ && p_->held; }

void windows_file_lock::release() noexcept {
    if (!p_ || !p_->held) return;  // idempotent
    OVERLAPPED ov{};
    (void)::UnlockFileEx(p_->h, 0, 1, 0, &ov);
    p_->held = false;
}

const std::string& windows_file_lock::path() const noexcept {
    static const std::string none;
    return p_ ? p_->sidecar : none;
}

}  // namespace jaal::platform
