// Compile-fail cases for jaal::Sendable. Each case must NOT compile, and
// the ones with MATCH must fail with a readable message naming the culprit.

#include <jaal/core/sendable.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

struct GotLine  { std::string text; };
struct BadLine  { std::string_view text; };
struct HiddenPtr { std::string s; const char* raw; };
class  Opaque   { int fd_ = -1; public: Opaque() = default; };

#if JAAL_CASE == 1
// a borrowed view hidden in a struct inside a Msg variant
consteval void f() { jaal::require_sendable<std::variant<GotLine, BadLine>>(); }
static_assert((f(), true));
#elif JAAL_CASE == 2
// a raw pointer hidden in a struct
consteval void f() { jaal::require_sendable<HiddenPtr>(); }
static_assert((f(), true));
#elif JAAL_CASE == 3
// shared mutable ownership
consteval void f() { jaal::require_sendable<std::vector<std::shared_ptr<int>>>(); }
static_assert((f(), true));
#elif JAAL_CASE == 4
// a class jaal can't see into, without an opt-in
consteval void f() { jaal::require_sendable<Opaque>(); }
static_assert((f(), true));
#else
#  error "unknown JAAL_CASE"
#endif
