#pragma once
// jaal::rng — a small, explicit, reproducible random source.
//
// splitmix64: 8 bytes of state, no allocation, and the SAME sequence on
// every platform and compiler (a std::mt19937 is portable, but the
// distributions in <random> are not: std::uniform_int_distribution can give
// different numbers per standard library, which would break replay).
//
// An rng is a VALUE. Copying it copies the position in the sequence, so a
// copy repeats what the original will produce; split() makes an independent
// stream instead. That's what lets the kernel hand a fresh, deterministic
// rng to every `random` effect (core/fx.hpp).
//
//   jaal::rng r{12345};
//   r.next();            // a uniform std::uint64_t
//   r.below(6) + 1;      // a die, unbiased
//   r.chance(0.1);       // true one time in ten
//   r.uniform();         // a double in [0, 1)
//   r.between(-1.0, 1.0);

#include <cstdint>
#include <type_traits>

namespace jaal {

class rng {
public:
    using result_type = std::uint64_t;

    constexpr rng() noexcept = default;
    constexpr explicit rng(std::uint64_t seed) noexcept : s_(seed) {}

    /// The next 64 bits. Every bit is usable.
    constexpr std::uint64_t next() noexcept {
        std::uint64_t z = (s_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    /// A number in [0, n), unbiased (rejection, not modulo). 0 for n == 0.
    constexpr std::uint64_t below(std::uint64_t n) noexcept {
        if (n == 0) return 0;
        // Reject the short tail so every value is equally likely.
        const std::uint64_t limit = UINT64_MAX - (UINT64_MAX % n) - 1;
        std::uint64_t x = next();
        while (x > limit) x = next();
        return x % n;
    }

    /// A number in [lo, hi], for any integer type. Swapped bounds are fine.
    template <class I>
        requires std::is_integral_v<I>
    constexpr I in(I lo, I hi) noexcept {
        if (hi < lo) {
            const I t = lo;
            lo = hi;
            hi = t;
        }
        using U = std::make_unsigned_t<I>;
        const auto span = static_cast<std::uint64_t>(static_cast<U>(hi) - static_cast<U>(lo));
        return static_cast<I>(static_cast<U>(lo) + static_cast<U>(below(span + 1)));
    }

    /// A double in [0, 1), from the top 53 bits (the mantissa's width).
    constexpr double uniform() noexcept {
        return static_cast<double>(next() >> 11) * 0x1.0p-53;
    }

    /// A double in [lo, hi).
    constexpr double between(double lo, double hi) noexcept {
        return lo + uniform() * (hi - lo);
    }

    /// True with probability p. p <= 0 never, p >= 1 always (no draw wasted).
    constexpr bool chance(double p) noexcept {
        if (p <= 0.0) return false;
        if (p >= 1.0) return true;
        return uniform() < p;
    }

    /// Pick one of n things, or -1 when n == 0.
    constexpr std::int64_t pick(std::size_t n) noexcept {
        return n == 0 ? -1 : static_cast<std::int64_t>(below(n));
    }

    /// An INDEPENDENT stream, and advance this one. Use this instead of
    /// copying when two places each need their own randomness.
    constexpr rng split() noexcept { return rng{next()}; }

    [[nodiscard]] constexpr std::uint64_t state() const noexcept { return s_; }

    // For <random>-style use (std::shuffle, distributions if you accept
    // that they're not portable).
    static constexpr std::uint64_t min() noexcept { return 0; }
    static constexpr std::uint64_t max() noexcept { return UINT64_MAX; }
    constexpr std::uint64_t operator()() noexcept { return next(); }

private:
    std::uint64_t s_ = 0;
};

// The sequence is fixed forever: replay and sim reports depend on it.
static_assert(rng{1}.state() == 1);

}  // namespace jaal
