// Compile-fail cases for guarded<T> and scope.

#include <jaal/kernel/guarded.hpp>
#include <jaal/kernel/scope.hpp>

#include <map>
#include <string>
#include <utility>

#if JAAL_CASE == 1
// a reference into the guarded data escaping the lock
int& leak(const jaal::unlocked& cap, jaal::guarded<std::map<std::string, int>>& g) {
    return g.with(cap, [](auto& m) -> int& { return m["a"]; });
}
#elif JAAL_CASE == 2
// a helper handle can't be moved out of the scope to outlive it
void f(const jaal::unlocked& cap) {
    jaal::scope(cap, [](jaal::nursery& n) {
        auto h = n.spawn([] { return 1; });
        auto stolen = std::move(h);
        (void)stolen;
    });
}
#elif JAAL_CASE == 3
// a nursery can't be made outside scope()
void f() { jaal::nursery n{std::stop_token{}}; }
#elif JAAL_CASE == 4
// reaching a second lock by capturing it
void f(const jaal::unlocked& cap, jaal::guarded<int>& a, jaal::guarded<int>& b) {
    a.with(cap, [&](int& x) { (void)b; ++x; });
}
#elif JAAL_CASE == 5
// ... or by passing it in as an argument
void f(const jaal::unlocked& cap, jaal::guarded<int>& a, jaal::guarded<int>& b) {
    a.with(cap, [](int&, jaal::guarded<int>* other) { (void)other; }, &b);
}
#else
#  error "unknown JAAL_CASE"
#endif
