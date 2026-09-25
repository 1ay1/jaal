// tests/core/loop_bound_test.cpp — loop_bound<T> and loop_token.
//
// The POSITIVE side of the loop_bound compile-fail cases.
//
// A compile-fail test proves the bad thing doesn't build. It does not prove
// the good thing does — and a case that fails because of a typo passes just
// as happily. This is the control: the legitimate uses must compile and
// work, so those four negatives are about the GUARANTEE, not about a
// misspelling.
//
// Compiling is most of passing; main() checks the runtime half.

#include <jaal/kernel/loop.hpp>

#include <type_traits>
#include <utility>

using jaal::kernel::loop_bound;
using jaal::kernel::loop_key;
using jaal::kernel::loop_token;

// ── the type-level facts the compile_fail cases rest on ──────────────────
// Asserting them HERE means a refactor that makes a token copyable turns
// those cases green-for-the-wrong-reason and breaks this at the same time.
static_assert(!std::is_copy_constructible_v<loop_token>,
              "a copyable token could be stashed and used off the loop");
static_assert(!std::is_move_constructible_v<loop_token>,
              "a movable token could be captured into a task body");
static_assert(!std::is_default_constructible_v<loop_token>,
              "a default-constructible token would be no proof at all");
static_assert(!std::is_convertible_v<int, loop_key>,
              "loop_key must not be forgeable from a literal");

// loop_bound itself is ordinary storage: constructible, and NOT copyable
// along with its value if T isn't — the restriction is on ACCESS, not on
// holding one.
static_assert(std::is_default_constructible_v<loop_bound<int>>);
static_assert(std::is_constructible_v<loop_bound<int>, int>);

int main() {
    // The kernel's own path: it can name loop_key, so it can mint proof.
    loop_bound<int> state{41};
    loop_token tok{loop_key{}};

    if (state.get(tok) != 41) return 1;

    state.get(tok) = 42;
    if (state.get(tok) != 42) return 2;

    // const access through the same proof
    const auto& cstate = state;
    if (cstate.get(tok) != 42) return 3;

    // with(): run something on the loop against the value
    state.with(tok, [](int& v) { v += 10; });
    if (state.get(tok) != 52) return 4;

    // A non-trivial T works the same way — the guard is on reaching the
    // value, not on what the value is.
    loop_bound<std::pair<int, int>> pair_state{std::pair{1, 2}};
    if (pair_state.get(tok).second != 2) return 5;

    return 0;
}
