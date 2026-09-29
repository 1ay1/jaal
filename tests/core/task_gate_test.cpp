// tests/core/task_gate_test.cpp — the task rules hold, and are ASKABLE.
//
// Cmd::task enforces two safety rules that exist to make data races
// impossible: the body may not capture (so nothing is shared by reference
// with a worker) and every argument must be Sendable (so nothing borrowed
// crosses a thread). compile_fail.task_no_capture and .task_arg_sendable
// pin that bad code doesn't build.
//
// This file pins the OTHER half, which is easy to get wrong: how you ask.
//
// The rules are enforced by a static_assert inside an `explain` overload
// that stays VIABLE for bad input — deliberately, because the alternative
// is a "no matching function" dump listing every candidate, and a
// programmer who can't see which rule they broke will work around it
// rather than fix it. The consequence is that
//
//     requires { Cmd::task(body, args...); }        // ← always true!
//
// is NOT a way to test whether a task is well-formed. A requires-expression
// only reports substitution failure; a static_assert firing in an
// instantiated body is a hard error, not a substitution failure, so the
// probe says "yes" and then the build breaks anyway if you call it.
//
// That is a trap worth a test, because the honest question has a real
// answer: ask the CONCEPTS directly. TaskBody<Body, Msg, Args...> and
// Sendable<Arg> are public, and they compose to exactly the constraint the
// good overload carries. Anything generic over task-ness must use those.

#include <jaal/core/cmd.hpp>
#include <jaal/core/core_fx.hpp>
#include <jaal/core/sink.hpp>

#include <string>
#include <string_view>
#include <variant>

struct Tick { int n; };
using Msg = std::variant<Tick>;
using Cmd = jaal::Cmd<Msg>;

// ── the shapes ───────────────────────────────────────────────────────────
struct Owned   { std::string s; };        // Sendable
struct Borrowed { std::string_view sv; }; // NOT Sendable: borrows

inline constexpr auto good_body     = [](jaal::Sink<Msg>, std::stop_token, Owned) {};
inline constexpr auto borrowed_body = [](jaal::Sink<Msg>, std::stop_token, Borrowed) {};

// ── the rules, asked the RIGHT way ───────────────────────────────────────
// This is the composition the constrained overload uses, and the one any
// generic code must use.
template <class Body, class... Args>
concept WellFormedTask =
    jaal::TaskBody<Body, Msg, Args...> && (jaal::Sendable<Args> && ...);

static_assert(WellFormedTask<decltype(good_body), Owned>,
              "an owned arg with a captureless body is a valid task");
static_assert(!WellFormedTask<decltype(borrowed_body), Borrowed>,
              "a borrowed arg must not be a valid task");

// Sendable is the part that makes a race impossible, and it is deep:
// a struct is only Sendable if every field is.
static_assert(jaal::Sendable<Owned>);
static_assert(!jaal::Sendable<Borrowed>);
static_assert(!jaal::Sendable<std::string_view>);
static_assert(!jaal::Sendable<int*>);
static_assert(jaal::Sendable<std::string>);

// A capturing body is refused however it is spelled.
void capture_check() {
    int local = 0;
    auto capturing = [&local](jaal::Sink<Msg>, std::stop_token, Owned) { (void)local; };
    static_assert(!WellFormedTask<decltype(capturing), Owned>,
                  "a capturing body must not be a valid task");
}

// ── the trap, pinned ─────────────────────────────────────────────────────
// If this ever becomes false, the `explain` overload stopped being viable
// for bad input. That would be an IMPROVEMENT (the probe would start
// telling the truth), but it is a behaviour change worth noticing rather
// than discovering: it means the diagnostics were restructured, and the
// comment above plus jaal's docs need updating to match.
template <class Body, class... Args>
concept CallExpressionCompiles = requires(Body b, Args... a) {
    Cmd::task(b, a...);
};

static_assert(CallExpressionCompiles<decltype(good_body), Owned>);
static_assert(CallExpressionCompiles<decltype(borrowed_body), Borrowed>,
              "the explain overload is viable by design: a requires-probe "
              "CANNOT be used to test task well-formedness. Use "
              "TaskBody && Sendable... instead. If this assert fires, the "
              "overload set was restructured — update the docs.");

int main() { return 0; }
