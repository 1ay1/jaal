// jaal::platform::poll_reactor — implementation. The only file that sees
// <poll.h>, <unistd.h> and friends for this backend.

#include <jaal/platform/posix/poll_reactor.hpp>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#if defined(__linux__)
#  include <sys/eventfd.h>
#endif

namespace jaal::platform {

namespace {

// Two fds for a self-pipe, or the same fd twice for an eventfd.
struct wake_fds {
    int read  = -1;
    int write = -1;
    bool is_eventfd = false;
};

result<wake_fds> make_wake() {
#if defined(__linux__)
    // eventfd: one fd, one syscall to signal, one to drain. Falls back to a
    // pipe when it's filtered (seccomp) or the fd table is tight.
    if (int e = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC); e >= 0)
        return wake_fds{e, e, true};
#endif
    int p[2];
#if defined(__linux__)
    if (::pipe2(p, O_NONBLOCK | O_CLOEXEC) != 0)
        return std::unexpected(error::from_errno(errno, "pipe2 for reactor wake"));
#else
    if (::pipe(p) != 0)
        return std::unexpected(error::from_errno(errno, "pipe for reactor wake"));
    for (int fd : p) {
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
#endif
    return wake_fds{p[0], p[1], false};
}

// Drain everything the waker accumulated: many wakes → one wakeup.
void drain_wake(const wake_fds& w) noexcept {
    if (w.is_eventfd) {
        std::uint64_t v;
        [[maybe_unused]] auto n = ::read(w.read, &v, sizeof v);
        return;
    }
    char buf[256];
    while (::read(w.read, buf, sizeof buf) > 0) {}
}

// poll() takes an int. Clamp the 64-bit chrono count into [0, INT_MAX]
// BEFORE narrowing: a raw cast of a value > INT_MAX wraps negative, and
// poll() reads a negative timeout as "block forever" (maya hit this).
int poll_timeout(std::optional<std::chrono::milliseconds> t) noexcept {
    if (!t) return -1;
    const auto c = t->count();
    if (c <= 0) return 0;
    if (c >= INT_MAX) return INT_MAX;
    return static_cast<int>(c);
}

}  // namespace

struct poll_reactor::state {
    wake_fds wake;
    struct slot {
        int           fd = -1;
        interest      what{};
        std::uint64_t token = 0;
        bool          live  = false;
    };
    std::vector<slot>          slots;     // index = registration slot
    std::vector<std::uint32_t> free;      // reusable slot indices
    std::vector<pollfd>        pfds;      // scratch for each wait
    std::vector<std::uint32_t> map;       // pfds[i+1] → slot index

