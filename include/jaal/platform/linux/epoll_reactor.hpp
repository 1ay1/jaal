#pragma once
// jaal::platform::epoll_reactor — the Linux reactor.
//
// Same contract as poll_reactor (and the same conformance suite), backed by
// epoll: registration is O(1) in the kernel instead of rebuilding a pollfd
// array every wait, which matters once a program watches many handles.
//
// Declared with NO OS headers; implemented in src/platform/linux/.
//
// Linux-specific rules:
//   * level-triggered, not edge-triggered. Edge-triggered loses readiness
//     if a caller doesn't drain a handle completely, which is exactly the
//     class of bug this layer exists to rule out.
//   * EPOLLRDHUP is requested so a half-closed peer is reported as hangup.
//   * epoll_wait's int timeout gets the same clamp as poll's.

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

#include "../../core/error.hpp"
#include "../concepts.hpp"

namespace jaal::platform {

class epoll_reactor {
    struct state;

public:
    using handle = int;

    class waker_ref {
    public:
        void wake() const noexcept;
    private:
        friend class epoll_reactor;
        explicit waker_ref(int fd) noexcept : fd_(fd) {}
        int fd_ = -1;                        // an eventfd
    };

    class registration {
    public:
        registration() noexcept = default;
        registration(registration&& o) noexcept;
        registration& operator=(registration&& o) noexcept;
        registration(const registration&)            = delete;
        registration& operator=(const registration&) = delete;
        ~registration();
    private:
        friend class epoll_reactor;
        registration(std::weak_ptr<state> s, std::uint32_t slot) noexcept
            : s_(std::move(s)), slot_(slot) {}
        void release() noexcept;
        std::weak_ptr<state> s_;
        std::uint32_t        slot_ = 0;
    };

    [[nodiscard]] static result<epoll_reactor> create();

    epoll_reactor(epoll_reactor&&) noexcept;
    epoll_reactor& operator=(epoll_reactor&&) noexcept;
    epoll_reactor(const epoll_reactor&)            = delete;
    epoll_reactor& operator=(const epoll_reactor&) = delete;
    ~epoll_reactor();

    [[nodiscard]] result<registration> watch(handle fd, interest what, std::uint64_t token);
    [[nodiscard]] result<wait_result>  wait(std::optional<std::chrono::milliseconds> timeout);
    [[nodiscard]] waker_ref            waker() const noexcept;
    [[nodiscard]] std::size_t          watched() const noexcept;

private:
    explicit epoll_reactor(std::shared_ptr<state> s) noexcept;
    static void unwatch(state& s, std::uint32_t slot) noexcept;
    std::shared_ptr<state> s_;
};

static_assert(Reactor<epoll_reactor>);

}  // namespace jaal::platform
