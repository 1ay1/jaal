#pragma once
// jaal::kernel::loop_token / loop_bound — the one-thread rule as a type.
//
// maya's real safety net is that almost everything runs on one thread: 68
// thread_locals say so in comments, and nothing enforces it. If a worker
// ever called the renderer it would silently get its own empty caches —
// a wrong answer, which is worse than a crash.
//
// loop_bound<T> makes that a type. The data is only reachable with a
// loop_token, and a token:
//   * is created ONLY by the kernel (private ctor + kernel_key)
//   * can't be copied or moved, so it can't be stored or captured
//   * is never handed to a task body (which is captureless anyway)
//   * has NO static or global accessor. That matters: a captureless lambda
//     can still call any function, so a loop_token::current() would hand
//     worker code a token. There isn't one.
//
// So code running on a worker has no way to name loop-bound state.

#include <utility>

namespace jaal::kernel {

/// The kernel's key for minting tokens. Tests use it deliberately.
struct loop_key {
    explicit loop_key() = default;
};

/// Proof that the holder is running on the loop thread.
class loop_token {
public:
    explicit loop_token(loop_key) noexcept {}

    loop_token(const loop_token&)            = delete;
    loop_token(loop_token&&)                 = delete;
    loop_token& operator=(const loop_token&) = delete;
    loop_token& operator=(loop_token&&)      = delete;
};

/// State only the loop thread may touch.
template <class T>
class loop_bound {
public:
    loop_bound() = default;

    template <class... Args>
        requires std::is_constructible_v<T, Args...>
    explicit loop_bound(Args&&... args) : value_(std::forward<Args>(args)...) {}

    [[nodiscard]] T&       get(const loop_token&) noexcept       { return value_; }
    [[nodiscard]] const T& get(const loop_token&) const noexcept { return value_; }

    /// Run f with the value, on the loop.
    template <class F>
    decltype(auto) with(const loop_token& t, F&& f) {
        return std::forward<F>(f)(get(t));
    }

private:
    T value_{};
};

}  // namespace jaal::kernel
