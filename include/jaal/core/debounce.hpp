#pragma once
// jaal::debounce<T> / jaal::throttle — "settle down before you act".
//
// The pattern every interactive app needs and nobody enjoys writing: a user
// types 8 characters, and you want ONE search, after the typing stops, for
// the LAST text — not 8 searches, and definitely not the 3rd result landing
// after the 8th.
//
// These are plain VALUES you keep in the model, not effects. Per D25 the
// kernel only owns what it must: debouncing needs no clock of its own, no
// thread and no subscription lifecycle. It's `after` plus a counter, so it
// lives in core as data — which means it replays, folds in `given`, and
// costs nothing at runtime.
//
//   struct Model { jaal::debounce<std::string> query; std::vector<Hit> hits; };
//   struct Typed { std::string text; };
//   struct Fire  { std::uint64_t token; };     // carries the token back
//
//   static std::pair<Model, Cmd> update(Model m, Msg msg) {
//       if (auto* t = std::get_if<Typed>(&msg)) {
//           auto tok = m.query.set(t->text);            // newest wins
//           return {m, Cmd::after(200ms, Msg{Fire{tok}})};
//       }
//       if (auto* f = std::get_if<Fire>(&msg)) {
//           if (!m.query.ready(f->token)) return {m, Cmd::none()};  // superseded
//           return {m, search(m.query.value())};        // fires once
//       }
//   }
//
// The token is what makes it correct. Every keystroke invalidates the timers
// already in flight, so the 7 earlier `Fire`s are dropped by `ready()` and
// only the last one gets through. Comparing text instead ("is this still
// what's typed?") looks equivalent and isn't: type "ab", delete to "a", and
// the stale timer for "a" matches.
//
// The same token guards the RESULT of slow work, which is the other half of
// the bug (D23's "stale results" case):
//
//   return {m, Cmd::task([](Sink<Msg> s, std::stop_token, std::string q,
//                           std::uint64_t tok) {
//                  s.send(Results{tok, run_query(q)});
//              }, m.query.value(), tok)};
//   ...
//   if (!m.query.ready(r->token)) return {m, Cmd::none()};   // an old answer

#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>

#include "frozen.hpp"
#include "sendable.hpp"

namespace jaal {

/// A value whose changes are debounced by token. Frozen- and Sendable-clean
/// (a value and a counter), so it can sit in any model.
template <class T>
class debounce {
public:
    using token_type = std::uint64_t;

    debounce() = default;
    explicit debounce(T initial) : value_(std::move(initial)) {}

    /// Record a new value and invalidate everything in flight. Returns the
    /// token to put in the message you delay; only that token is `ready`.
    [[nodiscard]] token_type set(T v) {
        value_ = std::move(v);
        return ++token_;
    }

    /// Bump the token without changing the value: "everything in flight is
    /// stale now" (the user hit Escape, the panel closed).
    [[nodiscard]] token_type invalidate() noexcept { return ++token_; }

    /// Is this the token from the most recent set()? False for a superseded
    /// timer or a late result.
    [[nodiscard]] bool ready(token_type t) const noexcept {
        return t == token_ && t != 0;
    }

    [[nodiscard]] const T& value() const noexcept { return value_; }
    [[nodiscard]] token_type token() const noexcept { return token_; }

    /// True before the first set(): nothing has been typed yet.
    [[nodiscard]] bool empty() const noexcept { return token_ == 0; }

    bool operator==(const debounce&) const = default;

private:
    T          value_{};
    token_type token_ = 0;
};

/// Rate-limits by TIME rather than by quiet: let something through at most
/// once per interval, and remember whether anything was dropped so the
/// caller can run once more at the end (the trailing edge).
///
/// For a progress bar fed by a fast stream: redraw 30 times a second, not
/// 10,000, but don't lose the final 100%.
///
///   struct Model { jaal::throttle redraw{33ms}; };
///   if (m.redraw.allow(now)) return {m, repaint()};
///   return {m, Cmd::none()};                 // dropped; pending() is true
///
/// `now` comes from the kernel (Cmd::now, or a host's frame time), never from
/// std::chrono directly: that's what keeps it testable and replayable.
class throttle {
public:
    using clock_type = std::chrono::steady_clock;
    using time_point = clock_type::time_point;
    using duration   = std::chrono::milliseconds;

    throttle() = default;
    explicit throttle(duration every) noexcept : every_(every) {}

    /// May something happen at `now`? True at most once per interval. When
    /// it returns false the attempt is remembered as pending.
    [[nodiscard]] bool allow(time_point now) noexcept {
        if (!last_ || now - *last_ >= every_) {
            last_    = now;
            pending_ = false;
            return true;
        }
        pending_ = true;
        return false;
    }

    /// Was anything dropped since the last allowed one? Check this when a
    /// burst ends, so the final state isn't the one that got thrown away.
    [[nodiscard]] bool pending() const noexcept { return pending_; }

    /// Take the pending flag: true once if anything was dropped.
    [[nodiscard]] bool take_pending() noexcept { return std::exchange(pending_, false); }

    /// When the next call could be allowed, or nullopt if one can be now.
    [[nodiscard]] std::optional<time_point> next_allowed() const noexcept {
        if (!last_) return std::nullopt;
        return *last_ + every_;
    }

    [[nodiscard]] duration interval() const noexcept { return every_; }
    void set_interval(duration d) noexcept { every_ = d; }

    /// Forget the history: the next allow() succeeds.
    void reset() noexcept {
        last_.reset();
        pending_ = false;
    }

    bool operator==(const throttle&) const = default;

private:
    duration                  every_{0};
    std::optional<time_point> last_;
    bool                      pending_ = false;
};

// Both hold private fields, so the structural walk can't see inside them
// (meta/fields.hpp: a class with private members isn't decomposable). They
// are opted in by hand, like shared<T>:
//
//   * debounce<T> owns a T and a counter. It's safe to move to another
//     thread, and immutable-by-value, EXACTLY WHEN its T is — so the
//     specialisations forward to T rather than asserting outright. A
//     debounce<shared_ptr<Doc>> is correctly still not Frozen.
//   * throttle holds a duration, an optional time_point and a bool: owned
//     values, nothing shared, nothing mutable.
template <class T>
inline constexpr bool sendable_opt_in<debounce<T>> = Sendable<T>;
template <class T>
inline constexpr bool frozen_opt_in<debounce<T>> = Frozen<T>;

template <> inline constexpr bool sendable_opt_in<throttle> = true;
template <> inline constexpr bool frozen_opt_in<throttle>   = true;

}  // namespace jaal
