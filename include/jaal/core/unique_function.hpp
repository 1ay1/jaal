#pragma once
// jaal::unique_function<Sig> — a move-only callable, on every standard library.
//
// std::move_only_function is a C++23 <functional> LIBRARY feature. libstdc++
// has shipped it since GCC 12; libc++ has not, as of libc++ 23 — not on
// Android/Termux, not on older Apple toolchains, and not on llvm-mingw, which
// is how jaal builds for Windows. `-std=c++26` alone therefore does not make
// the type exist, and code that names it fails to compile on exactly the
// platforms nobody happens to be building on.
//
// jaal needs a move-only callable because owned work captures owned things:
// a unique_ptr, a promise, a response handle (pool::job). std::function
// rejects every one of those — it requires a copyable target.
//
// So: alias the standard type where it exists, and provide the minimal
// type-erased equivalent where it doesn't. maya has the same shim
// (maya/core/function.hpp), but jaal sits BELOW maya in the dependency order
// and cannot include it; this is jaal's own, with the same surface.
//
// Surface, and only this (it is what jaal uses): construct from any callable,
// move-only, operator(), explicit operator bool, nullptr assign/compare. Only
// the plain R(Args...) signature form — no cv/ref/noexcept-qualified ones.

#include <version>

#if defined(__cpp_lib_move_only_function)

#include <functional>

namespace jaal {
template <class Sig>
using unique_function = std::move_only_function<Sig>;
}  // namespace jaal

#else

#include <concepts>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace jaal {

template <class Sig>
class unique_function;

template <class R, class... Args>
class unique_function<R(Args...)> {
    struct callable {
        virtual ~callable()             = default;
        virtual R invoke(Args&&... args) = 0;
    };
    template <class F>
    struct model final : callable {
        F fn;
        explicit model(F f) : fn(std::move(f)) {}
        R invoke(Args&&... args) override {
            // static_cast<R> also covers R = void and callables whose return
            // merely converts to R.
            return static_cast<R>(fn(std::forward<Args>(args)...));
        }
    };

    std::unique_ptr<callable> impl_;

public:
    unique_function() noexcept = default;
    unique_function(std::nullptr_t) noexcept {}

    template <class F>
        requires(!std::same_as<std::remove_cvref_t<F>, unique_function>
                 && std::invocable<std::decay_t<F>&, Args...>)
    unique_function(F&& f)
        : impl_(std::make_unique<model<std::decay_t<F>>>(std::forward<F>(f))) {}

    unique_function(unique_function&&) noexcept            = default;
    unique_function& operator=(unique_function&&) noexcept = default;
    unique_function(const unique_function&)                = delete;
    unique_function& operator=(const unique_function&)     = delete;

    unique_function& operator=(std::nullptr_t) noexcept {
        impl_.reset();
        return *this;
    }

    // Precondition (as std::move_only_function): holds a target.
    R operator()(Args... args) { return impl_->invoke(std::forward<Args>(args)...); }

    [[nodiscard]] explicit operator bool() const noexcept { return impl_ != nullptr; }

    friend bool operator==(const unique_function& f, std::nullptr_t) noexcept {
        return f.impl_ == nullptr;
    }
};

}  // namespace jaal

#endif  // __cpp_lib_move_only_function
