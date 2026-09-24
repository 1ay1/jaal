// jaal::platform::epoll_reactor — implementation.

#include <jaal/platform/linux/epoll_reactor.hpp>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <vector>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace jaal::platform {

namespace {

// The waker's epoll user-data. Registration slots use their index + 1, so
// 0 can never collide with one.
constexpr std::uint64_t kWakeTag = 0;

int epoll_timeout(std::optional<std::chrono::milliseconds> t) noexcept {
    if (!t) return -1;
    const auto c = t->count();
    if (c <= 0) return 0;
    if (c >= INT_MAX) return INT_MAX;       // clamp before narrowing
    return static_cast<int>(c);
}

}  // namespace

struct epoll_reactor::state {
    int ep   = -1;
    int wake = -1;
    struct slot {
        int           fd = -1;
        std::uint64_t token = 0;
        bool          live  = false;
    };
    std::vector<slot>          slots;
    std::vector<std::uint32_t> free;

    ~state() {
        if (ep >= 0)   ::close(ep);
        if (wake >= 0) ::close(wake);
    }
};

void epoll_reactor::waker_ref::wake() const noexcept {
    if (fd_ < 0) return;
    const std::uint64_t one = 1;             // async-signal-safe: one write(2)
    [[maybe_unused]] auto n = ::write(fd_, &one, sizeof one);
}

void epoll_reactor::registration::release() noexcept {
    if (auto s = s_.lock()) epoll_reactor::unwatch(*s, slot_);
    s_.reset();
}
epoll_reactor::registration::registration(registration&& o) noexcept
    : s_(std::move(o.s_)), slot_(o.slot_) {
    o.s_.reset();
}
epoll_reactor::registration& epoll_reactor::registration::operator=(registration&& o) noexcept {
    if (this != &o) {
        release();
        s_    = std::move(o.s_);
        slot_ = o.slot_;
        o.s_.reset();
    }
    return *this;
}
epoll_reactor::registration::~registration() { release(); }

result<void> epoll_reactor::registration::modify(interest what) {
    auto s = s_.lock();
    if (!s || slot_ >= s->slots.size() || !s->slots[slot_].live)
        return std::unexpected(error::make(std::errc::invalid_argument,
                                           "modify: registration is empty"));
    auto& sl = s->slots[slot_];
    epoll_event ev{};
    ev.events = EPOLLRDHUP;                              // level-triggered
    if (wants_read(what))  ev.events |= EPOLLIN;
    if (wants_write(what)) ev.events |= EPOLLOUT;
    ev.data.u64 = std::uint64_t{slot_} + 1;              // same token, same slot
    if (::epoll_ctl(s->ep, EPOLL_CTL_MOD, sl.fd, &ev) != 0)
        return std::unexpected(error::from_errno(errno, "epoll_ctl mod"));
    return {};
}

epoll_reactor::epoll_reactor(std::shared_ptr<state> s) noexcept : s_(std::move(s)) {}
epoll_reactor::epoll_reactor(epoll_reactor&&) noexcept            = default;
epoll_reactor& epoll_reactor::operator=(epoll_reactor&&) noexcept = default;
epoll_reactor::~epoll_reactor()                                   = default;

result<epoll_reactor> epoll_reactor::create() {
    auto s = std::make_shared<state>();
    s->ep = ::epoll_create1(EPOLL_CLOEXEC);
    if (s->ep < 0) return std::unexpected(error::from_errno(errno, "epoll_create1"));
    s->wake = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (s->wake < 0) return std::unexpected(error::from_errno(errno, "eventfd for reactor wake"));
    epoll_event ev{};
    ev.events   = EPOLLIN;
    ev.data.u64 = kWakeTag;
    if (::epoll_ctl(s->ep, EPOLL_CTL_ADD, s->wake, &ev) != 0)
        return std::unexpected(error::from_errno(errno, "epoll_ctl add wake"));
    return epoll_reactor(std::move(s));
}

