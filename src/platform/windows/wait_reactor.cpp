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
#include <timeapi.h>   // timeBeginPeriod / timeEndPeriod (winmm)

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

namespace jaal::platform {

namespace {

// Windows' default timer/scheduler granularity is ~15.6 ms, so a
// WaitForMultipleObjects call with a 1 ms timeout actually sleeps a full
// tick — every jaal timer (Sub::every, Cmd::after, frame pacing, debounce)
// quantises to 15 ms, and a 1 ms `every` fires ~12x in 200 ms instead of
// ~200x. timeBeginPeriod(1) raises the global resolution to 1 ms so sub-
// frame timing is honoured, matching the POSIX behaviour the kernel's
// timer rules assume.
//
// It is a PROCESS-WIDE, refcounted setting: the first live reactor raises
// it, the last one to go away lowers it (an unbalanced End would clobber
// another component's request). Never left raised past the reactor's life
// — a permanently high timer resolution costs power on laptops.
std::atomic<int> g_timer_res_refs{0};

void timer_res_acquire() noexcept {
    if (g_timer_res_refs.fetch_add(1, std::memory_order_acq_rel) == 0)
        ::timeBeginPeriod(1);
}
void timer_res_release() noexcept {
    if (g_timer_res_refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        ::timeEndPeriod(1);
}

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

bool pipe_gone(DWORD e) noexcept {
    return e == ERROR_BROKEN_PIPE || e == ERROR_HANDLE_EOF || e == ERROR_PIPE_NOT_CONNECTED
        || e == ERROR_INVALID_HANDLE;
}

// A pipe can't be waited on for data, so each watched pipe gets a thread
// parked in a zero-byte ReadFile. It returns when data or EOF arrives and
// consumes nothing; the thread then sets `ready`, which IS waitable. libuv
// reads non-overlapped pipes the same way. While that read is pending the
// reactor never peeks: on a synchronous handle the peek would queue behind it.
struct pipe_reader {
    HANDLE            pipe   = nullptr;
    HANDLE            ready  = nullptr;   // manual reset: the read returned
    HANDLE            rearm  = nullptr;   // auto reset: read again
    HANDLE            quit   = nullptr;   // manual reset
    HANDLE            thread = nullptr;
    // False once the handle refused a blocking read (overlapped, PIPE_NOWAIT,
    // a socket): the reactor polls it instead.
    std::atomic<bool> can_block{true};
    bool              armed    = false;   // reactor thread only
    std::uint32_t     spurious = 0;       // reactor thread only

    pipe_reader() = default;
    pipe_reader(const pipe_reader&)            = delete;
    pipe_reader& operator=(const pipe_reader&) = delete;

    static DWORD WINAPI run(void* p) {
        auto& r = *static_cast<pipe_reader*>(p);
        const HANDLE w[2] = {r.quit, r.rearm};   // quit first: it wins a tie
        for (;;) {
            if (::WaitForMultipleObjects(2, w, FALSE, INFINITE) != WAIT_OBJECT_0 + 1) return 0;
            char b;
            DWORD n = 0;
            if (!::ReadFile(r.pipe, &b, 0, &n, nullptr)) {
                const DWORD e = ::GetLastError();
                if (!pipe_gone(e) && e != ERROR_OPERATION_ABORTED) {
                    r.can_block.store(false, std::memory_order_release);
                    ::SetEvent(r.ready);
                    return 0;
                }
            }
            ::SetEvent(r.ready);
        }
    }

    static std::unique_ptr<pipe_reader> start(HANDLE h) {
        auto r   = std::make_unique<pipe_reader>();
        r->pipe  = h;
        r->ready = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        r->rearm = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        r->quit  = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!r->ready || !r->rearm || !r->quit) return nullptr;
        r->thread = ::CreateThread(nullptr, 64 * 1024, &run, r.get(),
                                   STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!r->thread) return nullptr;
        return r;
    }

    // Stop the thread and wait for it. A cancel can land before the thread
    // enters ReadFile and do nothing, so it's repeated until the thread exits.
    ~pipe_reader() {
        if (thread) {
            ::SetEvent(quit);
            do { ::CancelSynchronousIo(thread); }
            while (::WaitForSingleObject(thread, 1) == WAIT_TIMEOUT);
            ::CloseHandle(thread);
        }
        if (ready) ::CloseHandle(ready);
        if (rearm) ::CloseHandle(rearm);
        if (quit)  ::CloseHandle(quit);
    }
};

// FILE_PIPE_LOCAL_INFORMATION, for the free space on a pipe's write side.
struct pipe_local_info {
    ULONG type, config, max_instances, cur_instances, inbound_quota, read_available,
          outbound_quota, write_quota_available, state, end;
};
constexpr LONG  kPipeBroken       = static_cast<LONG>(0xC000014B);   // STATUS_PIPE_BROKEN
constexpr ULONG kPipeClosingState = 4;    // FILE_PIPE_CLOSING_STATE: the reader is gone

using query_file_fn = LONG(NTAPI*)(HANDLE, void*, void*, ULONG, int);

query_file_fn query_file() noexcept {
    static const query_file_fn fn = [] {
        FARPROC p = ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile");
        return reinterpret_cast<query_file_fn>(reinterpret_cast<void (*)()>(p));
    }();
    return fn;
}

}  // namespace

struct wait_reactor::state {
    // How a handle's readiness is found out.
    //   waitable  events, processes, console input: signalled satisfies
    //             whatever interest was asked for
    //   stream    a console: readable when signalled, always writable (as
    //             poll() says for a tty)
    //   file      a disk file: always readable and writable, as poll() says
    //   pipe      read via a pipe_reader, write via the pipe's free space
    enum class kind : std::uint8_t { waitable, stream, file, pipe };

