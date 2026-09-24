// Compile-fail cases for Frozen and shared<T>. Each must NOT compile, and
// fail with a message that names the real problem.

#include <jaal/core/shared.hpp>

#include <memory>
#include <string>
#include <string_view>

struct WithMut   { int a; mutable int cache; };
struct HasUnique { std::unique_ptr<std::string> title; };
struct HasView   { std::string_view text; };
struct Doc       { std::string title; };

#if JAAL_CASE == 1
// sharing a type with a mutable field
auto x = jaal::shared<WithMut>::make();
#elif JAAL_CASE == 2
// sharing a type that owns through a pointer
auto x = jaal::shared<HasUnique>::make();
#elif JAAL_CASE == 3
// sharing a borrowed view: fails on Sendable first
auto x = jaal::shared<HasView>::make();
#elif JAAL_CASE == 4
// no way to change a shared value
void f() {
    auto d = jaal::shared<Doc>::make(Doc{"t"});
    d->title = "changed";
}
#elif JAAL_CASE == 5
// no null state: can't default-construct
jaal::shared<Doc> d;
#elif JAAL_CASE == 6
// can't get a raw pointer to hand to someone
void f() {
    auto d = jaal::shared<Doc>::make(Doc{"t"});
    Doc* p = d.operator->();
    (void)p;
}
#else
#  error "unknown JAAL_CASE"
#endif
