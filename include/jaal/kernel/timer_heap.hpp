#pragma once
// jaal::kernel::timer_heap — `after` and `every`, as a heap with an index.
//
// A min-heap on the next fire time, so collect_due() only ever looks at the
// front. Two operations need a timer BY ID rather than by time, though:
//
//   replace_payload(id, p)  a kept subscription whose Msg changed; the timer
//                           keeps its phase, so an `every` that survives a
//                           model change doesn't restart
//   cancel(id)              a subscription that went away
//
// Both used to scan the heap. That's fine for a handful of timers and
// quadratic for a UI with one per row: reconcile calls replace_payload once
// per KEPT timer, so n timers cost n scans of n entries. Measured end to end
// at 256 timers: 281 ns per timer per reconcile, climbing with n.
//
// So the heap carries an id -> slot index, maintained by the three places
// that move an entry (push, the pop in collect_due, and the swap-erase in
// cancel). Every operation is then O(log n) or better.
//
// The index is a FLAT table, not a hash map, and that choice is measured.
// The first version used std::unordered_map<timer_id, slot>: a node
// allocation per armed timer, and a hash lookup on every swap of every
// sift. `after` — the commonest effect there is — went from 12.7 to 31 ns and
// gained a malloc per call. A timer id here is the index of a slot in a
// reusable vector plus a generation count: arming reuses a freed slot,
// looking one up is an array index, and a stale id (its slot since reused)
// is caught by the generation instead of silently touching someone else's
// timer. Steady state: no allocation, no hashing.
//
// cancel() is the one worth reading twice. The old version erased from the
// middle and called make_heap on the whole thing (O(n) + O(n)); this swaps
// the hole with the last entry and sifts that entry into place, which is
// O(log n) and touches two cache lines.
//
// Ordering rule: a repeating timer keeps PHASE (re-arms from its deadline),
// but a loop stalled for more than a period fires an `every` ONCE and
// resyncs, rather than delivering backdated ticks (D22).
//
// Entries have stable ids so the reconciler can cancel one by key without
// caring where it sits in the heap.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace jaal::kernel {

using timer_id = std::uint64_t;

// Saturating conversion: a duration that doesn't fit becomes "as far away as
// this clock can say". saturate_add was correct and never reached, because the
// overflow happened in the conversion before it (D16).
template <class To, class Rep, class Period>
[[nodiscard]] constexpr To saturate_cast(std::chrono::duration<Rep, Period> d) noexcept {
    using CommonRep = std::common_type_t<Rep, typename To::rep>;
    constexpr auto to_max = std::numeric_limits<typename To::rep>::max();
    constexpr auto to_min = std::numeric_limits<typename To::rep>::min();

    // Convert in the common representation, checking the ratio by hand so a
    // huge value can't wrap on the way in.
    using R = std::ratio_divide<Period, typename To::period>;
    const auto count = static_cast<CommonRep>(d.count());
    if (count > 0 && count > static_cast<CommonRep>(to_max) / static_cast<CommonRep>(R::num)
                                 * static_cast<CommonRep>(R::den))
        return To{to_max};
    if (count < 0 && count < static_cast<CommonRep>(to_min) / static_cast<CommonRep>(R::num)
                                 * static_cast<CommonRep>(R::den))
        return To{to_min};
    return std::chrono::duration_cast<To>(d);
}

template <class Clock, class Rep, class Period>
[[nodiscard]] constexpr auto saturate_add(typename Clock::time_point now,
                                          std::chrono::duration<Rep, Period> d)
    -> typename Clock::time_point {
    using duration = typename Clock::duration;
    const auto dd  = saturate_cast<duration>(d);
    const auto max = typename Clock::time_point(duration{std::numeric_limits<
        typename duration::rep>::max()});
    if (dd.count() > 0 && now > max - dd) return max;
    return now + dd;
}

