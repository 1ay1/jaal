#pragma once
// jaal reconcile — diff one subscribe() result against what's running.
//
// Pure: no threads, no clock, no I/O. Given the running set and a new Sub,
// it says what to KEEP, START and STOP, and the host/kernel acts on that.
// That makes the rules testable on their own:
//
//   * a source whose key appears in both old and new: KEEP it (a timer keeps
//     its phase); its payload is replaced with the new one, so a changed
//     Msg takes effect without a restart
//   * key only in new: START
//   * key only in old: STOP
//   * the same key twice in one result: reported as a duplicate, and only
//     the first is used (never silent, never two copies running)
//   * `numbered` sources (every) get an ordinal: the n-th source with the
//     same base key in this result gets ordinal n. So two every(1s) timers
//     are two keys, and adding a third doesn't disturb the first two.
//
// Routers aren't reconciled: they're stateless and rebuilt from each Sub.
// reconcile() hands them back in order for dispatch.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "../core/row.hpp"
#include "../core/sub.hpp"

namespace jaal {

namespace detail::rec {

template <class D>
concept numbered_source = SourceDescriptor<D> && requires {
    { D::numbered } -> std::convertible_to<bool>;
} && D::numbered && requires(typename D::key_type k) {
    { k.ordinal } -> std::convertible_to<std::uint32_t>;
};

// Hash for the tagged key variant.
struct key_hash {
    template <class V>
    std::size_t operator()(const V& v) const {
        return std::visit([&]<class K>(const K& k) -> std::size_t {
            if constexpr (std::same_as<K, detail::sub::no_source_key>) {
                return 0;                       // unreachable: no values exist
            } else {
                const auto h = std::hash<std::remove_cvref_t<decltype(k.key)>>{}(k.key);
                return h ^ (v.index() * 0x9E3779B97F4A7C15ull);
            }
        }, v);
    }
};

// A variant over a row's payloads; std::monostate when the row is empty,
// since std::variant<> is ill-formed.
template <class Msg, class... Ds> struct payload_variant {
    using type = std::variant<payload_t<Ds, Msg>...>;
};
template <class Msg> struct payload_variant<Msg> {
    using type = std::variant<std::monostate>;
};

}  // namespace detail::rec

/// The running set for a Sub row: source key → current payload.
template <class Msg, Row R>
class running_sources;

template <class Msg, SubDescriptor... Ds>
class running_sources<Msg, row<Ds...>> {
public:
    using row_type = row<Ds...>;
    using key      = source_key_t<row_type>;
    using payload  = typename detail::rec::payload_variant<Msg, Ds...>::type;

    struct started { key k; payload p; };
    struct updated { key k; payload p; };     // kept, payload replaced
    struct stopped { key k; };
    struct duplicate { key k; };

    struct plan {
        std::vector<started>   start;
        std::vector<updated>   keep;
        std::vector<stopped>   stop;
        std::vector<duplicate> duplicates;
        bool changed() const noexcept { return !start.empty() || !stop.empty(); }
        void clear() noexcept { start.clear(); keep.clear(); stop.clear(); duplicates.clear(); }
    };

    /// Diff `next` against what's running, update the running set to match,
    /// and return what changed. Routers are appended to `routers` in order.
    ///
    /// Performance. Subscription sets are usually small, so the storage is
    /// FLAT vectors, reused across calls: no hashing, no nodes, and a linear
    /// scan of a handful of contiguous keys beats a hash map every time.
    /// Measured before that change: three hash maps and five vectors
    /// allocated per call, ~1.15 us for an 8-source Sub.
    ///
    /// But "usually small" isn't "always": a UI whose model drives one
    /// subscription per visible row has hundreds. Three linear scans per
    /// source (duplicate check, running lookup, ordinal) made this
    /// QUADRATIC — measured 8 -> 256 sources was 32x the work and 295x the
    /// time, 79 -> 728 ns per source. So above `index_threshold` the same
    /// flat vectors get a hash index beside them, built once per call and
    /// reused; lookups become O(1) and the whole diff O(n). Below it,
    /// nothing changes and nothing is allocated.
    template <class RouterSink>
    const plan& reconcile(const basic_sub<Msg, row_type>& next, RouterSink&& routers) {
        want_.clear();
        ordinals_.clear();
        plan_.clear();
        indexed_     = false;                // want_ is empty: nothing to index yet
        ord_indexed_ = false;

        next.for_each([&]<class X>(const X& x) {
            (take_leaf<Ds>(x, routers), ...);
        });

        // The running set is indexed once, for the two loops below, when
        // either side is big enough for it to pay.
        const bool big = want_.size() > index_threshold || running_.size() > index_threshold;
        if (big) {
            index(running_ix_, running_);
        }

        for (auto& w : want_) {
            const bool running = big ? probe(running_ix_, running_, w.k) != nullptr
                                     : find(running_, w.k) != nullptr;
            if (running) plan_.keep.push_back({w.k, w.p});
            else         plan_.start.push_back({w.k, w.p});
        }
        if (big) {
            // want_ is already indexed (take_leaf built it), so this is O(1)
            // per running source instead of a scan of want_.
            for (auto& r : running_)
                if (!probe(want_ix_, want_, r.k)) plan_.stop.push_back({r.k});
        } else {
            for (auto& r : running_)
                if (!find(want_, r.k)) plan_.stop.push_back({r.k});
        }

        running_.swap(want_);                 // want_'s old storage is reused next call
        return plan_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return running_.size(); }
    [[nodiscard]] bool contains(const key& k) const { return find(running_, k) != nullptr; }

private:
    struct entry { key k; payload p; };

