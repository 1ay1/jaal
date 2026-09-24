// jaal::platform::posix_signals — implementation.

#include <jaal/platform/posix/signals.hpp>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <mutex>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace jaal::platform {

namespace {

constexpr int kSlots = 8;

constexpr int native_of(unsigned i) noexcept {
    switch (i) {
        case 0: return SIGINT;
        case 1: return SIGTERM;
        case 2: return SIGHUP;
        case 3: return SIGWINCH;
        case 4: return SIGCHLD;
        default: return 0;
    }
}

int index_of(int sig) noexcept {
    for (unsigned i = 0; i < signal_count; ++i)
        if (native_of(i) == sig) return static_cast<int>(i);
    return -1;
}

// Everything the handler touches: lock-free atomics only. The C++ standard
// allows lock-free atomic operations in a signal handler ([support.signal]).
struct slot {
    std::atomic<int>      fd{-1};        // write end, -1 when not published
    std::atomic<unsigned> wants{0};      // bit i: this slot wants signal i
    std::atomic<unsigned> pending{0};    // bit i: signal i arrived, not taken
    int                   read_fd = -1;  // loop-side only, guarded by g_mu
    bool                  used = false;  // guarded by g_mu
};
static_assert(std::atomic<int>::is_always_lock_free);
static_assert(std::atomic<unsigned>::is_always_lock_free);

slot             g_slots[kSlots];
std::atomic<int> g_in_handler{0};

}  // namespace

// Test-only: extra spin iterations between loading a slot's fd and writing
// to it. Widens the race that teardown has to survive, so a test can
// actually hit it instead of hoping. Zero (no effect) unless a test sets it.
namespace test { std::atomic<int> signals_window{0}; }

namespace {

// Loop-side bookkeeping: only touched under g_mu, never by the handler.
std::mutex       g_mu;
int              g_refs[signal_count] = {};        // slots wanting signal i
bool             g_installed[signal_count] = {};   // our handler is in place
struct sigaction g_prev[signal_count];             // what we replaced

extern "C" void on_signal(int sig) {
    const int saved_errno = errno;
    g_in_handler.fetch_add(1, std::memory_order_acq_rel);
    const int idx = index_of(sig);
    if (idx >= 0) {
        const unsigned b = 1u << static_cast<unsigned>(idx);
        for (auto& s : g_slots) {
            if (!(s.wants.load(std::memory_order_acquire) & b)) continue;
            const int fd = s.fd.load(std::memory_order_acquire);
            if (fd < 0) continue;
            for (int spin = test::signals_window.load(std::memory_order_relaxed);
                 spin > 0; --spin)
                std::atomic_signal_fence(std::memory_order_seq_cst);
            s.pending.fetch_or(b, std::memory_order_acq_rel);
            // Bit BEFORE byte. take() drains bytes then clears bits, so any
            // byte it sees has its bit already set: a signal can cause at
            // most one spurious wakeup, never a lost one.
            [[maybe_unused]] auto n = ::write(fd, "s", 1);   // EAGAIN: already pending
        }
    }
    g_in_handler.fetch_sub(1, std::memory_order_acq_rel);
    errno = saved_errno;
}

// Is signal i currently ignored by inheritance (nohup, a background job)?
bool inherited_ignore(unsigned i) {
    struct sigaction cur {};
    ::sigaction(native_of(i), nullptr, &cur);
    return cur.sa_handler == SIG_IGN;
}

bool add_handler(unsigned i) {                  // with g_mu held
    if (g_refs[i]++ > 0) return true;
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | (native_of(i) == SIGCHLD ? SA_NOCLDSTOP : 0);
    if (::sigaction(native_of(i), &sa, &g_prev[i]) != 0) {
        --g_refs[i];
        return false;
    }
    g_installed[i] = true;
    return true;
}

void drop_handler(unsigned i) {                 // with g_mu held
    if (--g_refs[i] > 0) return;
    if (g_installed[i]) {
        ::sigaction(native_of(i), &g_prev[i], nullptr);   // restore what was there
        g_installed[i] = false;
    }
}

}  // namespace