    ~state() {
        if (wake.read >= 0) ::close(wake.read);
        if (wake.write >= 0 && wake.write != wake.read) ::close(wake.write);
    }
};

// ── waker ───────────────────────────────────────────────────────────────
void poll_reactor::waker_ref::wake() const noexcept {
    if (fd_ < 0) return;
    // A single write(2): async-signal-safe, callable from any thread.
    // EAGAIN (pipe full, or eventfd counter saturated) means a wake is
    // already pending, which is all we need.
    if (counter_) {
        const std::uint64_t one = 1;
        [[maybe_unused]] auto n = ::write(fd_, &one, sizeof one);
    } else {
        [[maybe_unused]] auto n = ::write(fd_, "w", 1);
    }
}

// ── registration ────────────────────────────────────────────────────────
void poll_reactor::registration::release() noexcept {
    if (auto s = s_.lock()) poll_reactor::unwatch(*s, slot_);
    s_.reset();
}

poll_reactor::registration::registration(registration&& o) noexcept
    : s_(std::move(o.s_)), slot_(o.slot_) {
    o.s_.reset();
}

poll_reactor::registration& poll_reactor::registration::operator=(registration&& o) noexcept {
    if (this != &o) {
        release();
        s_    = std::move(o.s_);
        slot_ = o.slot_;
        o.s_.reset();
    }
    return *this;
}

poll_reactor::registration::~registration() { release(); }

// ── reactor ─────────────────────────────────────────────────────────────
poll_reactor::poll_reactor(std::shared_ptr<state> s) noexcept : s_(std::move(s)) {}
poll_reactor::poll_reactor(poll_reactor&&) noexcept            = default;
poll_reactor& poll_reactor::operator=(poll_reactor&&) noexcept = default;
poll_reactor::~poll_reactor()                                  = default;

result<poll_reactor> poll_reactor::create() {
    auto w = make_wake();
    if (!w) return std::unexpected(w.error());
    auto s  = std::make_shared<state>();
    s->wake = *w;
    return poll_reactor(std::move(s));
}

poll_reactor::waker_ref poll_reactor::waker() const noexcept {
    return s_ ? waker_ref(s_->wake.write, s_->wake.is_eventfd) : waker_ref(-1, false);
}

result<poll_reactor::registration>
poll_reactor::watch(handle fd, interest what, std::uint64_t token) {
    if (fd < 0) return std::unexpected(error::make(std::errc::bad_file_descriptor, "watch: bad fd"));
    for (auto& sl : s_->slots)
        if (sl.live && sl.fd == fd)
            return std::unexpected(error::make(std::errc::file_exists,
                                               "watch: fd already watched by this reactor"));
    std::uint32_t idx;
    if (!s_->free.empty()) {
        idx = s_->free.back();
        s_->free.pop_back();
    } else {
        idx = static_cast<std::uint32_t>(s_->slots.size());
        s_->slots.emplace_back();
    }
    s_->slots[idx] = {fd, what, token, true};
    return registration(std::weak_ptr<state>(s_), idx);
}

void poll_reactor::unwatch(state& s, std::uint32_t slot) noexcept {
    if (slot >= s.slots.size() || !s.slots[slot].live) return;
    s.slots[slot].live = false;
    s.free.push_back(slot);
}

std::size_t poll_reactor::watched() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(s_->slots.begin(), s_->slots.end(), [](auto& s) { return s.live; }));
}

result<wait_result> poll_reactor::wait(std::optional<std::chrono::milliseconds> timeout) {
    auto& pf = s_->pfds;
    auto& mp = s_->map;
    pf.clear();
    mp.clear();
    pf.push_back({s_->wake.read, POLLIN, 0});
    for (std::uint32_t i = 0; i < s_->slots.size(); ++i) {
        auto& sl = s_->slots[i];
        if (!sl.live) continue;
        short ev = 0;
        if (wants_read(sl.what))  ev |= POLLIN;
        if (wants_write(sl.what)) ev |= POLLOUT;
        pf.push_back({sl.fd, ev, 0});
        mp.push_back(i);
    }

    // EINTR never ends a wait early: retry with the time that's left.
    using clk = std::chrono::steady_clock;
    const auto start = clk::now();
    int r;
    for (;;) {
        auto left = timeout;
        if (timeout) {
            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - start);
            left = *timeout > spent ? *timeout - spent : std::chrono::milliseconds{0};
        }
        r = ::poll(pf.data(), static_cast<nfds_t>(pf.size()), poll_timeout(left));
        if (r >= 0) break;
        if (errno != EINTR) return std::unexpected(error::from_errno(errno, "poll"));
    }

    wait_result out;
    if (r == 0) {
        out.timeout = true;
        return out;
    }
    if (pf[0].revents & POLLIN) {
        drain_wake(s_->wake);
        out.woken = true;
    }
    for (std::size_t i = 1; i < pf.size() && out.count < std::size(out.ready); ++i) {
        const short re = pf[i].revents;
        if (re == 0) continue;
        auto& rd    = out.ready[out.count++];
        rd.token    = s_->slots[mp[i - 1]].token;
        rd.readable = (re & POLLIN) != 0;
        rd.writable = (re & POLLOUT) != 0;
        // Report a closed peer instead of letting the caller spin on
        // read() == 0 (maya's POLLHUP-on-stdin fix).
        rd.hangup   = (re & POLLHUP) != 0;
        rd.error    = (re & (POLLERR | POLLNVAL)) != 0;
    }
    return out;
}

}  // namespace jaal::platform
