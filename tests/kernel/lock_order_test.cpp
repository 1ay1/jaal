// tests/kernel/lock_order_test.cpp — every deadlock shape guarded/pool/scope
// can form is an error at the call, raised before anything blocks.
//
// Each case below would HANG without the check (or hang only sometimes,
// which is worse). With it, the bad call throws lock_order_error every time,
// on a single thread, with no timing involved.
#include <jaal/core/co_owned.hpp>
#include <jaal/kernel/guarded.hpp>
#include <jaal/kernel/pool.hpp>
#include <jaal/kernel/scope.hpp>
#include <jaal/kernel/worker_group.hpp>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#if defined(__unix__) || defined(__APPLE__)
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace {
int failures = 0;
void ok(bool c, const char* what) {
    if (!c) { std::fprintf(stderr, "FAIL %s\n", what); ++failures; }
}
template <class F>
bool throws_order(F f) {
    try { f(); } catch (const jaal::lock_order_error&) { return true; }
    return false;
}

// Globals: a captureless body can still reach these, which is exactly how
// a real nested lock sneaks in.
jaal::guarded<int> leaf_a;
jaal::guarded<int> leaf_b;
jaal::guarded<int> outer{jaal::lock_level{1, "outer"}, 0};
jaal::guarded<int> inner{jaal::lock_level{2, "inner"}, 0};

}  // namespace

int main() {
    // 1. Two leaves (level 0) are never held together. A-then-B on one
    //    thread and B-then-A on another is the classic ABBA deadlock; here
    //    the FIRST nesting already throws, on one thread.
    ok(throws_order([] {
        leaf_a.with([](int&) { leaf_b.with([](int&) {}); });
    }), "1: two leaves nested");

    // 2. Same lock twice: guarded isn't recursive, so this would self-deadlock.
    ok(throws_order([] {
        leaf_a.with([](int&) { leaf_a.read([](const int&) {}); });
    }), "2: re-entering the same guarded");

    // 3. Levels: outer(1) then inner(2) is the declared order and works ...
    {
        bool ran = false;
        try {
            ran = outer.with([](int&) { return inner.with([](int&) { return true; }); });
        } catch (...) {}
        ok(ran, "3a: lower then higher level is allowed");
    }
    // ... and the reverse throws.
    ok(throws_order([] {
        inner.with([](int&) { outer.with([](int&) {}); });
    }), "3b: higher then lower level");

    // 4. The check doesn't leak: after a throw, the thread holds nothing.
    {
        bool fine = true;
        try { leaf_a.with([](int&) {}); leaf_b.with([](int&) {}); } catch (...) { fine = false; }
        ok(fine, "4: unwinding released the held-lock record");
    }

    // 4b. A leaf (the default) may be taken inside a declared outer lock.
    {
        bool ran = false;
        try { ran = outer.with([](int&) { return leaf_a.with([](int&) { return true; }); }); }
        catch (...) {}
        ok(ran, "4b: a leaf inside a leveled lock is allowed");
    }

#if defined(__unix__) || defined(__APPLE__)
    // 4c. A child forked from inside a lock starts holding nothing.
    {
        const int status = leaf_a.with([](int&) {
            const pid_t pid = ::fork();
            if (pid == 0) {
                jaal::kernel::lock_order::forget_held_after_fork();
                int rc = 0;
                try { leaf_b.with([](int&) {}); } catch (...) { rc = 1; }
                ::_exit(rc);
            }
            int st = 0;
            ::waitpid(pid, &st, 0);
            return st;
        });
        ok(WIFEXITED(status) && WEXITSTATUS(status) == 0, "4c: forked child holds nothing");
    }
#endif

    // 5. Waiting for another thread while holding a lock.
    ok(throws_order([] {
        leaf_a.with([](int&) {
            leaf_b.wait_with([](const int& v) { return v > 0; }, [](int&) {});
        });
    }), "5a: wait_with on one guarded while holding another");
    ok(throws_order([] {
        leaf_a.with([](int&) {
            jaal::scope([](jaal::nursery&) {});
        });
    }), "5b: scope() join while holding a guarded");
    {
        static jaal::kernel::pool* p5 = nullptr;
        jaal::kernel::pool p{1};
        p5 = &p;
        ok(throws_order([] {
            leaf_a.with([](int&) { (void)p5->shutdown(); });
        }), "5c: pool shutdown while holding a guarded");
    }

    // 6. A pool's job stopping its own pool would wait for itself.
    {
        auto pool_ptr = std::make_unique<jaal::kernel::pool>(1);
        static jaal::kernel::pool* the_pool = nullptr;
        the_pool = pool_ptr.get();
        auto fut = pool_ptr->submit([](std::stop_token) {
            return throws_order([] { (void)the_pool->shutdown(); });
        });
        ok(fut.get(), "6: pool::shutdown from its own job");
        (void)pool_ptr->shutdown(jaal::kernel::pool::no_deadline);
    }

    // 7. ... including from an isolated job of that pool (worker_group's
    //    jobs are these; its stop() is noexcept, so there it aborts with the
    //    same message rather than throwing).
    {
        static jaal::kernel::pool* the_pool = nullptr;
        jaal::kernel::pool p{1};
        the_pool = &p;
        auto fut = p.submit_isolated([](std::stop_token) {
            return throws_order([] { (void)the_pool->shutdown(); });
        });
        ok(fut.get(), "7: pool::shutdown from its own isolated job");
    }

    // 8. Ordinary use is untouched: plain with/read/wait_with across threads.
    {
        jaal::kernel::pool p{2};
        p.post([](std::stop_token) { leaf_a.with([](int& v) { v = 5; }); });
        const int seen = leaf_a.wait_with([](const int& v) { return v == 5; },
                                          [](int& v) { return v; });
        ok(seen == 5, "8: a normal cross-thread wait still works");
    }

    if (failures) return 1;
    std::printf("lock_order: ok\n");
    return 0;
}
