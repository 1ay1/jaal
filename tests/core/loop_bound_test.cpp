// tests/core/loop_bound_test.cpp — loop_bound<T>, loop_token, on_loop().
//
// The POSITIVE side of the loop_bound compile-fail cases.
//
// A compile-fail test proves the bad thing doesn't build. It does not prove
// the good thing does — and a case that fails because of a typo passes just
// as happily. This is the control: the legitimate uses must compile and
// work, so those seven negatives are about the GUARANTEE, not about a
// misspelling.
//
// Compiling is most of passing; main() checks the runtime half:
//
//   * a thread with no kernel is NOT the loop
//   * a kernel's own thread IS, for as long as the kernel lives
//   * the identity is disarmed again once the kernel is gone, so a pool
//     thread reused afterwards cannot inherit proof
//   * nesting (a kernel started inside another kernel's fold) doesn't
//     disarm the outer one on the way out

#include <jaal/jaal.hpp>
#include <jaal/kernel/loop.hpp>

#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

using jaal::kernel::loop_bound;
using jaal::kernel::loop_identity;
using jaal::kernel::loop_key;
using jaal::kernel::loop_token;
using jaal::kernel::on_loop;

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

// The fix for the forgeable-token hole. loop_key's ctor is PRIVATE, so a
// worker cannot mint proof even by naming the type in full — which is what
// `loop_token t{loop_key{}}` used to do, two characters past the case the
// old compile-fail test pinned.
static_assert(!std::is_default_constructible_v<loop_key>,
              "loop_key's ctor must stay private: naming it IS the forge");
static_assert(!std::is_constructible_v<loop_token, loop_key>
              || !std::is_default_constructible_v<loop_key>,
              "a token may only come from the kernel or the checked on_loop()");

// loop_bound itself is ordinary storage: constructible, and NOT copyable
// along with its value if T isn't — the restriction is on ACCESS, not on
// holding one.
static_assert(std::is_default_constructible_v<loop_bound<int>>);
static_assert(std::is_constructible_v<loop_bound<int>, int>);

// Pinned to a thread, so it must never ride into a worker as a task
// argument. The opt-out says so at the boundary.
static_assert(!jaal::Sendable<loop_bound<int>>,
              "loop_bound must not be Sendable: it is pinned to one thread");
static_assert(!jaal::Sendable<loop_token>,
              "a token is proof about ONE thread; it must not travel");

// ── the runtime half ─────────────────────────────────────────────────────

static loop_bound<std::string> g_cache{};
static int g_fold_checks = 0;   // set on the loop, read after it stops

struct Probe {};
struct Nested {};

struct App {
    using Model = int;
    using Msg   = std::variant<Probe, Nested>;
    using Cmd   = jaal::Cmd<Msg>;

    static Cmd init(Model& m) { m = 0; return Cmd::send(Msg{Probe{}}); }

    static Cmd update(Model& m, Probe) {
        // A fold runs on the loop thread, so loop-bound state is reachable
        // with no token threaded through the call chain.
        if (!jaal::kernel::on_loop()) return Cmd::quit();
        ++g_fold_checks;

        g_cache.with([](std::string& s) { s = "written in a fold"; });

        // A copy may leave; a reference may not (compile_fail CASE 6).
        const auto seen = g_cache.with([](const std::string& s) { return s; });
        if (seen == "written in a fold") ++g_fold_checks;

        // The explicit-token form still works, for kernel-ish code that
        // already holds proof.
        on_loop([](const loop_token& t) {
            g_cache.get(t) += "!";
        });
        if (g_cache.with([](const std::string& s) { return s.size(); }) == 18)
            ++g_fold_checks;

        m = 1;
        return Cmd::send(Msg{Nested{}});
    }

    static Cmd update(Model& m, Nested) {
        // A kernel started INSIDE another kernel's fold must not disarm the
        // outer identity when it goes away — hence a depth, not a bool.
        {
            jaal::headless<App2> inner;
            inner.run_until_idle();
        }
        if (jaal::kernel::on_loop()) ++g_fold_checks;   // still armed
        m = 2;
        return Cmd::quit();
    }

    // A trivial second program for the nesting check.
    struct App2 {
        using Model = int;
        using Msg   = std::variant<Probe>;
        using Cmd   = jaal::Cmd<Msg>;
        static Cmd init(Model& m) { m = 0; return Cmd::send(Msg{Probe{}}); }
        static Cmd update(Model& m, Probe) {
            if (!jaal::kernel::on_loop()) return Cmd::quit();
            m = 1;
            return Cmd::quit();
        }
    };
};

int main() {
    // The storage half works without any kernel at all, as long as proof
    // comes from an armed identity. This is also the kernel's own path.
    {
        loop_identity arm;                  // pretend to be the loop
        loop_bound<int> state{41};
        const loop_token tok = loop_identity::token();

        if (state.get(tok) != 41) return 1;
        state.get(tok) = 42;
        if (state.get(tok) != 42) return 2;

        const auto& cstate = state;
        if (cstate.get(tok) != 42) return 3;

        state.with(tok, [](int& v) { v += 10; });
        if (state.get(tok) != 52) return 4;

        // The token-free form, which checks for itself.
        state.with([](int& v) { v += 1; });
        if (state.get(tok) != 53) return 5;

        loop_bound<std::pair<int, int>> pair_state{std::pair{1, 2}};
        if (pair_state.get(tok).second != 2) return 6;

        if (!jaal::kernel::on_loop()) return 7;
    }

    // Disarmed again: an identity that leaked past its scope would let a
    // pool thread pass the check later.
    if (jaal::kernel::on_loop()) return 8;

    // A plain thread is not the loop.
    bool worker_saw_loop = true;
    std::thread([&] { worker_saw_loop = jaal::kernel::on_loop(); }).join();
    if (worker_saw_loop) return 9;

    // A real kernel arms its own thread for its whole lifetime — including
    // the window between steps, where a host calls view().
    {
        jaal::headless<App> h;
        h.run_until_idle();
        if (h.model() != 2) return 10;
    }
    if (g_fold_checks != 4) return 11;

    // And the kernel's identity is gone with it.
    if (jaal::kernel::on_loop()) return 12;

    return 0;
}