epoll_reactor::waker_ref epoll_reactor::waker() const noexcept {
    return waker_ref(s_ ? s_->wake : -1);
}

result<epoll_reactor::registration>
epoll_reactor::watch(handle fd, interest what, std::uint64_t token) {
    if (fd < 0) return std::unexpected(error::make(std::errc::bad_file_descriptor, "watch: bad fd"));
    auto& s = *s_;
    std::uint32_t idx;
    if (!s.free.empty()) {
        idx = s.free.back();
        s.free.pop_back();
    } else {
        idx = static_cast<std::uint32_t>(s.slots.size());
        s.slots.emplace_back();
    }
    epoll_event ev{};
    ev.events = EPOLLRDHUP;                              // level-triggered
    if (wants_read(what))  ev.events |= EPOLLIN;
    if (wants_write(what)) ev.events |= EPOLLOUT;
    ev.data.u64 = std::uint64_t{idx} + 1;                // 0 is the waker
    if (::epoll_ctl(s.ep, EPOLL_CTL_ADD, fd, &ev) != 0) {
        const int e = errno;
        s.free.push_back(idx);
        if (e == EEXIST)
            return std::unexpected(error::make(std::errc::file_exists,
                                               "watch: fd already watched by this reactor"));
        return std::unexpected(error::from_errno(e, "epoll_ctl add"));
    }
    s.slots[idx] = {fd, token, true};
    return registration(std::weak_ptr<state>(s_), idx);
}

void epoll_reactor::unwatch(state& s, std::uint32_t slot) noexcept {
    if (slot >= s.slots.size() || !s.slots[slot].live) return;
    // The fd may already be closed by its owner; EBADF/ENOENT are fine.
    ::epoll_ctl(s.ep, EPOLL_CTL_DEL, s.slots[slot].fd, nullptr);
    s.slots[slot].live = false;
    s.free.push_back(slot);
}

std::size_t epoll_reactor::watched() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(s_->slots.begin(), s_->slots.end(), [](auto& x) { return x.live; }));
}

result<wait_result> epoll_reactor::wait(std::optional<std::chrono::milliseconds> timeout) {
    auto& s = *s_;
    epoll_event evs[17];                                 // 16 ready + the waker
    using clk = std::chrono::steady_clock;
    const auto start = clk::now();
    int n;
    for (;;) {
        auto left = timeout;
        if (timeout) {
            const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - start);
            left = *timeout > spent ? *timeout - spent : std::chrono::milliseconds{0};
        }
        n = ::epoll_wait(s.ep, evs, static_cast<int>(std::size(evs)), epoll_timeout(left));
        if (n >= 0) break;
        if (errno != EINTR) return std::unexpected(error::from_errno(errno, "epoll_wait"));
    }

    wait_result out;
    if (n == 0) {
        out.timeout = true;
        return out;
    }
    for (int i = 0; i < n; ++i) {
        const auto tag = evs[i].data.u64;
        const auto e   = evs[i].events;
        if (tag == kWakeTag) {
            std::uint64_t v;
            [[maybe_unused]] auto r = ::read(s.wake, &v, sizeof v);   // drain: many → one
            out.woken = true;
            continue;
        }
        const auto idx = static_cast<std::uint32_t>(tag - 1);
        if (idx >= s.slots.size() || !s.slots[idx].live) continue;   // unwatched meanwhile
        if (out.count >= std::size(out.ready)) continue;
        auto& rd    = out.ready[out.count++];
        rd.token    = s.slots[idx].token;
        rd.readable = (e & EPOLLIN) != 0;
        rd.writable = (e & EPOLLOUT) != 0;
        rd.hangup   = (e & (EPOLLHUP | EPOLLRDHUP)) != 0;
        rd.error    = (e & EPOLLERR) != 0;
    }
    return out;
}

}  // namespace jaal::platform