/// Milliseconds to wait for `deadline`, rounded UP, never negative.
/// nullopt means "no deadline: wait indefinitely".
///
/// Rounding DOWN caused a hot spin in maya: the loop woke just before the
/// timer was due, found nothing ready, computed a 0 ms timeout and spun. The
/// conversion lives in ONE place so no backend can get it wrong on its own.
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
        return push({saturate_add<Clock>(now, d), duration::zero(), std::move(p), new_id()});
    }

    /// Arm a repeating timer; first fire one period from now.
    template <class Rep, class Period>
    timer_id every(time_point now, std::chrono::duration<Rep, Period> period, Payload p) {
        auto per = saturate_cast<duration>(period);
        if (per <= duration::zero()) per = duration{1};
        return push({saturate_add<Clock>(now, per), per, std::move(p), new_id()});
    }

    /// Re-arm an existing repeating timer with a new payload, KEEPING its
    /// phase (its next fire time). This is what makes a kept subscription
    /// not restart its timer. O(1).
    bool replace_payload(timer_id id, Payload p) {
        const auto slot = slot_of(id);
        if (slot == kNone) return false;
        heap_[slot].payload = std::move(p);
        return true;
    }

    /// O(log n): swap the hole with the last entry and sift it into place.
    bool cancel(timer_id id) {
        const auto slot = slot_of(id);
        if (slot == kNone) return false;
        free_id(id);
        const std::size_t last = heap_.size() - 1;
        if (slot != last) {
            heap_[slot] = std::move(heap_[last]);
            heap_.pop_back();
            note(slot);
            sift(slot);
        } else {
            heap_.pop_back();
        }
        return true;
    }

    /// The next deadline, or nullopt when nothing is armed.
    [[nodiscard]] std::optional<time_point> next_deadline() const {
        if (heap_.empty()) return std::nullopt;
        return heap_.front().when;
    }

    /// Pop every timer due at `now` into `out`. Repeating ones stay in the
    /// heap and re-arm one period after their DEADLINE, not after `now`: the
    /// loop always notices a timer a little late (the wait rounds up, the
    /// step before it takes time), and re-arming from `now` added that
    /// lateness to every period. A 16 ms `every` ran at 54.7 Hz where 62.5
    /// was asked. Keeping phase fixes the rate exactly.
    ///
    /// D22 is kept: if the timer is more than a whole period late (the loop
    /// stalled), it fires ONCE and resyncs to now + period, instead of
    /// delivering the missed ticks as a catch-up storm.
    void collect_due(time_point now, std::vector<Payload>& out) {
        while (!heap_.empty() && heap_.front().when <= now) {
            auto e = std::move(heap_.front());
            pop_root();
            out.push_back(e.payload);
            if (e.period > duration::zero()) {
                const auto next = saturate_add<Clock>(e.when, e.period);
                e.when = next > now ? next : saturate_add<Clock>(now, e.period);
                push(std::move(e));
            } else {
                free_id(e.id);
            }
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return heap_.size(); }
    [[nodiscard]] bool empty() const noexcept       { return heap_.empty(); }
    void clear() noexcept {
        heap_.clear();
        // Bump every live slot's generation so no id handed out before the
        // clear can match anything after it, then free them all.
        for (std::uint32_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].pos != kFreePos) {
                ++slots_[i].gen;
                slots_[i].pos = kFreePos;
                free_.push_back(i);
            }
        }
    }

private:
    static constexpr std::size_t kNone = static_cast<std::size_t>(-1);

    static bool later_than(const entry& a, const entry& b) noexcept { return a.when > b.when; }

    // A timer_id is (generation << 32) | slot-table index. The slot table
    // maps it to the entry's current position in heap_.
    struct slot_rec {
        std::uint32_t pos = kFreePos;   // index into heap_, or kFreePos
        std::uint32_t gen = 0;          // bumped on every free
    };
    static constexpr std::uint32_t kFreePos = 0xFFFF'FFFFu;

    static std::uint32_t index_of(timer_id id) noexcept { return static_cast<std::uint32_t>(id); }
    static std::uint32_t gen_of(timer_id id) noexcept   { return static_cast<std::uint32_t>(id >> 32); }

    timer_id new_id() {
        std::uint32_t ix;
        if (!free_.empty()) {
            ix = free_.back();
            free_.pop_back();
        } else {
            ix = static_cast<std::uint32_t>(slots_.size());
            slots_.push_back({});
        }
        return (static_cast<timer_id>(slots_[ix].gen) << 32) | ix;
    }

    void free_id(timer_id id) {
        auto& r = slots_[index_of(id)];
        r.pos = kFreePos;
        ++r.gen;                        // a stale copy of `id` now matches nothing
        free_.push_back(index_of(id));
    }

    [[nodiscard]] std::size_t slot_of(timer_id id) const noexcept {
        const auto ix = index_of(id);
        if (ix >= slots_.size()) return kNone;
        const auto& r = slots_[ix];
        if (r.pos == kFreePos || r.gen != gen_of(id)) return kNone;
        return r.pos;
    }

    void note(std::size_t pos) { slots_[index_of(heap_[pos].id)].pos = static_cast<std::uint32_t>(pos); }

    timer_id push(entry e) {
        const auto id = e.id;
        heap_.push_back(std::move(e));
        std::size_t i = heap_.size() - 1;
        note(i);
        // Sift UP: a new entry can only be earlier than its parent.
        while (i > 0) {
            const std::size_t parent = (i - 1) / 2;
            if (!later_than(heap_[parent], heap_[i])) break;
            std::swap(heap_[parent], heap_[i]);
            note(i);
            note(parent);
            i = parent;
        }
        return id;
    }

    void pop_root() {
        const std::size_t last = heap_.size() - 1;
        if (last != 0) {
            heap_[0] = std::move(heap_[last]);
            heap_.pop_back();
            note(0);
            sift_down(0);
        } else {
            heap_.pop_back();
        }
    }

    /// Restore the heap around `i`, which may need to go either way.
    void sift(std::size_t i) {
        if (i > 0) {
            const std::size_t parent = (i - 1) / 2;
            if (later_than(heap_[parent], heap_[i])) {
                std::swap(heap_[parent], heap_[i]);
                note(i);
                note(parent);
                sift(parent);
                return;
            }
        }
        sift_down(i);
    }

    void sift_down(std::size_t i) {
        for (;;) {
            const std::size_t l = 2 * i + 1;
            const std::size_t r = l + 1;
            std::size_t best = i;
            if (l < heap_.size() && later_than(heap_[best], heap_[l])) best = l;
            if (r < heap_.size() && later_than(heap_[best], heap_[r])) best = r;
            if (best == i) return;
            std::swap(heap_[i], heap_[best]);
            note(i);
            note(best);
            i = best;
        }
    }

    std::vector<entry>          heap_;
    std::vector<slot_rec>       slots_;     // id -> position in heap_
    std::vector<std::uint32_t>  free_;      // slot-table entries to reuse
};

}  // namespace jaal::kernel
