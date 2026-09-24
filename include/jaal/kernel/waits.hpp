#pragma once
// jaal::kernel::waits — which threads are (transitively) waiting on this one.
//
// A blocking wait is a deadlock when the thread it waits FOR is, directly or
// through a chain, waiting on the waiter. jaal has exactly two kinds of
// wait that could close such a cycle:
//
//   * a scope owner joining its helpers (kernel/scope.hpp)
//   * a sender blocking on a full mailbox, which only the loop can drain
//     (kernel/mailbox.hpp, overflow::block)
//
// So the cycle to rule out is: the loop opens a scope and joins a helper,
// and that helper blocks sending to the loop's full mailbox. Each waits for
// the other. The mailbox already refuses to block the LOOP thread itself;
// this extends that to every thread the loop is waiting on.
//
// Each thread records the threads that are waiting on it: a helper inherits
// its owner's set plus the owner. A mailbox asks "is my loop in that set?"
// and, if it is, refuses the post (counted in mailbox_stats::loop_full)
// instead of waiting forever. The set is tiny (the depth of nested scopes).
//
// thread_local is jaal-internal and allowlisted (tests/lint/allowlist.txt).

#include <algorithm>
#include <thread>
#include <vector>

namespace jaal::kernel::waits_detail {

/// Threads waiting (transitively) on the current one.
inline std::vector<std::thread::id>& waiters() {
    thread_local std::vector<std::thread::id> w;
    return w;
}

/// Is `t` waiting on the current thread (directly or through a chain)?
inline bool waited_on_by(std::thread::id t) {
    const auto& w = waiters();
    return std::find(w.begin(), w.end(), t) != w.end();
}

/// The set a helper spawned from the current thread should start with.
inline std::vector<std::thread::id> for_helper() {
    auto w = waiters();
    w.push_back(std::this_thread::get_id());
    return w;
}

}  // namespace jaal::kernel::waits_detail
