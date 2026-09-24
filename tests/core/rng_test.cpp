// tests/core/rng_test.cpp — jaal::rng and the fx::random effect.
//
// What must hold:
//   * the sequence is FIXED: splitmix64 reference values, so a seed means
//     the same run on every platform, compiler and standard library
//   * below(n) is in range and unbiased enough to be uniform
//   * split() gives an independent stream; a copy repeats
//   * Cmd::random draws from the KERNEL's stream, so the same
//     options::random_seed replays the same draws, and two different seeds
//     don't agree
//   * seed_used() reports the seed actually used, so a real run (which picks
//     one) can be reproduced by passing it back

#include <jaal/core/rng.hpp>
#include <jaal/host/given.hpp>
#include <jaal/host/headless.hpp>
#include <jaal/jaal.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <set>
#include <variant>
#include <vector>

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,   \
                         __LINE__, #c);                                   \
            return 1;                                                     \
        }                                                                 \
    } while (0)

namespace {

// ── the generator ────────────────────────────────────────────────────────
int sequence_is_fixed() {
    // splitmix64 reference values for seed 0. If these ever change, every
    // recorded sim seed in every bug report becomes meaningless.
    jaal::rng r{0};
    CHECK(r.next() == 0xE220A8397B1DCDAFULL);
    CHECK(r.next() == 0x6E789E6AA1B965F4ULL);
    CHECK(r.next() == 0x06C45D188009454FULL);
    return 0;
}

int same_seed_same_numbers() {
    jaal::rng a{42}, b{42};
    for (int i = 0; i < 1000; ++i) CHECK(a.next() == b.next());
    return 0;
}

int below_is_in_range_and_spread() {
    jaal::rng r{7};
    std::array<int, 6> hits{};
    for (int i = 0; i < 6000; ++i) {
        auto v = r.below(6);
        CHECK(v < 6);
        ++hits[static_cast<std::size_t>(v)];
    }
    // A fair die over 6000 rolls: every face well inside a loose band. This
    // is a smoke test for bias, not a statistics suite.
    for (int h : hits) CHECK(h > 800 && h < 1200);
    CHECK(r.below(0) == 0);                  // no division by zero
    CHECK(r.below(1) == 0);                  // the only value there is
    return 0;
}

int in_covers_both_ends() {
    jaal::rng r{11};
    std::set<int> seen;
    for (int i = 0; i < 400; ++i) {
        int v = r.in(3, 5);
        CHECK(v >= 3 && v <= 5);
        seen.insert(v);
    }
    CHECK(seen.size() == 3);                 // inclusive on both ends
    CHECK(r.in(9, 9) == 9);                  // an empty range is that value
    // Swapped bounds are accepted rather than undefined.
    for (int i = 0; i < 50; ++i) {
        int v = r.in(10, 2);
        CHECK(v >= 2 && v <= 10);
    }
    // Signed ranges spanning zero don't overflow.
    jaal::rng s{12};
    for (int i = 0; i < 200; ++i) {
        int v = s.in(-5, 5);
        CHECK(v >= -5 && v <= 5);
    }
    return 0;
}

int doubles_stay_in_range() {
    jaal::rng r{13};
    for (int i = 0; i < 1000; ++i) {
        double u = r.uniform();
        CHECK(u >= 0.0 && u < 1.0);
        double b = r.between(-2.0, 2.0);
        CHECK(b >= -2.0 && b < 2.0);
    }
    // The ends of chance() don't draw at all, so they can't be off by one.
    CHECK(!r.chance(0.0));
    CHECK(!r.chance(-1.0));
    CHECK(r.chance(1.0));
    CHECK(r.chance(2.0));
    int heads = 0;
    for (int i = 0; i < 1000; ++i) heads += r.chance(0.5) ? 1 : 0;
    CHECK(heads > 400 && heads < 600);
    return 0;
}

int split_is_independent_but_copy_repeats() {
    jaal::rng a{99};
    jaal::rng copy = a;                      // a copy is the SAME stream
    CHECK(copy.next() == a.next());

    jaal::rng b{99};
    jaal::rng child = b.split();             // split is a different stream
    // The child doesn't repeat what the parent goes on to produce.
    bool differs = false;
    for (int i = 0; i < 8; ++i)
        if (child.next() != b.next()) differs = true;
    CHECK(differs);
    return 0;
}

int pick_handles_empty() {
    jaal::rng r{5};
    CHECK(r.pick(0) == -1);                  // nothing to pick from
    for (int i = 0; i < 100; ++i) {
        auto k = r.pick(3);
        CHECK(k >= 0 && k < 3);
    }
    return 0;
}

// ── the effect ───────────────────────────────────────────────────────────
// A program that rolls a die on demand and remembers every roll.
struct Dice {
    struct Model { std::vector<int> rolls; };
    struct Roll {};
    struct Rolled { int face; };
    using Msg = std::variant<Roll, Rolled>;
    using Cmd = jaal::Cmd<Msg>;