    HANDLE wake = nullptr;
    struct slot {
        HANDLE                       h     = nullptr;
        std::uint64_t                token = 0;
        interest                     what  = interest::read;
        kind                         k     = kind::waitable;
        bool                         live  = false;
        std::unique_ptr<pipe_reader> reader;
    };
    std::vector<slot>          slots;
    std::vector<std::uint32_t> free;
    std::uint64_t              os_waits = 0;   // reactor thread only

    state() { timer_res_acquire(); }

    ~state() {
        slots.clear();                        // stop the pipe readers first
        if (wake) ::CloseHandle(wake);
        timer_res_release();
    }

    [[nodiscard]] std::size_t live_count() const noexcept {
        return static_cast<std::size_t>(
            std::count_if(slots.begin(), slots.end(), [](const slot& s) { return s.live; }));
    }

    // A pipe watched only for reading gets a reader thread. One watched for
    // writing too is polled while it is: a pending synchronous read holds
    // the handle's I/O lock, and would block the free-space query (and a
    // write, on a duplex pipe). Write interest is short-lived backpressure.
    // If the thread can't be made, the pipe is polled too.
    static void apply(slot& sl) {
        if (sl.k != kind::pipe) return;
        if (sl.what == interest::read) {
            if (!sl.reader) sl.reader = pipe_reader::start(sl.h);
        } else {
            sl.reader.reset();
        }
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

result<void> wait_reactor::registration::modify(interest what) {
    auto s = s_.lock();
    if (!s || slot_ >= s->slots.size() || !s->slots[slot_].live)
        return std::unexpected(error::make(std::errc::invalid_argument,
                                           "modify: registration is empty"));
    auto& sl = s->slots[slot_];
    sl.what  = what;
    state::apply(sl);
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
template <class State>
result<std::uint32_t> add_slot(State& s, HANDLE h, std::uint64_t token,
                               interest what, typename State::kind k) {
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
    auto& sl  = s.slots[idx];
    sl.h      = h;
    sl.token  = token;
    sl.what   = what;
    sl.k      = k;
    sl.live   = true;
    sl.reader.reset();
    State::apply(sl);
    return idx;
}
}  // namespace

result<wait_reactor::registration>
wait_reactor::watch(handle h, interest what, std::uint64_t token) {
    // ASK THE OS what this handle is rather than assuming it's waitable.
    //
    // A byte-mode pipe is not a readiness object: WaitForMultipleObjects on
    // one does not block until data arrives. watch_pipe() exists for that,
    // but the callers that matter don't know they hold a pipe: maya hands us
    // whatever stdin is, and under mintty/MSYS2 (or `prog.exe < file`, or
    // `type NUL | prog.exe`) that's a pipe, not a console. Those launches
    // hung. GetFileType is one syscall and can't be wrong about this.
    using kind = state::kind;
    kind k = kind::waitable;
    switch (::GetFileType(static_cast<HANDLE>(h))) {
        case FILE_TYPE_PIPE: k = kind::pipe;   break;
        case FILE_TYPE_CHAR: k = kind::stream; break;
        case FILE_TYPE_DISK: k = kind::file;   break;
        default:             k = kind::waitable;
    }
    auto idx = add_slot(*s_, static_cast<HANDLE>(h), token, what, k);
    if (!idx) return std::unexpected(idx.error());
    return registration(std::weak_ptr<state>(s_), *idx);
}

result<wait_reactor::registration>
wait_reactor::watch_pipe(handle h, std::uint64_t token) {
    auto idx = add_slot(*s_, static_cast<HANDLE>(h), token, interest::read, state::kind::pipe);
    if (!idx) return std::unexpected(idx.error());
    return registration(std::weak_ptr<state>(s_), *idx);
}

void wait_reactor::unwatch(state& s, std::uint32_t slot) noexcept {
    if (slot >= s.slots.size() || !s.slots[slot].live) return;
    s.slots[slot].reader.reset();             // its thread is gone before we return
    s.slots[slot].live = false;
    s.free.push_back(slot);
}

std::size_t wait_reactor::watched() const noexcept { return s_->live_count(); }
std::uint64_t wait_reactor::os_waits() const noexcept { return s_->os_waits; }

namespace {

// A pipe's read side, without blocking. Fills `rd` and returns true when
// there's something to report.
template <class Slot>
bool probe_pipe_read(Slot& sl, readiness& rd) {
    pipe_reader* r = sl.reader.get();
    const bool threaded = r && r->can_block.load(std::memory_order_acquire);
    bool fired = false;
    if (threaded && r->armed) {
        if (::WaitForSingleObject(r->ready, 0) != WAIT_OBJECT_0) return false;   // still pending
        r->armed = false;
        fired    = true;
    }
    DWORD avail = 0;
    if (::PeekNamedPipe(sl.h, nullptr, 0, nullptr, &avail, nullptr)) {
        if (avail != 0) {
            if (r) r->spurious = 0;
            rd.readable = true;
            return true;
        }
        if (threaded) {
            // A read that keeps returning with nothing to read would bounce
            // between the two threads forever. Poll that handle instead.
            if (fired && ++r->spurious > 64) {
                r->can_block.store(false, std::memory_order_relaxed);
                return false;
            }
            ::ResetEvent(r->ready);
            ::SetEvent(r->rearm);
            r->armed = true;
        }
        return false;
    }
    // PeekNamedPipe fails once the writer has closed its end: that's EOF,
    // reported as hangup, never a spin.
    if (!pipe_gone(::GetLastError())) return false;   // transient: keep waiting
    rd.readable = true;
    rd.hangup   = true;
    return true;
}

// A pipe's write side: writable while it has free space. There's no event
// for space freeing up, so a backed-up pipe is polled (see wait()).
template <class Slot>
bool probe_pipe_write(const Slot& sl, readiness& rd) {
    pipe_local_info info{};
    ULONG_PTR       iosb[2]{};
    const auto      q = query_file();
    const LONG st = q ? q(sl.h, iosb, &info, sizeof info, 24 /* FilePipeLocalInformation */)
                      : -1;
    if (st == kPipeBroken || (st >= 0 && info.state == kPipeClosingState)) {
        rd.writable = true;                   // a write now fails at once
        rd.hangup   = true;
        rd.error    = true;
        return true;
    }
    if (st < 0) {
        // Can't ask (no read-attributes access). A blocking write just
        // blocks, so saying "writable" can't wedge the host.
        rd.writable = true;
        return true;
    }
    if (info.write_quota_available == 0) return false;
    rd.writable = true;
    return true;
}

}  // namespace

result<wait_result> wait_reactor::wait(std::optional<std::chrono::milliseconds> timeout) {
    auto& s = *s_;
    using kind = state::kind;
    wait_result out;

    // Probe every watched handle once, without blocking. Fills `out`,
    // returns whether anything was ready, and says whether some handle can
    // only be polled (a backed-up pipe write, a pipe with no reader thread).
    bool must_poll = false;
    auto probe = [&]() -> bool {
        bool any = false;
        must_poll = false;
        if (::WaitForSingleObject(s.wake, 0) == WAIT_OBJECT_0) {
            ::ResetEvent(s.wake);            // drain: all pending wakes are one
            out.woken = true;
            any = true;
        }
        for (auto& sl : s.slots) {
            if (!sl.live || out.count >= std::size(out.ready)) continue;
            const bool r = wants_read(sl.what), w = wants_write(sl.what);
            readiness rd{};
            switch (sl.k) {
                case kind::waitable:
                    if (::WaitForSingleObject(sl.h, 0) == WAIT_OBJECT_0) {
                        rd.readable = r;
                        rd.writable = w;
                    }
                    break;
                case kind::stream:
                    rd.readable = r && ::WaitForSingleObject(sl.h, 0) == WAIT_OBJECT_0;
                    rd.writable = w;
                    break;
                case kind::file:
                    rd.readable = r;
                    rd.writable = w;
                    break;
                case kind::pipe: {
                    if (r) {
                        (void)probe_pipe_read(sl, rd);
                        const auto* pr = sl.reader.get();
                        if (!pr || !pr->can_block.load(std::memory_order_acquire))
                            must_poll = true;
                    }
                    if (w && !rd.hangup) {
                        readiness wr{};
                        if (probe_pipe_write(sl, wr)) {
                            rd.writable = true;
                            rd.hangup  |= wr.hangup;
                            rd.error   |= wr.error;
                        } else {
                            must_poll = true;     // no event for "space freed"
                        }
                    }
                    break;
                }
            }
            if (!rd.readable && !rd.writable && !rd.hangup && !rd.error) continue;
            rd.token = sl.token;
            out.ready[out.count++] = rd;
            any = true;
        }
        return any;
    };

    if (probe()) return out;

    const DWORD total = wait_ms(timeout);
    using clk = std::chrono::steady_clock;
    const auto start = clk::now();
    std::vector<HANDLE> hs;
    hs.reserve(s.slots.size() + 1);

    for (;;) {
        // The wait list: the waker, every handle whose signal means
        // something for its interest, and each pipe reader that's armed.
        hs.clear();
        hs.push_back(s.wake);
        for (auto& sl : s.slots) {
            if (!sl.live) continue;
            const bool r = wants_read(sl.what);
            if (sl.k == kind::waitable || (sl.k == kind::stream && r)) {
                hs.push_back(sl.h);
            } else if (sl.k == kind::pipe && r && sl.reader && sl.reader->armed
                       && sl.reader->can_block.load(std::memory_order_acquire)) {
                hs.push_back(sl.reader->ready);
            }
        }

        DWORD slice = total;
        if (total != INFINITE) {
            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   clk::now() - start).count();
            slice = spent >= total ? 0 : total - static_cast<DWORD>(spent);
        }
        // Only a handle that can't signal is polled, and only while it can't.
        constexpr DWORD kPollQuantum = 5;
        if (must_poll) slice = std::min<DWORD>(slice, kPollQuantum);

        ++s.os_waits;
        const DWORD res = ::WaitForMultipleObjects(static_cast<DWORD>(hs.size()), hs.data(),
                                                   FALSE, slice);
        if (res == WAIT_FAILED)
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
