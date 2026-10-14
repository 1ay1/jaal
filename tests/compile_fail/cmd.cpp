// Compile-fail cases for Cmd, rows and tasks. Each must NOT compile, and
// the ones with MATCH must fail for the stated reason.

#include <jaal/core/core_fx.hpp>
#include <jaal/core/fx.hpp>

#include <chrono>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>

using namespace std::chrono_literals;
using jaal::Sink;
using jaal::make_row;
namespace fx = jaal::fx;

struct Msg { int v; };
struct Beep {};
using beep = jaal::pure_fx<Beep, "beep">;
using C = jaal::Cmd<Msg>;

#if JAAL_CASE == 1
// an effect that's not in the row
C c = Beep{};
#elif JAAL_CASE == 2
// narrowing a Cmd to fewer effects (exact rows: basic_cmd)
jaal::basic_cmd<Msg, make_row<fx::after>> small = C::quit();
#elif JAAL_CASE == 3
// a task body that captures by reference
void f() {
    std::string local = "x";
    auto c = C::task([&](Sink<Msg>, std::stop_token, int) { (void)local; }, 1);
}
#elif JAAL_CASE == 4
// a task argument that borrows
void f(std::string_view v) {
    auto c = C::task([](Sink<Msg>, std::stop_token, std::string_view) {}, v);
}
#elif JAAL_CASE == 5
// mapping a Cmd that holds a task with a capturing mapper: the mapper
// would run on the worker thread
void f() {
    struct P { Msg m; int k; };
    int k = 3;
    auto c = C::task([](Sink<Msg>, std::stop_token) {});
    auto p = std::move(c).map([k](Msg m) { return P{m, k}; });
}
#elif JAAL_CASE == 6
// a Msg that isn't Sendable can't have a Sink
struct Bad { std::string_view s; };
Sink<Bad> s;
#elif JAAL_CASE >= 7 && JAAL_CASE <= 11
struct Ok { int v; };
struct Fail { std::string why; };
struct Other { std::string_view nope; };
using M2 = std::variant<Ok, Fail>;
using C2 = jaal::Cmd<M2>;
#  if JAAL_CASE == 7
// a perform body that captures
void f() {
    int k = 1;
    auto c = C2::perform<Ok>([k] { return k; });
}
#  elif JAAL_CASE == 8
// a perform argument that borrows
void f(std::string_view v) {
    auto c = C2::perform<Ok>([](std::string_view s) { return static_cast<int>(s.size()); }, v);
}
#  elif JAAL_CASE == 9
// Ok can't hold what the body returns
auto c = C2::perform<Ok>([] { return std::string("x"); });
#  elif JAAL_CASE == 10
// a body that can't be called with the arguments given
auto c = C2::perform<Ok>([](int a, int b) { return a + b; }, 1);
#  elif JAAL_CASE == 11
// attempt's Fail can't be built from an exception (needs an int)
struct NotFail { explicit NotFail(int c) : code(c) {} int code; };
using M3 = std::variant<Ok, NotFail>;
auto c = jaal::Cmd<M3>::attempt<Ok, NotFail>([] { return 1; });
#  endif
#else
#  error "unknown JAAL_CASE"
#endif
