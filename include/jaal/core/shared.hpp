#pragma once
// jaal::shared<T> — share an immutable value between threads, safely.
//
// The only built-in way in jaal to have two threads see the same object.
// It requires Shareable<T> = Sendable<T> && Frozen<T>:
//   * Frozen: nothing reachable from T can change through const access,
//     so readers never race with each other
//   * Sendable: T holds nothing borrowed, so nothing inside it can be
//     freed out from under a reader
// and it only ever hands out const access, so no writer exists at all.
//
// Lifetime: reference counted. A reader holding a shared<T> keeps the
// value alive. There is no reset(), no release(), no raw pointer out, and
// no way to be null:
//   * construction only through make(), which builds the value
//   * no default constructor
//   * MOVE COPIES. A moved-from std::shared_ptr is null; a moved-from
//     shared<T> is still valid and still points at the same value. So
//     "use after move" can't give a null dereference. (One atomic
//     increment per move is the price.)
//
// This fixes maya's theme-slot bug by construction: the slot held a raw
// pointer to a Theme someone else could destroy. A shared<const Theme>
// can't be destroyed while anyone holds it.
//
// shared<T> is itself Sendable and Frozen, so shared values nest:
//     struct Node { std::string label; std::vector<jaal::shared<Node>> kids; };

#include <concepts>
#include <memory>
#include <type_traits>
#include <utility>

#include "frozen.hpp"
#include "sendable.hpp"

namespace jaal {

template <class T>
concept Shareable = Sendable<T> && Frozen<T>;

template <class T>
    requires std::is_object_v<T> && (!std::is_const_v<T>) && (!std::is_volatile_v<T>)
class shared {
public:
    using element_type = T;

    /// Build a T in place and freeze it. The check lives HERE, not on the
    /// class, so shared<Node> can name itself inside Node (recursive trees)
    /// while Node is still incomplete.
    template <class... Args>
        requires std::constructible_from<T, Args...>
    [[nodiscard]] static shared make(Args&&... args) {
        require_shareable();
        return shared(std::make_shared<const T>(std::forward<Args>(args)...));
    }

    shared(const shared&) noexcept            = default;
    shared& operator=(const shared&) noexcept = default;

    // Move copies: the source stays valid (see header comment).
    shared(shared&& o) noexcept : p_(o.p_) {}
    shared& operator=(shared&& o) noexcept {
        p_ = o.p_;
        return *this;
    }

    ~shared() = default;

    [[nodiscard]] const T& get()        const noexcept { return *p_; }
    [[nodiscard]] const T& operator*()  const noexcept { return *p_; }
    [[nodiscard]] const T* operator->() const noexcept { return p_.get(); }

    /// Same object (not just equal values).
    [[nodiscard]] bool same_as(const shared& o) const noexcept { return p_ == o.p_; }

    /// Compare the VALUES, when T is comparable.
    friend bool operator==(const shared& a, const shared& b)
        requires std::equality_comparable<T>
    {
        return a.p_ == b.p_ || *a.p_ == *b.p_;
    }

private:
    explicit shared(std::shared_ptr<const T> p) noexcept : p_(std::move(p)) {}

    static consteval void require_shareable() {
        require_sendable<T>();
        require_frozen<T>();
    }

    std::shared_ptr<const T> p_;
};

// shared<T> is safe to move and share for ANY T it can be built from,
// because make() is the only way to build one and make() checks T.
template <class T> inline constexpr bool sendable_opt_in<shared<T>> = true;
template <class T> inline constexpr bool frozen_opt_in<shared<T>>   = true;

}  // namespace jaal
