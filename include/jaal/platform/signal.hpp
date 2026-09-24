#pragma once
// jaal::platform signals — OS signals as events, never as code in a handler.
//
// User code never runs inside a signal handler. A signal source turns each
// signal into (a) a pending bit and (b) a wakeup on a handle the reactor
// watches. The loop then calls take() and gets a signal_set, on its own
// thread, like any other event. All the async-signal-safety reasoning lives
// in one place: the backend.
//
//   auto sigs = native_signals::install({sig::interrupt, sig::resize}).value();
//   auto reg  = reactor.watch(sigs.handle(), interest::read, kSigToken).value();
//   ... wait ...
//   for (auto s : sigs.take()) { ... }
//
// The portable set is deliberately small. `suspend` (SIGTSTP) is left out:
// catching it changes job control, and doing it right (restore the tty,
// re-raise with the default action, redo raw mode on SIGCONT) is a host's
// job, not this layer's.

#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <type_traits>

#include "../core/error.hpp"

namespace jaal::platform {

enum class sig : std::uint8_t {
    interrupt,   // Ctrl+C              SIGINT   / CTRL_C_EVENT, CTRL_BREAK_EVENT
    terminate,   // asked to stop       SIGTERM  / CTRL_LOGOFF_EVENT, CTRL_SHUTDOWN_EVENT
    hangup,      // terminal went away  SIGHUP   / CTRL_CLOSE_EVENT
    resize,      // window size changed SIGWINCH / (console input, not a signal: see host)
    child,       // a child exited      SIGCHLD  / (no equivalent)
};
inline constexpr unsigned signal_count = 5;

/// A set of signals, as a small bitmask.
class signal_set {
public:
    constexpr signal_set() noexcept = default;
    constexpr signal_set(std::initializer_list<sig> l) noexcept {
        for (auto s : l) add(s);
    }
    static constexpr signal_set from_bits(std::uint8_t b) noexcept {
        signal_set s;
        s.bits_ = static_cast<std::uint8_t>(b & mask());
        return s;
    }

    constexpr void add(sig s) noexcept    { bits_ |= bit(s); }
    constexpr void remove(sig s) noexcept { bits_ &= static_cast<std::uint8_t>(~bit(s)); }
    [[nodiscard]] constexpr bool contains(sig s) const noexcept { return (bits_ & bit(s)) != 0; }
    [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }
    [[nodiscard]] constexpr std::uint8_t bits() const noexcept { return bits_; }

    constexpr signal_set operator|(signal_set o) const noexcept { return from_bits(bits_ | o.bits_); }
    constexpr signal_set operator&(signal_set o) const noexcept { return from_bits(bits_ & o.bits_); }
    constexpr bool operator==(const signal_set&) const noexcept = default;

    // Iterate the signals in the set: for (signal s : set) ...
    class iterator {
    public:
        constexpr sig operator*() const noexcept { return static_cast<sig>(i_); }
        constexpr iterator& operator++() noexcept { i_ = next(bits_, i_ + 1); return *this; }
        constexpr bool operator==(const iterator&) const noexcept = default;
    private:
        friend class signal_set;
        constexpr iterator(std::uint8_t b, unsigned i) noexcept : bits_(b), i_(next(b, i)) {}
        static constexpr unsigned next(std::uint8_t b, unsigned i) noexcept {
            while (i < signal_count && !(b & (1u << i))) ++i;
            return i;
        }
        std::uint8_t bits_;
        unsigned     i_;
    };
    [[nodiscard]] constexpr iterator begin() const noexcept { return {bits_, 0}; }
    [[nodiscard]] constexpr iterator end() const noexcept   { return {bits_, signal_count}; }

private:
    static constexpr std::uint8_t bit(sig s) noexcept {
        return static_cast<std::uint8_t>(1u << static_cast<unsigned>(s));
    }
    static constexpr std::uint8_t mask() noexcept {
        return static_cast<std::uint8_t>((1u << signal_count) - 1);
    }
    std::uint8_t bits_ = 0;
};

/// A signal source: installed with the signals it wants, exposes one handle
/// for the reactor to watch, and hands arrived signals over with take().
/// Uninstalls (restoring whatever was there before) when destroyed.
template <class S>
concept SignalSource =
    std::movable<S> && !std::copyable<S>
    && requires(S& s, const S& cs, signal_set set) {
        typename S::native_handle;
        { S::install(set) } -> std::same_as<result<S>>;
        { cs.handle() } -> std::same_as<typename S::native_handle>;
        { s.take() } -> std::same_as<signal_set>;
        { cs.watching() } -> std::same_as<signal_set>;
    };

}  // namespace jaal::platform