    /// Sources per side above which the flat scans get a hash index. Chosen
    /// from the scaling measurement: at 16 the two are within noise, and
    /// below 16 the scan wins on cache locality alone.
    static constexpr std::size_t index_threshold = 16;

    static const entry* find(const std::vector<entry>& v, const key& k) noexcept {
        for (auto& e : v) if (e.k == k) return &e;
        return nullptr;
    }

    /// Fill `ix` with v's keys. Open addressing in a FLAT vector, because a
    /// node-based map is the wrong shape here: std::unordered_map::clear()
    /// frees every node, so re-indexing each cycle allocates one node per
    /// source. Measured, at 64 sources: 192 allocations per reconcile with
    /// unordered_map (3 per source, every cycle) against 0.14 for the whole
    /// call with this. The table's storage is reused, so a steady state
    /// allocates nothing at all.
    ///
    /// Power-of-two capacity at 2x load, linear probing: keys are small and
    /// contiguous, and a probe that misses lands in the same cache line as
    /// the one that hits.
    void index(std::vector<std::uint32_t>& ix, const std::vector<entry>& v) const {
        std::size_t cap = 16;
        while (cap < v.size() * 2) cap <<= 1;
        ix.assign(cap, kEmpty);                  // reuses the buffer when cap is unchanged
        const std::size_t mask = cap - 1;
        for (std::size_t i = 0; i < v.size(); ++i) {
            std::size_t h = detail::rec::key_hash{}(v[i].k) & mask;
            while (ix[h] != kEmpty) h = (h + 1) & mask;
            ix[h] = static_cast<std::uint32_t>(i);
        }
    }

    /// Where `k` sits in `v`, using index `ix`, or null.
    static const entry* probe(const std::vector<std::uint32_t>& ix,
                              const std::vector<entry>& v, const key& k) noexcept {
        const std::size_t mask = ix.size() - 1;
        std::size_t h = detail::rec::key_hash{}(k) & mask;
        for (;;) {
            const auto slot = ix[h];
            if (slot == kEmpty) return nullptr;
            if (v[slot].k == k) return &v[slot];
            h = (h + 1) & mask;
        }
    }

    /// Is `k` already in want_? O(1) once want_ has grown past the
    /// threshold; the index is built lazily, at the crossing, exactly once.
    bool want_has(const key& k) {
        if (!indexed_ && want_.size() > index_threshold) {
            index(want_ix_, want_);
            indexed_ = true;
        }
        if (indexed_) return probe(want_ix_, want_, k) != nullptr;
        return find(want_, k) != nullptr;
    }

    void want_add(key k, payload p) {
        want_.push_back({std::move(k), std::move(p)});
        if (indexed_) {
            // Grow (and rebuild) only when the load factor would pass 1/2.
            if (want_.size() * 2 > want_ix_.size()) {
                index(want_ix_, want_);
            } else {
                const std::size_t mask = want_ix_.size() - 1;
                std::size_t h = detail::rec::key_hash{}(want_.back().k) & mask;
                while (want_ix_[h] != kEmpty) h = (h + 1) & mask;
                want_ix_[h] = static_cast<std::uint32_t>(want_.size() - 1);
            }
        }
    }

