// jaal::platform::kqueue_reactor — implementation. Run on macOS (arm64)
// against the full conformance suite.

#include <jaal/platform/darwin/kqueue_reactor.hpp>

#include <algorithm>
#include <cerrno>
#include <ctime>
#include <vector>

#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

namespace jaal::platform {

namespace {

// EVFILT_USER ident for the waker; registrations use their slot + 1 as the
// udata, so 0 never names a registration.
constexpr std::uintptr_t kWakeIdent = 1;

// Longest single kevent() wait. Darwin's kevent rejects a timespec whose
// tv_sec exceeds 100'000'000 with EINVAL (found on the first real-hardware
// run: wait(milliseconds::max()) failed instead of blocking). So a longer
// timeout is waited in chunks of at most one day; wait() loops until the
// caller's full timeout has elapsed. A negative count means "don't block".
constexpr std::int64_t kMaxChunkMs = 24LL * 60 * 60 * 1000;

bool to_timespec(std::optional<std::chrono::milliseconds> t, timespec& ts) noexcept {
    if (!t) return false;                        // nullptr timeout: wait forever
    std::int64_t c = t->count();
    if (c < 0) c = 0;
    if (c > kMaxChunkMs) c = kMaxChunkMs;
    ts.tv_sec  = static_cast<time_t>(c / 1000);
    ts.tv_nsec = static_cast<long>((c % 1000) * 1'000'000);
    return true;
}

}  // namespace

struct kqueue_reactor::state {
    int kq = -1;
    struct slot {
        int           fd = -1;
        interest      what{};
        std::uint64_t token = 0;
        bool          live  = false;
    };
    std::vector<slot>          slots;
    std::vector<std::uint32_t> free;

