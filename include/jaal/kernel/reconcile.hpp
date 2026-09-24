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
    /// Performance: subscription sets are small (a handful of sources), so
    /// this uses FLAT vectors with linear search instead of hash maps, and
    /// reuses its storage across calls. Measured before: three hash maps and
    /// five vectors allocated per call, ~1.15 us for an 8-source Sub. The
    /// plan it returns refers to storage owned here and is valid until the
    /// next reconcile().
    template <class RouterSink>
    const plan& reconcile(const Sub<Msg, row_type>& next, RouterSink&& routers) {
        want_.clear();
        ordinals_.clear();
        plan_.clear();

        next.for_each([&]<class X>(const X& x) {
            (take_leaf<Ds>(x, routers), ...);
        });

        for (auto& w : want_) {
            if (find(running_, w.k)) plan_.keep.push_back({w.k, w.p});
            else                     plan_.start.push_back({w.k, w.p});
        }
        for (auto& r : running_)
            if (!find(want_, r.k)) plan_.stop.push_back({r.k});

        running_.swap(want_);                 // want_'s old storage is reused next call
        return plan_;
    }

    [[nodiscard]] std::size_t size() const noexcept { return running_.size(); }
    [[nodiscard]] bool contains(const key& k) const { return find(running_, k) != nullptr; }

private:
    struct entry { key k; payload p; };

    static const entry* find(const std::vector<entry>& v, const key& k) noexcept {
        for (auto& e : v) if (e.k == k) return &e;
        return nullptr;
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
                if (find(want_, tk)) plan_.duplicates.push_back({tk});
                else                 want_.push_back({std::move(tk), payload{x}});
            } else {
                routers(x);
            }
        }
    }

    std::uint32_t next_ordinal(const key& base) {
        for (auto& [k, n] : ordinals_) if (k == base) return n++;
        ordinals_.emplace_back(base, 1u);
        return 0;
    }

    std::vector<entry>                          running_;
    std::vector<entry>                          want_;       // scratch, reused
    std::vector<std::pair<key, std::uint32_t>>  ordinals_;   // scratch, reused
    plan                                        plan_;       // scratch, reused
};

}  // namespace jaal

// std::hash for tagged_key's variant alternatives goes through key_hash;
// std::variant<tagged_key<...>> itself is hashed by detail::rec::key_hash.
