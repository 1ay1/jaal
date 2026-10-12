#pragma once
// jaal::kernel::lock_order — the one wait the type system can't see.
//
// Lock order and "no blocking wait under a lock" are TYPES now
// (core/capability.hpp): taking a guarded needs a capability that proves the
// thread holds only lower-ranked locks, and every blocking wait needs
// `unlocked`. Programs that break either rule do not compile, so nothing is
// tracked at runtime for them.
//
// One deadlock shape is about IDENTITY, not rank, and stays a runtime check:
//
//   A job of pool P calls P.shutdown(). shutdown waits for every job of P
//   to finish, including the one calling it, which is waiting. The job is
//   on a fresh thread and legitimately holds `unlocked`, so the type of the
//   call is fine; only "which pool am I a job of" says it is wrong.
//
// Each job records its pool for its duration (running_job), and shutdown
// checks it before waiting. A violation throws jaal::lock_order_error at the
// call, before anything blocks, on every run that reaches it.
//
// thread_local is jaal-internal and allowlisted (tests/lint/allowlist.txt).

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace jaal {

/// A pool shut down from one of its own jobs: it would wait for itself.
struct lock_order_error : std::logic_error {
    using std::logic_error::logic_error;
};

}  // namespace jaal

namespace jaal::kernel::lock_order {

// A fixed-size stack, NOT a std::vector: thread_local objects with
// destructors are torn down at thread exit, and a static destructor or a
// detached thread can still run a job after that. Trivially destructible
// storage is alive for the thread's whole life.
template <class T, std::size_t N>
struct fixed_stack {
    T           items[N];
    std::size_t n = 0;

    bool push(const T& v) noexcept {
        if (n == N) return false;
        items[n++] = v;
        return true;
    }
    void pop() noexcept { if (n) --n; }
    [[nodiscard]] const T* begin() const noexcept { return items; }
    [[nodiscard]] const T* end() const noexcept { return items + n; }
};

// Deeper nesting than this (a job of a pool running a job of another pool,
// inline, sixteen deep) is a design problem in itself; past it the identity
// isn't tracked, never a crash.
inline constexpr std::size_t kMaxNested = 16;

/// The pools whose job the current thread is running, innermost last (a job
/// can run another pool's job inline, so it's a stack).
inline fixed_stack<const void*, kMaxNested>& running_for() {
    thread_local fixed_stack<const void*, kMaxNested> r;
    return r;
}

class running_job {
public:
    explicit running_job(const void* pool) : pushed_(running_for().push(pool)) {}
    ~running_job() { if (pushed_) running_for().pop(); }
    running_job(const running_job&)            = delete;
    running_job& operator=(const running_job&) = delete;

private:
    bool pushed_;
};

/// shutdown() of `pool` from one of its own jobs would wait for itself.
inline void require_not_own_job(const void* pool, const char* what) {
    for (const void* p : running_for())
        if (p == pool)
            throw lock_order_error(
                std::string("jaal: ") + what +
                " called from one of that pool's own jobs; it would wait for "
                "itself. Stop it from its owner instead.");
}

/// For noexcept callers (destructors, worker_group::stop): the same check,
/// but a violation prints the reason and aborts. Still loud and immediate,
/// which beats a hang with no output.
inline void check_or_abort(const void* pool, const char* what) noexcept {
    try {
        require_not_own_job(pool, what);
    } catch (const lock_order_error& e) {
        std::fprintf(stderr, "%s\n", e.what());
        std::abort();
    }
}

}  // namespace jaal::kernel::lock_order