    ~state() {
        if (kq >= 0) ::close(kq);
    }
};

void kqueue_reactor::waker_ref::wake() const noexcept {
    if (kq_ < 0) return;
    // One kevent() call: async-signal-safe per POSIX's list (kevent is a
    // syscall with no userspace locking).
    struct kevent ev;
    EV_SET(&ev, kWakeIdent, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
    ::kevent(kq_, &ev, 1, nullptr, 0, nullptr);
}

void kqueue_reactor::registration::release() noexcept {
    if (auto s = s_.lock()) kqueue_reactor::unwatch(*s, slot_);
    s_.reset();
}
kqueue_reactor::registration::registration(registration&& o) noexcept
    : s_(std::move(o.s_)), slot_(o.slot_) {
    o.s_.reset();
}
kqueue_reactor::registration& kqueue_reactor::registration::operator=(registration&& o) noexcept {
    if (this != &o) {
        release();
        s_    = std::move(o.s_);
        slot_ = o.slot_;
        o.s_.reset();
    }
    return *this;
}
kqueue_reactor::registration::~registration() { release(); }

result<void> kqueue_reactor::registration::modify(interest what) {
    auto s = s_.lock();
    if (!s || slot_ >= s->slots.size() || !s->slots[slot_].live)
        return std::unexpected(error::make(std::errc::invalid_argument,
                                           "modify: registration is empty"));
    auto& sl = s->slots[slot_];
    if (sl.what == what) return {};

    // kqueue holds one filter per direction, so this is a delete of the
    // filters no longer wanted and an add of the new ones. Adding first
    // would leave the old filter live if the add failed.
    struct kevent evs[2];
    int n = 0;
    void* udata = reinterpret_cast<void*>(static_cast<std::uintptr_t>(slot_) + 1);
    if (wants_read(sl.what)  && !wants_read(what))
        EV_SET(&evs[n++], sl.fd, EVFILT_READ,  EV_DELETE, 0, 0, nullptr);
    if (wants_write(sl.what) && !wants_write(what))
        EV_SET(&evs[n++], sl.fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    if (n > 0) ::kevent(s->kq, evs, n, nullptr, 0, nullptr);   // fd may be gone: fine

    n = 0;
    if (!wants_read(sl.what)  && wants_read(what))
        EV_SET(&evs[n++], sl.fd, EVFILT_READ,  EV_ADD, 0, 0, udata);
    if (!wants_write(sl.what) && wants_write(what))
        EV_SET(&evs[n++], sl.fd, EVFILT_WRITE, EV_ADD, 0, 0, udata);
    if (n > 0 && ::kevent(s->kq, evs, n, nullptr, 0, nullptr) != 0)
        return std::unexpected(error::from_errno(errno, "kevent modify"));

    sl.what = what;
    return {};
}

kqueue_reactor::kqueue_reactor(std::shared_ptr<state> s) noexcept : s_(std::move(s)) {}
kqueue_reactor::kqueue_reactor(kqueue_reactor&&) noexcept            = default;
kqueue_reactor& kqueue_reactor::operator=(kqueue_reactor&&) noexcept = default;
kqueue_reactor::~kqueue_reactor()                                    = default;

result<kqueue_reactor> kqueue_reactor::create() {
    auto s = std::make_shared<state>();
    s->kq = ::kqueue();
    if (s->kq < 0) return std::unexpected(error::from_errno(errno, "kqueue"));
    // EV_CLEAR: the user event resets when delivered, so N triggers before
    // a wait are one wakeup.
    struct kevent ev;
    EV_SET(&ev, kWakeIdent, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
    if (::kevent(s->kq, &ev, 1, nullptr, 0, nullptr) != 0)
        return std::unexpected(error::from_errno(errno, "kevent add EVFILT_USER"));
    return kqueue_reactor(std::move(s));
}

kqueue_reactor::waker_ref kqueue_reactor::waker() const noexcept {
    return waker_ref(s_ ? s_->kq : -1);
}

result<kqueue_reactor::registration>
kqueue_reactor::watch(handle fd, interest what, std::uint64_t token) {
    if (fd < 0) return std::unexpected(error::make(std::errc::bad_file_descriptor, "watch: bad fd"));
    auto& s = *s_;
    for (auto& sl : s.slots)
        if (sl.live && sl.fd == fd)
            return std::unexpected(error::make(std::errc::file_exists,
                                               "watch: fd already watched by this reactor"));
    std::uint32_t idx;
    if (!s.free.empty()) {
        idx = s.free.back();
        s.free.pop_back();
    } else {
        idx = static_cast<std::uint32_t>(s.slots.size());
        s.slots.emplace_back();
    }
    // kqueue filters are level-triggered unless EV_CLEAR is given, which
    // matches the contract (readiness persists until drained).
    struct kevent evs[2];
    int n = 0;
    void* udata = reinterpret_cast<void*>(static_cast<std::uintptr_t>(idx) + 1);
    if (wants_read(what))  EV_SET(&evs[n++], fd, EVFILT_READ,  EV_ADD, 0, 0, udata);
    if (wants_write(what)) EV_SET(&evs[n++], fd, EVFILT_WRITE, EV_ADD, 0, 0, udata);
    if (::kevent(s.kq, evs, n, nullptr, 0, nullptr) != 0) {
        const int e = errno;
        s.free.push_back(idx);
        return std::unexpected(error::from_errno(e, "kevent add"));
    }
    s.slots[idx] = {fd, what, token, true};
    return registration(std::weak_ptr<state>(s_), idx);
}

void kqueue_reactor::unwatch(state& s, std::uint32_t slot) noexcept {
    if (slot >= s.slots.size() || !s.slots[slot].live) return;
    auto& sl = s.slots[slot];
    struct kevent evs[2];
    int n = 0;
    if (wants_read(sl.what))  EV_SET(&evs[n++], sl.fd, EVFILT_READ,  EV_DELETE, 0, 0, nullptr);
    if (wants_write(sl.what)) EV_SET(&evs[n++], sl.fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    ::kevent(s.kq, evs, n, nullptr, 0, nullptr);      // fd may be closed already: fine
    sl.live = false;
    s.free.push_back(slot);
}

std::size_t kqueue_reactor::watched() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(s_->slots.begin(), s_->slots.end(), [](auto& x) { return x.live; }));
}

result<wait_result> kqueue_reactor::wait(std::optional<std::chrono::milliseconds> timeout) {
    auto& s = *s_;
    struct kevent evs[33];                             // read+write per handle, + waker
    using clk = std::chrono::steady_clock;
    const auto start = clk::now();
    int n;
    for (;;) {
        auto left = timeout;
        if (timeout) {
            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - start);
            left = *timeout > spent ? *timeout - spent : std::chrono::milliseconds{0};
        }
        timespec ts{};
        const bool bounded = to_timespec(left, ts);
        n = ::kevent(s.kq, nullptr, 0, evs, static_cast<int>(std::size(evs)),
                     bounded ? &ts : nullptr);
        // A chunk ran out but the caller's timeout didn't: wait again.
        if (n == 0 && left && left->count() > kMaxChunkMs) continue;
        if (n >= 0) break;
        if (errno != EINTR) return std::unexpected(error::from_errno(errno, "kevent wait"));
    }

    wait_result out;
    if (n == 0) {
        out.timeout = true;
        return out;
    }
    for (int i = 0; i < n; ++i) {
        const auto& e = evs[i];
        if (e.filter == EVFILT_USER) {
            out.woken = true;                          // EV_CLEAR already reset it
            continue;
        }
        const auto idx = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(e.udata) - 1);
        if (idx >= s.slots.size() || !s.slots[idx].live) continue;
        // One slot can produce a READ and a WRITE event: merge them.
        readiness* rd = nullptr;
        for (std::uint8_t k = 0; k < out.count; ++k)
            if (out.ready[k].token == s.slots[idx].token) rd = &out.ready[k];
        if (!rd) {
            if (out.count >= std::size(out.ready)) continue;
            rd = &out.ready[out.count++];
            rd->token = s.slots[idx].token;
        }
        if (e.filter == EVFILT_READ)  rd->readable = true;
        if (e.filter == EVFILT_WRITE) rd->writable = true;
        if (e.flags & EV_EOF)         rd->hangup   = true;
        if (e.flags & EV_ERROR)       rd->error    = true;
    }
    return out;
}

}  // namespace jaal::platform
