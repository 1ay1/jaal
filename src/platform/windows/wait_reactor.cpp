// jaal::platform::wait_reactor — implementation. The only file that sees
// <windows.h> for this backend.

#include <jaal/platform/windows/wait_reactor.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <vector>

namespace jaal::platform {

namespace {

// Clamp a chrono timeout into WaitForMultipleObjects' DWORD. "No deadline"
// and anything too big both become INFINITE. A raw cast would wrap a large
// count to a small one, and the wait would return almost at once (maya hit
// this as a busy poll).
DWORD wait_ms(std::optional<std::chrono::milliseconds> t) noexcept {
    if (!t) return INFINITE;
    const auto c = t->count();
    if (c <= 0) return 0;
    if (c >= static_cast<long long>(INFINITE)) return INFINITE;
    return static_cast<DWORD>(c);
}

}  // namespace

struct wait_reactor::state {
    HANDLE wake = nullptr;
    struct slot {
        HANDLE        h     = nullptr;
        std::uint64_t token = 0;
        bool          pipe  = false;
        bool          live  = false;
    };
    std::vector<slot>          slots;
    std::vector<std::uint32_t> free;

    ~state() {
        if (wake) ::CloseHandle(wake);
    }

    [[nodiscard]] std::size_t live_count() const noexcept {
        return static_cast<std::size_t>(
            std::count_if(slots.begin(), slots.end(), [](const slot& s) { return s.live; }));
    }
};

// ── waker ───────────────────────────────────────────────────────────────
void wait_reactor::waker_ref::wake() const noexcept {
    if (ev_) ::SetEvent(static_cast<HANDLE>(ev_));
}

// ── registration ────────────────────────────────────────────────────────
void wait_reactor::registration::release() noexcept {
    if (auto s = s_.lock()) wait_reactor::unwatch(*s, slot_);
    s_.reset();
}
wait_reactor::registration::registration(registration&& o) noexcept
    : s_(std::move(o.s_)), slot_(o.slot_) {
    o.s_.reset();
}
wait_reactor::registration& wait_reactor::registration::operator=(registration&& o) noexcept {
    if (this != &o) {
        release();
        s_    = std::move(o.s_);
        slot_ = o.slot_;
        o.s_.reset();
    }
    return *this;
}
wait_reactor::registration::~registration() { release(); }

result<void> wait_reactor::registration::modify(interest) {
    auto s = s_.lock();
    if (!s || slot_ >= s->slots.size() || !s->slots[slot_].live)
        return std::unexpected(error::make(std::errc::invalid_argument,
                                           "modify: registration is empty"));
    // WaitForMultipleObjects signals on a handle's own state; there is no
    // read/write interest to set (watch() ignores it too). Accepting the
    // call keeps host code portable: a socket host that adds write interest
    // on EAGAIN compiles and runs here, and simply keeps being woken.
    return {};
}

// ── reactor ─────────────────────────────────────────────────────────────
wait_reactor::wait_reactor(std::shared_ptr<state> s) noexcept : s_(std::move(s)) {}
wait_reactor::wait_reactor(wait_reactor&&) noexcept            = default;
wait_reactor& wait_reactor::operator=(wait_reactor&&) noexcept = default;
wait_reactor::~wait_reactor()                                  = default;

result<wait_reactor> wait_reactor::create() {
    // Manual reset: stays signalled until wait() drains it (see header).
    HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ev)   // CreateEventW returns NULL on failure, not INVALID_HANDLE_VALUE
        return std::unexpected(error{std::errc::io_error,
                                     static_cast<std::int32_t>(::GetLastError()),
                                     "CreateEventW for reactor wake"});
    auto s  = std::make_shared<state>();
    s->wake = ev;
    return wait_reactor(std::move(s));
}

wait_reactor::waker_ref wait_reactor::waker() const noexcept {
    return waker_ref(s_ ? s_->wake : nullptr);
}

namespace {
result<std::uint32_t> add_slot(auto& s, HANDLE h, std::uint64_t token, bool pipe) {
    if (!h || h == INVALID_HANDLE_VALUE)
        return std::unexpected(error::make(std::errc::bad_file_descriptor, "watch: bad handle"));
    for (auto& sl : s.slots)
        if (sl.live && sl.h == h)
            return std::unexpected(error::make(std::errc::file_exists,
                                               "watch: handle already watched by this reactor"));
    if (s.live_count() >= wait_reactor::max_watched)
        return std::unexpected(error::make(std::errc::too_many_files_open,
                                           "watch: WaitForMultipleObjects takes at most 64 handles"));
    std::uint32_t idx;
    if (!s.free.empty()) {
        idx = s.free.back();
        s.free.pop_back();
    } else {
        idx = static_cast<std::uint32_t>(s.slots.size());
        s.slots.emplace_back();
    }
    s.slots[idx] = {h, token, pipe, true};
    return idx;
}
}  // namespace

result<wait_reactor::registration>
wait_reactor::watch(handle h, interest, std::uint64_t token) {
    auto idx = add_slot(*s_, static_cast<HANDLE>(h), token, false);
    if (!idx) return std::unexpected(idx.error());
    return registration(std::weak_ptr<state>(s_), *idx);
}

