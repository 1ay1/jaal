// Compile-fail cases for guarded<T> and scope.

#include <jaal/kernel/guarded.hpp>
#include <jaal/kernel/scope.hpp>

#include <atomic>
#include <map>
#include <string>
#include <string_view>
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
#elif JAAL_CASE == 6
// a scope helper borrowing a block local: the use-after-scope ASan caught,
// where a pointer to a later local reached a helper through a captured slot
void f(const jaal::unlocked& cap) {
    jaal::scope(cap, [](jaal::nursery& n) {
        std::atomic<int*> p{nullptr};
        auto h = n.spawn([&p] { if (int* q = p.load()) (void)*q; });
        { int x = 42; p.store(&x); }
    });
}
#elif JAAL_CASE == 7
// ... or getting a pointer in as an argument
void f(const jaal::unlocked& cap) {
    jaal::scope(cap, [](jaal::nursery& n) {
        int x = 0;
        auto h = n.spawn([](int* p) { ++*p; }, &x);
    });
}
#elif JAAL_CASE == 8
// a helper handed its own nursery could spawn or join from another thread
void f(const jaal::unlocked& cap) {
    jaal::scope(cap, [](jaal::nursery& n) {
        auto h = n.spawn([](jaal::nursery* self) { (void)self; }, &n);
    });
}
#elif JAAL_CASE == 9
// a view into the owner's string is a borrow too
void f(const jaal::unlocked& cap) {
    const std::string s = "abc";
    jaal::scope(cap, [&](jaal::nursery& n) {
        auto h = n.spawn([](std::string_view v) { return v.size(); }, std::string_view(s));
    });
}
#elif JAAL_CASE == 10
// only parallel_for can make the internal loan, so it can't smuggle a pointer
struct P { int* p; };
void f() { jaal::detail::scope::loan<P> l{P{nullptr}, 1}; (void)l; }
#else
#  error "unknown JAAL_CASE"
#endif