    // One leaf of the flattened Sub, offered to descriptor D. Does nothing
    // unless x is D's payload.
    template <class D, class X, class RouterSink>
    void take_leaf(const X& x, RouterSink& routers) {
        if constexpr (std::same_as<X, payload_t<D, Msg>>) {
            if constexpr (SourceDescriptor<D>) {
                auto k = D::key(x);
                if constexpr (detail::rec::numbered_source<D>) {
                    auto base    = k;
                    base.ordinal = 0;
                    k.ordinal    = next_ordinal(key{tagged_key<D>{base}});
                }
                key tk{tagged_key<D>{std::move(k)}};
                if (want_has(tk)) plan_.duplicates.push_back({tk});
                else              want_add(std::move(tk), payload{x});
            } else {
                routers(x);
            }
        }
    }

    std::uint32_t next_ordinal(const key& base) {
        // Ordinals are per BASE key: two every(1s) timers carrying the same
        // msg are one base with ordinals 0 and 1. So this is short whenever
        // sources REPEAT — but a UI with one timer per row has n DISTINCT
        // bases, and then a linear scan here is the last quadratic term in
        // the diff. Measured at 512 sources: 41 ns/source with one base
        // against 349 ns/source with 512, all of it this scan.
        //
        // Above the threshold the bases get the same flat open-addressed
        // index as want_. The list and the index are never both live: the
        // crossing moves every entry across, once. (Two halves of the same
        // counter in two places would hand out a duplicate ordinal, and a
        // duplicate ordinal is a collided subscription key.)
        if (!ord_indexed_ && ordinals_.size() > index_threshold) {
            ord_ix_.assign(ordinal_cap(ordinals_.size()), kEmpty);
            for (std::size_t i = 0; i < ordinals_.size(); ++i) ord_insert(i);
            ord_indexed_ = true;
        }
        if (!ord_indexed_) {
            for (auto& [k, n] : ordinals_) if (k == base) return n++;
            ordinals_.emplace_back(base, 1u);
            return 0;
        }
        // Indexed: probe, then either bump the counter or add a base.
        const std::size_t mask = ord_ix_.size() - 1;
        std::size_t h = detail::rec::key_hash{}(base) & mask;
        for (;;) {
            const auto slot = ord_ix_[h];
            if (slot == kEmpty) break;
            if (ordinals_[slot].first == base) return ordinals_[slot].second++;
            h = (h + 1) & mask;
        }
        ordinals_.emplace_back(base, 1u);
        if (ordinals_.size() * 2 > ord_ix_.size()) {      // grow: rehash all
            ord_ix_.assign(ordinal_cap(ordinals_.size()), kEmpty);
            for (std::size_t i = 0; i < ordinals_.size(); ++i) ord_insert(i);
        } else {
            ord_ix_[h] = static_cast<std::uint32_t>(ordinals_.size() - 1);
        }
        return 0;
    }

    static std::size_t ordinal_cap(std::size_t n) noexcept {
        std::size_t cap = 16;
        while (cap < n * 2) cap <<= 1;
        return cap;
    }

    void ord_insert(std::size_t i) {
        const std::size_t mask = ord_ix_.size() - 1;
        std::size_t h = detail::rec::key_hash{}(ordinals_[i].first) & mask;
        while (ord_ix_[h] != kEmpty) h = (h + 1) & mask;
        ord_ix_[h] = static_cast<std::uint32_t>(i);
    }

    static constexpr std::uint32_t kEmpty = 0xFFFF'FFFFu;

    std::vector<entry>                          running_;
    std::vector<entry>                          want_;       // scratch, reused
    std::vector<std::pair<key, std::uint32_t>>  ordinals_;   // scratch, reused
    plan                                        plan_;       // scratch, reused

    // Open-addressed indexes into want_ / running_, used only above
    // index_threshold. Flat, and their storage is reused, so the steady state
    // allocates nothing.
    std::vector<std::uint32_t>                  want_ix_;
    mutable std::vector<std::uint32_t>          running_ix_;
    std::vector<std::uint32_t>                  ord_ix_;
    bool                                        indexed_ = false;
    bool                                        ord_indexed_ = false;
};

}  // namespace jaal

// std::hash for tagged_key's variant alternatives goes through key_hash;
// std::variant<tagged_key<...>> itself is hashed by detail::rec::key_hash.