result<wait_reactor::registration>
wait_reactor::watch_pipe(handle h, std::uint64_t token) {
    auto idx = add_slot(*s_, static_cast<HANDLE>(h), token, true);
    if (!idx) return std::unexpected(idx.error());
    return registration(std::weak_ptr<state>(s_), *idx);
}

void wait_reactor::unwatch(state& s, std::uint32_t slot) noexcept {
    if (slot >= s.slots.size() || !s.slots[slot].live) return;
    s.slots[slot].live = false;
    s.free.push_back(slot);
}

std::size_t wait_reactor::watched() const noexcept { return s_->live_count(); }

result<wait_result> wait_reactor::wait(std::optional<std::chrono::milliseconds> timeout) {
    auto& s = *s_;
    wait_result out;

    // Probe every watched handle once (no wait). Pipes by PeekNamedPipe;
    // waitables by a zero-timeout wait. Fills `out`, returns whether
    // anything was ready.
    auto probe = [&]() -> bool {
        bool any = false;
        if (::WaitForSingleObject(s.wake, 0) == WAIT_OBJECT_0) {
            ::ResetEvent(s.wake);            // drain: all pending wakes are one
            out.woken = true;
            any = true;
        }
        for (auto& sl : s.slots) {
            if (!sl.live || out.count >= std::size(out.ready)) continue;
            if (sl.pipe) {
                DWORD avail = 0;
                if (::PeekNamedPipe(sl.h, nullptr, 0, nullptr, &avail, nullptr)) {
                    if (avail == 0) {
                        // Empty, but is the writer still there? PeekNamedPipe
                        // reports SUCCESS with avail == 0 for BOTH "open and
                        // idle" and "already at EOF", so this branch can't
                        // just `continue` -- on a pipe that is empty and
                        // closed we would wait forever for bytes that can
                        // never come. That is the `type NUL | prog.exe` hang:
                        // cmd.exe hands over an ANONYMOUS pipe, writes
                        // nothing, and closes. The failing-peek branch below
                        // never fires because the peek keeps succeeding.
                        //
                        // A zero-byte ReadFile distinguishes them without
                        // consuming anything: on a live pipe it returns TRUE
                        // (nothing to do), on a closed one it fails with
                        // ERROR_BROKEN_PIPE / ERROR_HANDLE_EOF.
                        DWORD got = 0;
                        if (::ReadFile(sl.h, nullptr, 0, &got, nullptr))
                            continue;                 // open and idle: keep waiting
                        const DWORD e = ::GetLastError();
                        if (e != ERROR_BROKEN_PIPE && e != ERROR_HANDLE_EOF)
                            continue;                 // some other transient: keep waiting
                        auto& rd = out.ready[out.count++];
                        rd.token = sl.token;
                        rd.readable = true;
                        rd.hangup = true;
                        any = true;
                        continue;
                    }
                    auto& rd = out.ready[out.count++];
                    rd.token = sl.token;
                    rd.readable = true;
                } else {
                    // The writer closed its end: report it, never spin.
                    auto& rd = out.ready[out.count++];
                    rd.token = sl.token;
                    rd.readable = true;
                    rd.hangup = true;
                }
                any = true;
            } else if (::WaitForSingleObject(sl.h, 0) == WAIT_OBJECT_0) {
                auto& rd = out.ready[out.count++];
                rd.token = sl.token;
                rd.readable = true;
                any = true;
            }
        }
        return any;
    };

    if (probe()) return out;

    const bool has_pipe = std::any_of(s.slots.begin(), s.slots.end(),
                                      [](const state::slot& sl) { return sl.live && sl.pipe; });
    const DWORD total = wait_ms(timeout);
    using clk = std::chrono::steady_clock;
    const auto start = clk::now();

    // Build the wait list: the waker first, then every waitable (not pipes).
    std::vector<HANDLE> hs;
    hs.reserve(s.slots.size() + 1);
    hs.push_back(s.wake);
    for (auto& sl : s.slots)
        if (sl.live && !sl.pipe) hs.push_back(sl.h);

    for (;;) {
        // With a pipe in the set, block in short slices so PeekNamedPipe is
        // re-checked; without one, block for the whole remaining time.
        DWORD slice = total;
        if (total != INFINITE) {
            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   clk::now() - start).count();
            slice = spent >= total ? 0 : total - static_cast<DWORD>(spent);
        }
        constexpr DWORD kPipeQuantum = 5;
        if (has_pipe) slice = std::min<DWORD>(slice, kPipeQuantum);

        const DWORD r = ::WaitForMultipleObjects(static_cast<DWORD>(hs.size()), hs.data(),
                                                 FALSE, slice);
        if (r == WAIT_FAILED)
            return std::unexpected(error{std::errc::io_error,
                                         static_cast<std::int32_t>(::GetLastError()),
                                         "WaitForMultipleObjects"});
        // Whatever woke us (or not), probe each handle: WFMO only names the
        // lowest signalled index.
        if (probe()) return out;

        if (total != INFINITE) {
            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   clk::now() - start).count();
            if (spent >= static_cast<long long>(total)) {
                out.timeout = true;
                return out;
            }
        }
    }
}

}  // namespace jaal::platform
