#pragma once
// jaal::Sink<Msg> — the only way work outside the loop can talk to it.
//
// Design: CONCURRENCY.md §4.4. The rules, all enforced by the type:
//
//   * WEAK. A Sink holds a weak reference to the mailbox. There is no API
//     that turns it into a strong one, so a task can never keep the
//     runtime alive. maya's BackgroundQueue hit exactly that: a task that
//     held the queue strongly made a cycle, the queue leaked, and at
//     shutdown a worker destroyed the queue and joined itself.
//
//   * MINTED BY THE KERNEL ONLY. The constructor that takes a mailbox is
//     private; the kernel (and tests, through sink_access) are the only
//     callers. User code can copy a Sink it was given, never forge one.
//
//   * Msg MUST BE Sendable, so nothing borrowed rides along.
//
//   * Sending after shutdown is a no-op that returns false. Never a crash,
//     never a write into freed memory.
//
// A default-constructed Sink is "closed": every send returns false. That's
// what a task gets in tests that don't care about its output.

#include <memory>
#include <utility>

#include "sendable.hpp"

namespace jaal {

/// What a mailbox must provide for a Sink to post into it. The kernel's
/// mailbox (kernel/mailbox.hpp) satisfies this; tests can use their own.
template <class B, class Msg>
concept MailboxFor = requires(B& b, Msg m) {
    { b.post(std::move(m)) } -> std::same_as<bool>;
};

namespace detail {
template <class Msg>
struct mailbox_iface {
    virtual ~mailbox_iface() = default;
    virtual bool post(Msg m) = 0;
};
}  // namespace detail

struct sink_access;   // the kernel's key

template <class Msg>
class Sink {
    static_assert(Sendable<Msg>, "jaal: a Sink's Msg must be Sendable "
                                 "(see jaal::sendable_reason<Msg>())");
public:
    using msg_type = Msg;

    Sink() noexcept = default;                         // closed

    /// Post a message. False when the loop is gone (or this Sink is closed).
    bool send(Msg m) const {
        if (auto box = box_.lock()) return box->post(std::move(m));
        return false;
    }

    /// Is anyone still listening? Only a hint: the loop may stop right after.
    [[nodiscard]] bool open() const noexcept { return !box_.expired(); }

private:
    friend struct sink_access;
    explicit Sink(std::weak_ptr<detail::mailbox_iface<Msg>> box) noexcept
        : box_(std::move(box)) {}

    std::weak_ptr<detail::mailbox_iface<Msg>> box_;
};

// Sinks are Sendable: they own a weak reference and nothing else, and
// posting through one is thread-safe by the mailbox contract.
template <class Msg> inline constexpr bool sendable_opt_in<Sink<Msg>> = true;

/// The kernel's key for minting sinks. Tests use it too; app code has no
/// reason to, and a ban-list check can flag it outside jaal and tests.
struct sink_access {
    template <class Msg>
    static Sink<Msg> make(std::weak_ptr<detail::mailbox_iface<Msg>> box) noexcept {
        return Sink<Msg>(std::move(box));
    }
};

}  // namespace jaal
