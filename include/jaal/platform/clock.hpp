#pragma once
// jaal::platform::steady_clock / sim_clock — time, real and simulated.
//
// The kernel is a template over its Clock, so every timer rule is testable
// without sleeping. Rules:
//   * a Clock must be STEADY. system_clock can jump backwards when the user
//     changes the wall clock, so it doesn't satisfy the concept and can't
//     be used for timers by accident.
//   * now() is an instance call, not static, so a sim_clock is per-test
//     rather than a global.

#include <chrono>
#include <concepts>

namespace jaal::platform {

template <class C>
concept Clock = requires {
    typename C::duration;
    typename C::time_point;
    requires C::is_steady;
} && requires(C& c) {
    { c.now() } -> std::same_as<typename C::time_point>;
};

/// The real clock.
struct steady_clock {
    using base       = std::chrono::steady_clock;
    using duration   = base::duration;
    using time_point = base::time_point;
    static constexpr bool is_steady = true;

    [[nodiscard]] time_point now() const noexcept { return base::now(); }
};

/// A clock that only moves when a test says so.
struct sim_clock {
    using duration   = std::chrono::nanoseconds;
    using time_point = std::chrono::time_point<sim_clock, duration>;
    static constexpr bool is_steady = true;

    [[nodiscard]] time_point now() const noexcept { return now_; }

    void advance(duration d) noexcept { now_ += d; }
    void set(time_point t) noexcept   { now_ = t; }

private:
    time_point now_{};
};

static_assert(Clock<steady_clock>);
static_assert(Clock<sim_clock>);
// The point of requiring is_steady: a wall clock can jump.
static_assert(!Clock<std::chrono::system_clock>);

}  // namespace jaal::platform
