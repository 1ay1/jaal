#pragma once
// jaal::timeline<P> — step through a recorded run, both ways.
//
// A recorded run is the list of messages update() folded (kernel/replay.hpp,
// or a sim report). Because update() is pure, the model after message i is
// a function of the first i messages, so the whole run can be walked like a
// video: jump anywhere, step back, diff two points, bisect to the first bad
// model.
//
//   jaal::timeline<App> t(report.messages);
//   t.size();                          // steps: messages + 1 (the initial model)
//   const auto& m = t.at(40);          // model after 40 messages
//   t.message(39);                     // the message that produced it
//   t.changes(40);                     // field diff between step 39 and 40
//   auto bad = t.first_bad([](const auto& m) { return m.balance >= 0; });
//
// Cost: a model copy is kept every `stride` steps (default 64), so at(i)
// folds at most stride-1 messages from the nearest snapshot. Memory is
// size/stride models. Stepping to i+1 from i is one fold (the last model
// reached is cached).
//
// Effects are never run (same rule as replay): they are the run's outputs.
//
// The Model must be Frozen: a deep value, so a copy is a real snapshot.

#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../core/diff.hpp"
#include "../core/frozen.hpp"
#include "../core/program.hpp"

namespace jaal {

template <Program P>
    requires std::copy_constructible<typename P::Model>
          && std::copy_constructible<typename P::Msg>
class timeline {
    // Snapshots are copies. That's only a snapshot if the model is a VALUE:
    // a model holding shared_ptr<Doc> would have every snapshot share one
    // Doc, and stepping back would show today's Doc. Frozen rules that out
    // (no shared/unique owners, pointers, views or `mutable`).
    static_assert((require_frozen<typename P::Model>(), true));

public:
    using model_type = typename P::Model;
    using msg_type   = typename P::Msg;

    explicit timeline(std::vector<msg_type> msgs, std::size_t stride = 64)
        : msgs_(std::move(msgs)), stride_(stride ? stride : 1) {
        auto [m, c] = run_init<P>();
        (void)c;
        // Build every snapshot up front: one pass over the run. After this,
        // at() never folds more than stride-1 messages.
        snaps_.reserve(msgs_.size() / stride_ + 1);
        snaps_.push_back(m);
        for (std::size_t i = 0; i < msgs_.size(); ++i) {
            m = fold(std::move(m), msgs_[i]);
            if ((i + 1) % stride_ == 0) snaps_.push_back(m);
        }
        cur_.emplace(0, snaps_.front());
    }

    /// Steps in the run: the initial model plus one per message.
    [[nodiscard]] std::size_t size() const noexcept { return msgs_.size() + 1; }

    /// The model after `step` messages (step 0 = the initial model).
    const model_type& at(std::size_t step) {
        if (step >= size()) throw std::out_of_range("jaal::timeline::at: step past the end");
        // Walk forward from the cached model when that's closer than the
        // snapshot; otherwise restart from the snapshot at or before `step`.
        const std::size_t snap = step / stride_;
        if (!cur_ || cur_->first > step || cur_->first < snap * stride_)
            cur_.emplace(snap * stride_, snaps_[snap]);
        while (cur_->first < step) {
            cur_->second = fold(std::move(cur_->second), msgs_[cur_->first]);
            ++cur_->first;
        }
        return cur_->second;
    }

    /// The message that took step-1 to step. step must be >= 1.
    [[nodiscard]] const msg_type& message(std::size_t step) const {
        if (step == 0 || step >= size())
            throw std::out_of_range("jaal::timeline::message: no message leads to this step");
        return msgs_[step - 1];
    }

    /// Field changes between step-1 and step (what message(step) did).
    [[nodiscard]] std::vector<field_change> changes(std::size_t step, diff_options o = {}) {
        if (step == 0) return {};
        model_type before = at(step - 1);
        return diff(before, at(step), o);
    }

    /// Field changes between any two steps.
    [[nodiscard]] std::vector<field_change> changes(std::size_t from, std::size_t to,
                                                    diff_options o = {}) {
        model_type a = at(from);
        return diff(a, at(to), o);
    }

    /// The first step whose model fails `good`, checking every step in
    /// order. Works for any predicate.
    template <class F>
        requires std::is_invocable_r_v<bool, F&, const model_type&>
    [[nodiscard]] std::optional<std::size_t> first_bad(F good) {
        for (std::size_t s = 0; s < size(); ++s)
            if (!good(at(s))) return s;
        return std::nullopt;
    }

    /// Like first_bad, but by bisection: O(log n) model checks. Only right
    /// when the property is MONOTONE (once bad, stays bad), e.g. "the
    /// cache has no stale entry" in a program that never repairs it. For a
    /// property that can recover, use first_bad.
    template <class F>
        requires std::is_invocable_r_v<bool, F&, const model_type&>
    [[nodiscard]] std::optional<std::size_t> bisect(F good) {
        if (good(at(size() - 1))) return std::nullopt;
        if (!good(at(0))) return 0;
        std::size_t lo = 0, hi = size() - 1;           // good(lo), !good(hi)
        while (hi - lo > 1) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (good(at(mid))) lo = mid; else hi = mid;
        }
        return hi;
    }

    [[nodiscard]] const std::vector<msg_type>& messages() const noexcept { return msgs_; }

private:
    static model_type fold(model_type m, const msg_type& msg) {
        auto [next, cmd] = detail::prog::split(P::update(std::move(m), msg));
        (void)cmd;
        return std::move(next);
    }

    std::vector<msg_type>                         msgs_;
    std::size_t                                   stride_;
    std::vector<model_type>                       snaps_;   // snaps_[k] = model at step k*stride
    std::optional<std::pair<std::size_t, model_type>> cur_; // last model reached
};

}  // namespace jaal
