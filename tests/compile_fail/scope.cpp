// Compile-fail cases for guarded<T> and scope.

#include <jaal/kernel/guarded.hpp>
#include <jaal/kernel/scope.hpp>

#include <map>
#include <string>
#include <utility>

#if JAAL_CASE == 1
// a reference into the guarded data escaping the lock
int& leak(jaal::guarded<std::map<std::string, int>>& g) {
    return g.with([](auto& m) -> int& { return m["a"]; });
}
#elif JAAL_CASE == 2
// a helper handle can't be moved out of the scope to outlive it
void f() {
    jaal::scope([](jaal::nursery& n) {
        auto h = n.spawn([] { return 1; });
        auto stolen = std::move(h);
        (void)stolen;
    });
}
#elif JAAL_CASE == 3
// a nursery can't be made outside scope()
void f() { jaal::nursery n{std::stop_token{}}; }
#elif JAAL_CASE == 4
// taking a second lock while holding the first (lock-order deadlock), by
// capturing it
void f(jaal::guarded<int>& a, jaal::guarded<int>& b) {
    a.with([&](int& x) { b.with([](int& y) { ++y; }); ++x; });
}
#elif JAAL_CASE == 5
// ... or by passing it in as an argument
void f(jaal::guarded<int>& a, jaal::guarded<int>& b) {
    a.with([](int&, jaal::guarded<int>* other) { other->with([](int&) {}); }, &b);
}
#else
#  error "unknown JAAL_CASE"
#endif
