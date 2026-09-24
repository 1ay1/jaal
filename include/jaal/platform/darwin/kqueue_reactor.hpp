#pragma once
// jaal::platform::kqueue_reactor — the macOS / BSD reactor.
//
// Same contract as the others, backed by kqueue. The waker is EVFILT_USER,
// a kernel-side user event: no pipe, no fd pair, and a trigger is one
// syscall. Many triggers before a wait collapse into one event (EV_CLEAR),
// which is exactly the coalescing the contract asks for.
//
// Declared with NO OS headers; implemented in src/platform/darwin/.
//
// STATUS: this backend compiles for arm64 macOS (checked with a zig
// cross-compile) but has NOT been run: there's no Mac in the loop. It
// passes the conformance suite only once someone runs it on macOS.

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

#include "../../core/error.hpp"
#include "../concepts.hpp"

namespace jaal::platform {

class kqueue_reactor {
    struct state;

public:
    using handle = int;

    class waker_ref {
    public:
        void wake() const noexcept;
    private:
        friend class kqueue_reactor;
        explicit waker_ref(int kq) noexcept : kq_(kq) {}
        int kq_ = -1;
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
        friend class kqueue_reactor;
        registration(std::weak_ptr<state> s, std::uint32_t slot) noexcept
            : s_(std::move(s)), slot_(slot) {}
        void release() noexcept;
        std::weak_ptr<state> s_;
        std::uint32_t        slot_ = 0;
    };

    [[nodiscard]] static result<kqueue_reactor> create();

    kqueue_reactor(kqueue_reactor&&) noexcept;
    kqueue_reactor& operator=(kqueue_reactor&&) noexcept;
    kqueue_reactor(const kqueue_reactor&)            = delete;
    kqueue_reactor& operator=(const kqueue_reactor&) = delete;
    ~kqueue_reactor();

    [[nodiscard]] result<registration> watch(handle fd, interest what, std::uint64_t token);
    [[nodiscard]] result<wait_result>  wait(std::optional<std::chrono::milliseconds> timeout);
    [[nodiscard]] waker_ref            waker() const noexcept;
    [[nodiscard]] std::size_t          watched() const noexcept;

private:
    explicit kqueue_reactor(std::shared_ptr<state> s) noexcept;
    static void unwatch(state& s, std::uint32_t slot) noexcept;
    std::shared_ptr<state> s_;
};

static_assert(Reactor<kqueue_reactor>);

}  // namespace jaal::platform
