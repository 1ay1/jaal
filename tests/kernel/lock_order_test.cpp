// jaal capabilities: the orders that ARE allowed compile and run.
//
// The orders that are NOT allowed don't compile; tests/compile_fail/
// capability.cpp pins each one with the exact diagnostic. This file proves
// the other half: the type rules don't reject anything that's correct, and
// the runtime behaviour (exclusion, waking, the self-join check) still holds.

#include <jaal/kernel/guarded.hpp>
#include <jaal/kernel/lock_order.hpp>
#include <jaal/kernel/pool.hpp>
#include <jaal/kernel/scope.hpp>
#include <jaal/kernel/worker_group.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <stop_token>
#include <type_traits>
#include <vector>

namespace {

int failures = 0;
void ok(bool c, const char* what) {
    if (!c) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

using outer_rank = jaal::rank<10, "test.outer">;
using mid_rank   = jaal::rank<20, "test.mid">;
using inner_rank = jaal::rank<30, "test.inner">;

jaal::guarded<int, outer_rank> outer{0};
jaal::guarded<int, mid_rank>   mid{0};
jaal::guarded<int, inner_rank> inner{0};
jaal::guarded<int>             leaf_a{0};

// A function that takes locks of rank >= mid declares so in its signature.
int bump_mid_then_leaf(const jaal::below<mid_rank>& cap) {
    return mid.with(cap, [](const jaal::held<mid_rank>& h, int& m) {
        ++m;
        return leaf_a.with(h, [](int& v) { return ++v; });
    });
}

// ── static facts about the types ────────────────────────────────────────
// Capabilities never move, copy, or cross threads.
static_assert(!std::is_copy_constructible_v<jaal::unlocked>);
static_assert(!std::is_move_constructible_v<jaal::unlocked>);
static_assert(!std::is_copy_constructible_v<jaal::held<mid_rank>>);
static_assert(!std::is_move_constructible_v<jaal::below<mid_rank>>);
static_assert(!jaal::Sendable<jaal::unlocked>);
static_assert(!jaal::Sendable<jaal::held<mid_rank>>);
static_assert(!jaal::Sendable<jaal::below<mid_rank>>);
static_assert(!jaal::Sendable<jaal::guarded<int, mid_rank>>);
// Nobody outside jaal can make one.
static_assert(!std::is_default_constructible_v<jaal::unlocked>);
static_assert(!std::is_default_constructible_v<jaal::held<mid_rank>>);
// The order, as conversions:
static_assert(std::is_convertible_v<const jaal::unlocked&, jaal::below<outer_rank>>);
static_assert(std::is_convertible_v<const jaal::unlocked&, jaal::below<jaal::leaf>>);
static_assert(std::is_convertible_v<const jaal::held<outer_rank>&, jaal::below<inner_rank>>);
static_assert(std::is_convertible_v<const jaal::held<outer_rank>&, jaal::below<jaal::leaf>>);
static_assert(!std::is_convertible_v<const jaal::held<inner_rank>&, jaal::below<outer_rank>>);
static_assert(!std::is_convertible_v<const jaal::held<mid_rank>&, jaal::below<mid_rank>>);
static_assert(!std::is_convertible_v<const jaal::held<jaal::leaf>&, jaal::below<jaal::leaf>>);
// A held lock is never "nothing held", so it can't wait.
static_assert(!std::is_convertible_v<const jaal::held<outer_rank>&, const jaal::unlocked&>);
// A weaker promise passes on; a stronger one can't be invented.
static_assert(std::is_convertible_v<const jaal::below<outer_rank>&, jaal::below<inner_rank>>);
static_assert(!std::is_convertible_v<const jaal::below<inner_rank>&, jaal::below<outer_rank>>);
// Joining objects are never lock contents.
static_assert(!jaal::JoinFree<jaal::kernel::pool>);
static_assert(!jaal::JoinFree<std::unique_ptr<jaal::kernel::worker_group>>);
static_assert(!jaal::JoinFree<std::vector<std::shared_ptr<jaal::kernel::pool>>>);
static_assert(jaal::JoinFree<std::weak_ptr<jaal::kernel::pool>>);
static_assert(jaal::JoinFree<std::vector<int>>);

}  // namespace

int main() {
    jaal::thread_root([](const jaal::unlocked& cap) {
        // 1. Declared order, three deep, through the held capabilities.
        {
            const int got = outer.with(cap, [](const jaal::held<outer_rank>& h1, int& o) {
                o = 1;
                return mid.with(h1, [](const jaal::held<mid_rank>& h2, int& m) {
                    m = 2;
                    return inner.with(h2, [](const jaal::held<inner_rank>& h3, int& i) {
                        i = 3;
                        return leaf_a.with(h3, [](int& l) { l = 4; return l; });
                    });
                });
            });
            ok(got == 4, "1: outer -> mid -> inner -> leaf, in order");
        }

        // 2. Skipping ranks is fine (outer straight to inner).
        {
            const int got = outer.with(cap, [](const jaal::held<outer_rank>& h, int&) {
                return inner.read(h, [](const int& i) { return i; });
            });
            ok(got == 3, "2: outer -> inner skips mid");
        }

        // 3. A `below<R>` parameter: callable with nothing held, or from
        //    inside a lower lock.
        {
            const int a = bump_mid_then_leaf(cap);
            const int b = outer.with(cap, [](const jaal::held<outer_rank>& h, int&) {
                return bump_mid_then_leaf(h);
            });
            ok(a == 5 && b == 6, "3: below<mid> from unlocked and from outer");
        }

        // 4. Bodies that don't take the capability still work.
        {
            leaf_a.with(cap, [](int& v) { v = 0; });
            const int v = leaf_a.read(cap, [](const int& x) { return x; });
            ok(v == 0, "4: capability-free bodies");
        }

        // 5. A job asks for its own `unlocked` and takes a lock with it; the
        //    caller waits for the result with ITS unlocked.
        {
            jaal::kernel::pool p{2};
            p.post([](const jaal::unlocked& job_cap, std::stop_token) {
                leaf_a.with(job_cap, [](int& v) { v = 42; });
            });
            const int seen = leaf_a.wait_with(cap,
                [](const int& v) { return v == 42; }, [](int& v) { return v; });
            ok(seen == 42, "5: a job's own unlocked; cross-thread wait_with");

            auto fut = p.submit([](const jaal::unlocked& job_cap, std::stop_token, int add) {
                return mid.with(job_cap, [](int& m, int a) { return m += a; }, add);
            }, 100);
            ok(fut.get() >= 100, "5b: submit with a capability-taking body");
            (void)p.shutdown(cap, jaal::kernel::pool::no_deadline);
        }

        // 6. Plain jobs (no capability) are unchanged.
        {
            jaal::kernel::pool p{1};
            auto fut = p.submit([](std::stop_token, int x) { return x * 2; }, 21);
            ok(fut.get() == 42, "6: plain job body");
            (void)p.shutdown(cap);
        }

        // 7. scope(): helpers may ask for their own unlocked.
        {
            leaf_a.with(cap, [](int& v) { v = 0; });
            auto sum = jaal::scope(cap, [](jaal::nursery& n) {
                auto h1 = n.spawn([](const jaal::unlocked& hc) {
                    return leaf_a.with(hc, [](int& v) { return ++v; });
                });
                auto h2 = n.spawn([](const jaal::unlocked& hc, std::stop_token) {
                    return leaf_a.with(hc, [](int& v) { return ++v; });
                });
                auto h3 = n.spawn([] { return 100; });
                return h1.join() + h2.join() + h3.join();
            });
            ok(sum == 1 + 2 + 100, "7: scope helpers");
        }

        // 8. wait_with_for times out and still runs the body.
        {
            auto [v, held] = inner.wait_with_for(cap, std::chrono::milliseconds(10),
                [](const int& i) { return i == -1; }, [](int& i) { return i; });
            ok(!held && v == 3, "8: wait_with_for timeout");
        }

        // 9. try_with / try_read on a free lock.
        {
            auto a = outer.try_with(cap, [](int& o) { return o; });
            auto b = outer.try_read(cap, [](const int& o) { return o + 1; });
            ok(a && b && *b == *a + 1, "9: try_with/try_read on a free lock");
        }

        // 10. worker_group: jobs get unlocked, stop needs it.
        {
            jaal::kernel::worker_group g;
            g.post([](const jaal::unlocked& jc, std::stop_token) {
                inner.with(jc, [](int& i) { i = 7; });
            });
            g.stop(cap);
            ok(inner.read(cap, [](const int& i) { return i; }) == 7, "10: worker_group");
        }

        // 11. The one runtime check left: a pool's job shutting down its own
        //     pool would wait for itself.
        {
            static jaal::kernel::pool* the_pool = nullptr;
            auto p = std::make_unique<jaal::kernel::pool>(1);
            the_pool = p.get();
            auto fut = p->submit([](const jaal::unlocked& jc, std::stop_token) {
                try { (void)the_pool->shutdown(jc); }
                catch (const jaal::lock_order_error&) { return true; }
                return false;
            });
            ok(fut.get(), "11: pool::shutdown from its own job throws");
            (void)p->shutdown(cap, jaal::kernel::pool::no_deadline);
        }
        return 0;
    });

    if (failures) return 1;
    std::printf("capability: ok\n");
    return 0;
}
