#pragma once
// jaal::kernel::timer_heap — deadlines, and the two rules that matter.
//
//   1. A deadline is computed with saturating arithmetic, INCLUDING the
//      unit conversion. `now + huge` is signed overflow (UB), and so is
//      duration_cast<nanoseconds>(milliseconds::max()), which wraps to
//      -1 ms: a "fire never" timer would fire at once.
//
//   2. Turning a deadline into a wait timeout ROUNDS UP. Rounding down
//      caused a hot spin in maya: the loop woke just before the timer was
//      due, found nothing ready, computed a 0 ms timeout and spun. The
//      conversion lives in ONE place (timeout_from) so no backend can get
//      it wrong on its own.
//
// Repeating timers re-arm from NOW, not from the missed deadline: if the
// loop stalled for ten periods, an `every` fires ONCE and carries on. A
// catch-up storm is never what an app wants.
//
// Entries have stable ids so the reconciler can cancel one by key without
// touching the rest.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace jaal::kernel {

/// duration_cast that CLAMPS instead of overflowing.
///
/// std::chrono::duration_cast<nanoseconds>(milliseconds::max()) is signed
/// overflow: it wraps to -1 ms. Fed into a timer, "fire never" became "fire
/// now". saturate_add was correct and never reached, because the overflow
/// happened one step earlier, in the unit conversion. Found by kernel_test
/// rule 5.
template <class To, class Rep, class Period>
[[nodiscard]] constexpr To saturate_cast(std::chrono::duration<Rep, Period> d) noexcept {
    using namespace std::chrono;
    if constexpr (std::ratio_greater_v<Period, typename To::period>) {
        // Coarse → fine (ms → ns) is the direction that overflows. To's
        // limits, expressed in d's coarser unit, are small numbers, so the
        // comparison itself can't overflow.
        const auto hi = duration_cast<duration<Rep, Period>>(To::max());
        const auto lo = duration_cast<duration<Rep, Period>>(To::min());
        if (d >= hi) return To::max();
        if (d <= lo) return To::min();
        return duration_cast<To>(d);
    } else {
        return duration_cast<To>(d);   // fine → coarse can't overflow
    }
}

/// now + d, clamped instead of overflowing. d may be in any unit.
template <class Clock, class Rep, class Period>
[[nodiscard]] constexpr auto saturate_add(typename Clock::time_point now,
                                          std::chrono::duration<Rep, Period> d) noexcept ->
    typename Clock::time_point {
    using tp  = typename Clock::time_point;
    using dur = typename Clock::duration;
    const dur dd = saturate_cast<dur>(d);
    if (dd <= dur::zero()) return dd == dur::zero() ? now : now + dd;
    const auto left = tp::max() - now;
    if (dd > left) return tp::max();
    return now + dd;
}

/// Milliseconds to wait for `deadline`, rounded UP, never negative.
/// nullopt means "no deadline: wait indefinitely".
template <class Clock>
[[nodiscard]] constexpr std::optional<std::chrono::milliseconds>
timeout_from(typename Clock::time_point now,
             std::optional<typename Clock::time_point> deadline) noexcept {
    using namespace std::chrono;
    if (!deadline) return std::nullopt;
    if (*deadline <= now) return milliseconds{0};
    const auto left = *deadline - now;
    // ceil: 0.3 ms left must wait 1 ms, never 0 (the hot-spin bug).
    return ceil<milliseconds>(left);
}

using timer_id = std::uint64_t;

template <class Clock, class Payload>
class timer_heap {
public:
    using time_point = typename Clock::time_point;
    using duration   = typename Clock::duration;

    struct entry {
        time_point when;
        duration   period{};      // zero = one-shot
        Payload    payload;
        timer_id   id = 0;
    };

    /// Arm a one-shot timer. d may be in any unit; conversion saturates.
    template <class Rep, class Period>
    timer_id after(time_point now, std::chrono::duration<Rep, Period> d, Payload p) {
        return push({saturate_add<Clock>(now, d), duration::zero(), std::move(p), ++next_id_});
    }

    /// Arm a repeating timer; first fire one period from now.
    template <class Rep, class Period>
    timer_id every(time_point now, std::chrono::duration<Rep, Period> period, Payload p) {
        auto per = saturate_cast<duration>(period);
        if (per <= duration::zero()) per = duration{1};
        return push({saturate_add<Clock>(now, per), per, std::move(p), ++next_id_});
    }

    /// Re-arm an existing repeating timer with a new payload, KEEPING its
    /// phase (its next fire time). This is what makes a kept subscription
    /// not restart its timer.
    bool replace_payload(timer_id id, Payload p) {
        for (auto& e : heap_)
            if (e.id == id) { e.payload = std::move(p); return true; }
        return false;
    }

    bool cancel(timer_id id) {
        auto it = std::find_if(heap_.begin(), heap_.end(),
                               [&](const entry& e) { return e.id == id; });
        if (it == heap_.end()) return false;
        heap_.erase(it);
        std::make_heap(heap_.begin(), heap_.end(), later{});
        return true;
    }

    /// The next deadline, or nullopt when nothing is armed.
    [[nodiscard]] std::optional<time_point> next_deadline() const {
        if (heap_.empty()) return std::nullopt;
        return heap_.front().when;
    }

    /// Pop every timer due at `now` into `out`. Repeating ones re-arm from
    /// NOW (no catch-up storm) and stay in the heap.
    void collect_due(time_point now, std::vector<Payload>& out) {
        while (!heap_.empty() && heap_.front().when <= now) {
            std::pop_heap(heap_.begin(), heap_.end(), later{});
            auto e = std::move(heap_.back());
            heap_.pop_back();
            out.push_back(e.payload);
            if (e.period > duration::zero()) {
                e.when = saturate_add<Clock>(now, e.period);
                heap_.push_back(std::move(e));
                std::push_heap(heap_.begin(), heap_.end(), later{});
            }
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return heap_.size(); }
    [[nodiscard]] bool empty() const noexcept       { return heap_.empty(); }
    void clear() noexcept                           { heap_.clear(); }

private:
    struct later {
        bool operator()(const entry& a, const entry& b) const noexcept {
            return a.when > b.when;          // min-heap on `when`
        }
    };

    timer_id push(entry e) {
        const auto id = e.id;
        heap_.push_back(std::move(e));
        std::push_heap(heap_.begin(), heap_.end(), later{});
        return id;
    }

    std::vector<entry> heap_;
    timer_id           next_id_ = 0;
};

}  // namespace jaal::kernel
