#pragma once
// jaal::co_owned<T> — state that background jobs share, and its owner too.
//
// Jobs are captureless and take Sendable arguments, so a job can't borrow
// `this` or a local. When several threads really do need the same object,
// it goes in a co_owned<T>: reference counted, so nobody frees it under a
// job, and T must be Sync, so using it from all of them at once is safe.
//
//   struct Conn { const std::string url; jaal::guarded<State> st; };
//   auto c = jaal::co_owned<Conn>::make(url);
//   pool.post([](std::stop_token, jaal::co_owned<Conn> c) { ... }, c);
//
// Like shared<T>: built only by make() (which checks Sync<T>), never null,
// and a move copies, so a moved-from handle still works. Unlike shared<T>
// it hands out T&, because Sync is what makes that safe.

#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>

#include "sendable.hpp"
#include "sync.hpp"

namespace jaal {

template <class T>
    requires std::is_object_v<T> && (!std::is_const_v<T>) && (!std::is_volatile_v<T>)
class co_owned {
public:
    using element_type = T;

    template <class... Args>
        requires std::constructible_from<T, Args...>
    [[nodiscard]] static co_owned make(Args&&... args) {
        require_sync<T>();
        return co_owned(std::make_shared<T>(std::forward<Args>(args)...));
    }

    /// The handle to an object make() built, from inside it. T derives from
    /// std::enable_shared_from_this<T> (so it's a class jaal can't walk, and
    /// is Sync only by opt-in). Throws std::bad_weak_ptr when `self` wasn't
    /// built by make().
    [[nodiscard]] static co_owned of(T& self)
        requires std::derived_from<T, std::enable_shared_from_this<T>>
    {
        require_sync<T>();
        return co_owned(self.shared_from_this());
    }

    co_owned(const co_owned&) noexcept            = default;
    co_owned& operator=(const co_owned&) noexcept = default;
    co_owned(co_owned&& o) noexcept : p_(o.p_) {}
    co_owned& operator=(co_owned&& o) noexcept {
        p_ = o.p_;
        return *this;
    }
    ~co_owned() = default;

    [[nodiscard]] T& get()        const noexcept { return *p_; }
    [[nodiscard]] T& operator*()  const noexcept { return *p_; }
    [[nodiscard]] T* operator->() const noexcept { return p_.get(); }

private:
    explicit co_owned(std::shared_ptr<T> p) noexcept : p_(std::move(p)) {}
    std::shared_ptr<T> p_;
};

// Safe to move for any T it can be built from: make() and of() check T.
template <class T> inline constexpr bool sendable_opt_in<co_owned<T>> = true;
template <class T> inline constexpr bool sync_when_const<co_owned<T>> = true;

}  // namespace jaal
