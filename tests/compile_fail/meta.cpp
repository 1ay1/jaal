// Compile-fail cases for jaal::meta. Built one case at a time with
// -DJAAL_CASE=<n>; each case must NOT compile. See cmake/JaalCompileFail.cmake.

#include <jaal/meta.hpp>

namespace m = jaal::meta;
struct A {}; struct B {};

#if JAAL_CASE == 1
// at<> past the end
using x = m::at_t<m::list<A>, 1>;

#elif JAAL_CASE == 2
// concat of something that isn't a list
using x = m::concat_t<m::list<A>, B>;

#elif JAAL_CASE == 3
// JAAL_REQUIRE shows its message
JAAL_REQUIRE(m::unique<m::list<A, A>>, "jaal-test: duplicate effect in row");

#else
#  error "unknown JAAL_CASE"
#endif