    static Cmd update(Model&, Roll) {
        return Cmd::random([](jaal::rng& r) -> Msg {
            return Rolled{static_cast<int>(r.in(1, 6))};
        });
    }
    static Cmd update(Model& m, Rolled r) { m.rolls.push_back(r.face); return {}; }
};

std::vector<int> roll_with_seed(std::uint64_t seed, int times) {
    jaal::kernel::options o;
    o.random_seed = seed;
    jaal::headless<Dice> h(o);
    for (int i = 0; i < times; ++i) h.send(Dice::Roll{});
    h.advance(std::chrono::milliseconds{1});   // fold the rolls and their results
    return h.model().rolls;
}

int effect_is_reproducible() {
    auto a = roll_with_seed(1234, 20);
    auto b = roll_with_seed(1234, 20);
    CHECK(a.size() == 20);
    CHECK(a == b);                            // same seed, same rolls
    for (int f : a) CHECK(f >= 1 && f <= 6);
    // A different seed gives a different run (with 20 d6 rolls, the chance
    // of a collision is about 6^-20).
    CHECK(roll_with_seed(9999, 20) != a);
    return 0;
}

int seed_is_reported() {
    jaal::kernel::options o;                  // no seed: the kernel picks one
    jaal::headless<Dice> h1(o);
    // headless fixes a seed so tests are deterministic...
    CHECK(h1.kernel().seed_used() == jaal::headless<Dice>::default_seed);

    // ...and whatever seed ran, feeding it back reproduces the draws.
    o.random_seed = 0;
    jaal::headless<Dice> h2(o);
    const auto used = h2.kernel().seed_used();
    CHECK(used != 0);
    for (int i = 0; i < 10; ++i) h2.send(Dice::Roll{});
    h2.advance(std::chrono::milliseconds{1});
    CHECK(h2.model().rolls == roll_with_seed(used, 10));
    return 0;
}

int draws_are_ordered_and_progress() {
    // Two random effects in one batch draw in the order they appear, from
    // one stream: the second doesn't repeat the first.
    auto rolls = roll_with_seed(77, 40);
    bool any_different = false;
    for (std::size_t i = 1; i < rolls.size(); ++i)
        if (rolls[i] != rolls[0]) any_different = true;
    CHECK(any_different);                     // not the same number 40 times
    return 0;
}

int given_runs_random_from_a_fixed_seed() {
    // given is data-level: no kernel, but `random` still resolves, so a
    // test can assert on the exact face. given isn't copyable (chaining
    // mutates in place), so each run gets its own object.
    jaal::given<Dice> a;
    a.when(Dice::Roll{}).settle();
    jaal::given<Dice> b;
    b.when(Dice::Roll{}).settle();
    CHECK(a.ok());
    CHECK(a.model().rolls.size() == 1);
    CHECK(a.model().rolls == b.model().rolls);         // repeatable

    // with_seed picks a different stream, and the effect is consumed.
    jaal::given<Dice> c;
    c.with_seed(5).when(Dice::Roll{}).settle();
    CHECK(c.ok());
    CHECK(c.model().rolls.size() == 1);
    CHECK(c.expect_effect<jaal::fx::random>(0).ok());
    return 0;
}

int map_carries_the_draw() {
    // Cmd::map re-targets a random effect: the child's roll arrives wrapped.
    struct Wrapped { Dice::Msg inner; };
    using Parent = std::variant<Wrapped>;
    using PCmd = jaal::Cmd<Parent>;

    Dice::Model m;
    auto c = Dice::update(m, Dice::Roll{});
    PCmd p = std::move(c).map([](Dice::Msg d) -> Parent { return Wrapped{d}; });
    auto* r = std::get_if<jaal::fx::random::type<Parent>>(&p.inner);
    CHECK(r != nullptr);
    jaal::rng rr{3};
    Parent out = r->to_msg(rr);
    auto& w = std::get<Wrapped>(out);
    int face = std::get<Dice::Rolled>(w.inner).face;
    CHECK(face >= 1 && face <= 6);
    return 0;
}

}  // namespace

int main() {
    if (int r = sequence_is_fixed())                 return r;
    if (int r = same_seed_same_numbers())            return r;
    if (int r = below_is_in_range_and_spread())      return r;
    if (int r = in_covers_both_ends())               return r;
    if (int r = doubles_stay_in_range())             return r;
    if (int r = split_is_independent_but_copy_repeats()) return r;
    if (int r = pick_handles_empty())                return r;
    if (int r = effect_is_reproducible())            return r;
    if (int r = seed_is_reported())                  return r;
    if (int r = draws_are_ordered_and_progress())    return r;
    if (int r = given_runs_random_from_a_fixed_seed()) return r;
    if (int r = map_carries_the_draw())              return r;
    std::puts("rng: ok");
    return 0;
}
