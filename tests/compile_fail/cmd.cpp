// Compile-fail cases for Cmd, rows and tasks. Each must NOT compile, and
// the ones with MATCH must fail for the stated reason.

#include <jaal/core/core_fx.hpp>
#include <jaal/core/fx.hpp>

#include <chrono>
#include <stop_token>
#include <string>
#include <string_view>

using namespace std::chrono_literals;
using jaal::Cmd;
using jaal::Sink;
using jaal::make_row;
namespace fx = jaal::fx;

struct Msg { int v; };
struct Beep {};
using beep = jaal::pure_fx<Beep, "beep">;
using C = jaal::CoreCmd<Msg>;

#if JAAL_CASE == 1
// an effect that's not in the row
C c = Beep{};
#elif JAAL_CASE == 2
// narrowing a Cmd to fewer effects
Cmd<Msg, make_row<fx::after>> small = C::quit();
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
#else
#  error "unknown JAAL_CASE"
#endif
