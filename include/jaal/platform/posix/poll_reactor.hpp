#pragma once
// jaal::platform::poll_reactor — the portable POSIX reactor (poll + a wake
// eventfd/pipe). The reference backend: every other reactor must behave
// exactly like this one (tests/platform/conformance.cpp).
//
// Declared here with NO OS headers; implemented in src/platform/posix/.

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

#include "../../core/error.hpp"
#include "../concepts.hpp"

namespace jaal::platform {

class poll_reactor {
    struct state;                               // defined in the .cpp

public:
    using handle = int;                         // file descriptor

    /// A non-owning handle to the reactor's waker. Copy it to other threads;
    /// wake() is thread-safe and async-signal-safe.
    ///
    /// It holds the raw write fd, so it must not be used after the reactor
    /// is destroyed (the fd could be reused). The kernel guarantees that by
    /// only giving a waker to its own mailbox, which dies first.
    class waker_ref {
    public:
        void wake() const noexcept;
    private:
        friend class poll_reactor;
        waker_ref(int fd, bool counter) noexcept : fd_(fd), counter_(counter) {}
        int  fd_      = -1;
        bool counter_ = false;       // eventfd (8-byte counter) vs pipe (1 byte)
    };

    /// RAII watch. Dropping it (or moving from it) stops watching.
    ///
    /// It points at the reactor's heap state, not at the reactor object, so
    /// moving the reactor doesn't leave it dangling. The pointer is weak: a
    /// registration that outlives its reactor unwatches as a safe no-op
    /// instead of writing into freed memory.
    class registration {
    public:
        registration() noexcept = default;
        registration(registration&& o) noexcept;
        registration& operator=(registration&& o) noexcept;
        registration(const registration&)            = delete;
        registration& operator=(const registration&) = delete;
        ~registration();
    private:
        friend class poll_reactor;
        registration(std::weak_ptr<state> s, std::uint32_t slot) noexcept
            : s_(std::move(s)), slot_(slot) {}
        void release() noexcept;
        std::weak_ptr<state> s_;
        std::uint32_t        slot_ = 0;
    };

    [[nodiscard]] static result<poll_reactor> create();

    poll_reactor(poll_reactor&&) noexcept;
    poll_reactor& operator=(poll_reactor&&) noexcept;
    poll_reactor(const poll_reactor&)            = delete;
    poll_reactor& operator=(const poll_reactor&) = delete;
    ~poll_reactor();

    [[nodiscard]] result<registration> watch(handle fd, interest what, std::uint64_t token);
    [[nodiscard]] result<wait_result>  wait(std::optional<std::chrono::milliseconds> timeout);
    [[nodiscard]] waker_ref            waker() const noexcept;

    /// Registered handles (not counting the waker). For tests.
    [[nodiscard]] std::size_t watched() const noexcept;

private:
    explicit poll_reactor(std::shared_ptr<state> s) noexcept;
    static void unwatch(state& s, std::uint32_t slot) noexcept;
    std::shared_ptr<state> s_;
};

static_assert(Reactor<poll_reactor>);

}  // namespace jaal::platform
