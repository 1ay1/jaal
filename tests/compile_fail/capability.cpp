// Compile-fail cases for jaal's capabilities: every lock-order mistake, and
// every way to smuggle a capability, fails to build. Each case is one bug
// that a runtime checker could only catch if a test happened to run it.

#include <jaal/kernel/guarded.hpp>
#include <jaal/kernel/pool.hpp>
#include <jaal/kernel/scope.hpp>
#include <jaal/kernel/worker_group.hpp>

#include <functional>
#include <memory>
#include <stop_token>
#include <utility>

using outer_rank = jaal::rank<10, "test.outer">;
using inner_rank = jaal::rank<20, "test.inner">;

jaal::guarded<int, outer_rank> outer;
jaal::guarded<int, inner_rank> inner;
jaal::guarded<int>             leaf_a;
jaal::guarded<int>             leaf_b;

#if JAAL_CASE == 1
// higher rank, then lower: the ABBA half that deadlocks
void f(const jaal::unlocked& cap) {
    inner.with(cap, [](const jaal::held<inner_rank>& h, int&) {
        outer.with(h, [](int&) {});
    });
}
#elif JAAL_CASE == 2
// two leaves held together
void f(const jaal::unlocked& cap) {
    leaf_a.with(cap, [](const jaal::held<jaal::leaf>& h, int&) {
        leaf_b.with(h, [](int&) {});
    });
}
#elif JAAL_CASE == 3
// re-entering the lock you hold (guarded is not recursive)
void f(const jaal::unlocked& cap) {
    outer.with(cap, [](const jaal::held<outer_rank>& h, int&) {
        outer.read(h, [](const int&) {});
    });
}
#elif JAAL_CASE == 4
// a blocking wait under a lock
void f(const jaal::unlocked& cap) {
    outer.with(cap, [](const jaal::held<outer_rank>& h, int&) {
        inner.wait_with(h, [](const int& v) { return v > 0; }, [](int&) {});
    });
}
#elif JAAL_CASE == 5
// a scope join under a lock
void f(const jaal::unlocked& cap) {
    outer.with(cap, [](const jaal::held<outer_rank>& h, int&) {
        jaal::scope(h, [](jaal::nursery&) {});
    });
}
#elif JAAL_CASE == 6
// a pool shutdown under a lock (the pool is reachable through a function
// that was handed the held capability)
void stop_pool(const jaal::held<outer_rank>& h, jaal::kernel::pool& q) { (void)q.shutdown(h); }
#elif JAAL_CASE == 7
// a function declared to take locks of rank >= outer, called under inner
void takes_outer(const jaal::below<outer_rank>&) {}
void f(const jaal::unlocked& cap) {
    inner.with(cap, [](const jaal::held<inner_rank>& h, int&) { takes_outer(h); });
}
#elif JAAL_CASE == 8
// taking a lock with no capability at all
void f() { outer.with([](int&) {}); }
#elif JAAL_CASE == 9
// forging a capability
void f() {
    jaal::unlocked forged;
    outer.with(forged, [](int&) {});
}
#elif JAAL_CASE == 10
// copying a capability out of its frame
void f(const jaal::unlocked& cap) { jaal::unlocked copy = cap; (void)copy; }
#elif JAAL_CASE == 11
// keeping a capability's address
void f(const jaal::unlocked& cap) { auto* p = &cap; (void)p; }
#elif JAAL_CASE == 12
// sending a capability into a job
void f(jaal::kernel::pool& p, const jaal::unlocked& cap) {
    p.post([](std::stop_token, std::reference_wrapper<const jaal::unlocked>) {}, std::cref(cap));
}
#elif JAAL_CASE == 13
// capturing the outer capability into a lock body
void f(const jaal::unlocked& cap) {
    leaf_a.with(cap, [&cap](int&) { leaf_b.with(cap, [](int&) {}); });
}
#elif JAAL_CASE == 14
// a guarded that owns a joining object (dropping it would wait under the lock)
jaal::guarded<std::unique_ptr<jaal::kernel::worker_group>> slot;
#elif JAAL_CASE == 15
// a rank outside 1..65534
using bad_rank = jaal::rank<0, "zero">;
jaal::guarded<int, bad_rank> g;
#else
#  error "unknown JAAL_CASE"
#endif
