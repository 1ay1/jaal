// jaal::platform::console_signals — implementation.

#include <jaal/platform/windows/console_signals.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace jaal::platform {

namespace {

constexpr int kSlots = 8;

struct slot {
    std::atomic<void*>    ev{nullptr};   // published event, nullptr when not
    std::atomic<unsigned> wants{0};
    std::atomic<unsigned> pending{0};
    HANDLE                owned = nullptr;   // guarded by g_mu
    bool                  used  = false;     // guarded by g_mu
};

slot             g_slots[kSlots];
std::atomic<int> g_in_handler{0};
std::mutex       g_mu;
int              g_sources = 0;          // installed sources; handler added at 1

int map_ctrl(DWORD type) noexcept {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:    return static_cast<int>(sig::interrupt);
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT: return static_cast<int>(sig::terminate);
        case CTRL_CLOSE_EVENT:    return static_cast<int>(sig::hangup);
        default:                  return -1;
    }
}

BOOL WINAPI on_ctrl(DWORD type) {
    const int idx = map_ctrl(type);
    if (idx < 0) return FALSE;            // not ours: let the next handler see it
    const unsigned b = 1u << static_cast<unsigned>(idx);
    g_in_handler.fetch_add(1, std::memory_order_acq_rel);
    bool handled = false;
    for (auto& s : g_slots) {
        if (!(s.wants.load(std::memory_order_acquire) & b)) continue;
        void* ev = s.ev.load(std::memory_order_acquire);
        if (!ev) continue;
        s.pending.fetch_or(b, std::memory_order_acq_rel);   // bit before event
        ::SetEvent(static_cast<HANDLE>(ev));
        handled = true;
    }
    g_in_handler.fetch_sub(1, std::memory_order_acq_rel);
    // TRUE only if some source took it. Otherwise the default action runs
    // (Ctrl+C ends the process), which is what an app without a source for
    // `interrupt` expects.
    return handled ? TRUE : FALSE;
}

}  // namespace

result<console_signals> console_signals::install(signal_set wanted) {
    std::lock_guard lk(g_mu);
    int idx = -1;
    for (int i = 0; i < kSlots; ++i)
        if (!g_slots[i].used) { idx = i; break; }
    if (idx < 0)
        return std::unexpected(error::make(std::errc::too_many_files_open,
                                           "signals: more than 8 sources installed"));
    HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual reset
    if (!ev)
        return std::unexpected(error{std::errc::io_error,
                                     static_cast<std::int32_t>(::GetLastError()),
                                     "CreateEventW for signals"});
    if (g_sources == 0 && !::SetConsoleCtrlHandler(on_ctrl, TRUE)) {
        const auto e = ::GetLastError();
        ::CloseHandle(ev);
        return std::unexpected(error{std::errc::io_error, static_cast<std::int32_t>(e),
                                     "SetConsoleCtrlHandler"});
    }
    ++g_sources;

    // Only the signals that console control events can deliver.
    const signal_set deliverable{sig::interrupt, sig::terminate, sig::hangup};
    const signal_set got = wanted & deliverable;

    auto& s = g_slots[idx];
    s.used  = true;
    s.owned = ev;
    s.pending.store(0, std::memory_order_release);
    s.wants.store(got.bits(), std::memory_order_release);
    s.ev.store(ev, std::memory_order_release);          // publish last
    return console_signals(idx, got);
}

void console_signals::release() noexcept {
    if (slot_ < 0) return;
    std::lock_guard lk(g_mu);
    auto& s = g_slots[slot_];
    s.wants.store(0, std::memory_order_release);
    s.ev.store(nullptr, std::memory_order_release);     // unpublish
    // Wait out handler runs that loaded the event before we unpublished it,
    // so SetEvent can't hit a closed (and reused) handle. Same reasoning as
    // the POSIX source.
    while (g_in_handler.load(std::memory_order_acquire) != 0) std::this_thread::yield();
    if (--g_sources == 0) ::SetConsoleCtrlHandler(on_ctrl, FALSE);
    if (s.owned) ::CloseHandle(s.owned);
    s.owned = nullptr;
    s.pending.store(0, std::memory_order_release);
    s.used = false;
    slot_  = -1;
}

console_signals::console_signals(console_signals&& o) noexcept
    : slot_(std::exchange(o.slot_, -1)), got_(o.got_) {}

console_signals& console_signals::operator=(console_signals&& o) noexcept {
    if (this != &o) {
        release();
        slot_ = std::exchange(o.slot_, -1);
        got_  = o.got_;
    }
    return *this;
}

console_signals::~console_signals() { release(); }

console_signals::native_handle console_signals::handle() const noexcept {
    if (slot_ < 0) return nullptr;
    std::lock_guard lk(g_mu);
    return g_slots[slot_].owned;
}

signal_set console_signals::take() noexcept {
    if (slot_ < 0) return {};
    auto& s = g_slots[slot_];
    // Reset the event FIRST, then take the bits. The handler sets the bit
    // before signalling, so a control event that lands in between leaves
    // its bit AND a signalled event: at most one spurious wakeup, never a
    // lost one. (The other order could clear the event after a new bit was
    // set, and that bit would sit unseen until the next unrelated wakeup.)
    HANDLE ev;
    {
        std::lock_guard lk(g_mu);
        ev = s.owned;
    }
    if (ev) ::ResetEvent(ev);
    const auto bits = s.pending.exchange(0, std::memory_order_acq_rel);
    return signal_set::from_bits(static_cast<std::uint8_t>(bits));
}

namespace test {
// Deliver a control event through the real handler, as Windows would (on
// another thread). The real path, GenerateConsoleCtrlEvent, needs an
// attached console: under wine it fails (tried: ERROR_INVALID_ACCESS), and
// CI runners often have none. So the tests exercise jaal's handler, mapping,
// per-source delivery and reset ordering, but NOT Windows actually calling
// the handler. That needs a run on real Windows with a console.
void console_ctrl(unsigned long type) { on_ctrl(type); }
}  // namespace test

}  // namespace jaal::platform
