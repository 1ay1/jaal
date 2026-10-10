#pragma once
// jaal::kernel::lock_order — the two ways guarded<T> could deadlock, made
// errors that fire every time, not only when the timing is unlucky.
//
// A deadlock needs a thread to hold a lock while it waits for something.
// guarded<T> already rules out the usual way in: its body is captureless and
// takes only Sendable arguments, so it can't name a second guarded. What's
// left is a body that reaches one through a global or a static, or a thread
// that blocks (joins, waits for a pool, waits on another guarded) while it
// holds one. Both are checked here, on every call:
//
//   1. Levels. Every guarded has a level. A thread may only take a lock
//      whose level is HIGHER than every lock it already holds. Two locks at
//      the same level are never held together, so a cycle can't form: it
//      would need a lock taken while holding one at its own level or above.
//      A plain guarded is a LEAF: it has the top level, so it can be taken
//      inside any outer lock but nothing can be taken inside it, and two
//      leaves are never held together. The few locks whose body takes
//      another lock say so with a lower level.
//
//   2. No waiting with a lock held. wait_with on a guarded the thread
//      doesn't hold, a pool/worker_group shutdown, and a scope join all
//      call require_no_locks_held() first.
//
// A violation throws jaal::lock_order_error at the call that would have
// risked the deadlock, naming both locks, before anything blocks. It's a
// programming error, like an out-of-range index: tests that cover a path
// prove that path can't deadlock this way.
//
// thread_local is jaal-internal and allowlisted (tests/lint/allowlist.txt).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <cstddef>

namespace jaal {

/// A lock taken out of order, or a blocking wait with a lock held.
struct lock_order_error : std::logic_error {
    using std::logic_error::logic_error;
};

/// How a guarded ranks against others. Higher levels are taken later.
/// The default is a leaf: taken last, never with another leaf.
struct lock_level {
    static constexpr std::uint16_t leaf = 0xFFFF;
    std::uint16_t value = leaf;
    const char*   name  = "guarded";
};

}  // namespace jaal

namespace jaal::kernel::lock_order {

struct held_lock {
    const void*   id;
    std::uint16_t level;
    const char*   name;
};

// A fixed-size stack, NOT a std::vector: thread_local objects with
// destructors are torn down at thread exit, and a static destructor or a
// detached thread can still take a guarded after that. Trivially
// destructible storage is alive for the thread's whole life.
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
    [[nodiscard]] bool empty() const noexcept { return n == 0; }
    [[nodiscard]] const T& back() const noexcept { return items[n - 1]; }
    [[nodiscard]] const T* begin() const noexcept { return items; }
    [[nodiscard]] const T* end() const noexcept { return items + n; }
};

// Deeper nesting than this is a design problem in itself; past it the
// order isn't tracked, never a crash.
inline constexpr std::size_t kMaxHeld = 16;

/// The guarded locks the current thread holds, innermost last.
inline fixed_stack<held_lock, kMaxHeld>& held() {
    thread_local fixed_stack<held_lock, kMaxHeld> h;
    return h;
}

/// A forked child has one thread, a copy of the forking one, and holds
/// nothing it could release: the parent's locks aren't its own. A child
/// that keeps running C++ (rather than exec'ing at once) and takes guarded
/// locks calls this first.
inline void forget_held_after_fork() noexcept { held().n = 0; }

/// Called before taking `id`. Throws if it would break the order.
inline void before_acquire(const void* id, lock_level lv) {
    const auto& h = held();
    for (const auto& l : h) {
        if (l.id == id)
            throw lock_order_error(std::string("jaal: guarded '") + lv.name +
                                   "' locked again on the thread that holds it "
                                   "(it isn't recursive: this would deadlock)");
        if (l.level >= lv.value)
            throw lock_order_error(
                std::string("jaal: guarded '") + lv.name + "' (level " +
                std::to_string(lv.value) + ") locked while holding '" + l.name +
                "' (level " + std::to_string(l.level) +
                "). A lock may only be taken while holding lower-level ones; "
                "give the inner one a higher lock_level, or don't nest them.");
    }
}

/// RAII: the current thread holds `id` for this scope.
class hold {
public:
    hold(const void* id, lock_level lv) : pushed_(held().push({id, lv.value, lv.name})) {}
    ~hold() { if (pushed_) held().pop(); }
    hold(const hold&)            = delete;
    hold& operator=(const hold&) = delete;

private:
    bool pushed_;
};

/// Called before anything that blocks until ANOTHER thread acts: a join, a
/// pool shutdown, a wait on a guarded this thread doesn't hold.
inline void require_no_locks_held(const char* what) {
    const auto& h = held();
    if (h.empty()) return;
    throw lock_order_error(std::string("jaal: ") + what + " while holding guarded '" +
                           h.back().name +
                           "'. Blocking with a lock held deadlocks the moment "
                           "the thread you wait for needs that lock; do the "
                           "wait outside the guarded body.");
}

/// The pools whose job the current thread is running (a job can post to
/// and run inside another pool's job, so it's a stack).
inline fixed_stack<const void*, kMaxHeld>& running_for() {
    thread_local fixed_stack<const void*, kMaxHeld> r;
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

/// Called by a pool's shutdown: waiting for a pool from one of its own jobs
/// waits for itself.
inline void require_not_own_job(const void* pool, const char* what) {
    for (const void* p : running_for())
        if (p == pool)
            throw lock_order_error(std::string("jaal: ") + what +
                                   " called from one of that pool's own jobs: it would "
                                   "wait for itself. Stop it from its owner instead.");
}

/// For noexcept callers (destructors, worker_group::stop): the same checks,
/// but a violation prints the reason and aborts. Still loud and immediate,
/// which beats a hang with no output.
inline void check_or_abort(const void* pool, const char* what) noexcept {
    try {
        require_not_own_job(pool, what);
        require_no_locks_held(what);
    } catch (const lock_order_error& e) {
        std::fprintf(stderr, "%s\n", e.what());
        std::abort();
    }
}

}  // namespace jaal::kernel::lock_order