// ── install ─────────────────────────────────────────────────────────────
result<posix_signals> posix_signals::install(signal_set wanted) {
    std::lock_guard lk(g_mu);
    int idx = -1;
    for (int i = 0; i < kSlots; ++i)
        if (!g_slots[i].used) { idx = i; break; }
    if (idx < 0)
        return std::unexpected(error::make(std::errc::too_many_files_open,
                                           "signals: more than 8 sources installed"));
    int p[2];
    if (::pipe(p) != 0) return std::unexpected(error::from_errno(errno, "pipe for signals"));
    for (int fd : p) {
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    auto& s = g_slots[idx];
    s.used    = true;
    s.read_fd = p[0];
    s.pending.store(0, std::memory_order_release);

    signal_set got;
    for (unsigned i = 0; i < signal_count; ++i) {
        const auto sg = static_cast<signal>(i);
        if (!wanted.contains(sg)) continue;
        // nohup / background jobs: an inherited SIG_IGN stays ignored,
        // unless we already own the handler (then it isn't inherited).
        if (!g_installed[i] && inherited_ignore(i)) continue;
        if (!add_handler(i)) continue;
        got.add(sg);
    }
    // Publish last: from here the handler may write to this slot.
    s.wants.store(got.bits(), std::memory_order_release);
    s.fd.store(p[1], std::memory_order_release);
    return posix_signals(idx, got);
}

// ── teardown ────────────────────────────────────────────────────────────
void posix_signals::release() noexcept {
    if (slot_ < 0) return;
    std::lock_guard lk(g_mu);
    auto& s = g_slots[slot_];
    // 1. unpublish: new handler runs skip this slot
    s.wants.store(0, std::memory_order_release);
    const int wfd = s.fd.exchange(-1, std::memory_order_acq_rel);
    // 2. drop our share of each handler; the last one restores the old one
    for (unsigned i = 0; i < signal_count; ++i)
        if (got_.contains(static_cast<signal>(i))) drop_handler(i);
    // 3. wait out handler runs that may have loaded the old fd before step 1.
    //    Any run that loaded it counted itself in first; any run that starts
    //    now sees -1. Bounded in practice: a handler is a handful of
    //    instructions. (A signal storm could keep this >0 indefinitely; that
    //    would already be a stuck program.)
    while (g_in_handler.load(std::memory_order_acquire) != 0) std::this_thread::yield();
    // 4. now nothing can write to wfd: close it
    if (wfd >= 0) ::close(wfd);
    if (s.read_fd >= 0) ::close(s.read_fd);
    s.read_fd = -1;
    s.pending.store(0, std::memory_order_release);
    s.used = false;
    slot_  = -1;
}

posix_signals::posix_signals(posix_signals&& o) noexcept
    : slot_(std::exchange(o.slot_, -1)), got_(o.got_) {}

posix_signals& posix_signals::operator=(posix_signals&& o) noexcept {
    if (this != &o) {
        release();
        slot_ = std::exchange(o.slot_, -1);
        got_  = o.got_;
    }
    return *this;
}

posix_signals::~posix_signals() { release(); }

posix_signals::native_handle posix_signals::handle() const noexcept {
    return slot_ < 0 ? -1 : g_slots[slot_].read_fd;
}

signal_set posix_signals::watching() const noexcept { return got_; }

signal_set posix_signals::take() noexcept {
    if (slot_ < 0) return {};
    auto& s = g_slots[slot_];
    // Drain FIRST, then clear the bits. A signal landing between the two
    // leaves its byte in the pipe and its bit set: we return the bit now and
    // the next wait wakes once for nothing, which is harmless. The other
    // order can leave a set bit with an empty pipe: a signal nobody sees.
    char buf[64];
    while (::read(s.read_fd, buf, sizeof buf) > 0) {}
    const auto bits = s.pending.exchange(0, std::memory_order_acq_rel);
    return signal_set::from_bits(static_cast<std::uint8_t>(bits));
}

}  // namespace jaal::platform
