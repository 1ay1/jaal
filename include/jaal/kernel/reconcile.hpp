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
        return std::visit([&]<class K>(const K& k) {
            const auto h = std::hash<std::remove_cvref_t<decltype(k.key)>>{}(k.key);
            return h ^ (v.index() * 0x9E3779B97F4A7C15ull);
        }, v);
    }
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
    using payload  = std::variant<payload_t<Ds, Msg>...>;

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
    };

    /// Diff `next` against what's running, update the running set to match,
    /// and return what changed. Routers are appended to `routers` in order.
    template <class RouterSink>
    plan reconcile(const Sub<Msg, row_type>& next, RouterSink&& routers) {
        std::unordered_map<key, payload, detail::rec::key_hash> want;
        std::vector<key> order;
        std::unordered_map<key, std::uint32_t, detail::rec::key_hash> next_ordinal;
        plan p;

        next.for_each([&]<class X>(const X& x) {
            (take_leaf<Ds>(x, want, order, next_ordinal, p, routers), ...);
        });

        for (auto& k : order) {
            auto it = running_.find(k);
            if (it == running_.end()) p.start.push_back({k, want.at(k)});
            else                      p.keep.push_back({k, want.at(k)});
        }
        for (auto& [k, _] : running_)
            if (!want.contains(k)) p.stop.push_back({k});

        running_ = std::move(want);
        return p;
    }

    [[nodiscard]] std::size_t size() const noexcept { return running_.size(); }
    [[nodiscard]] bool contains(const key& k) const { return running_.contains(k); }

private:
    using want_map    = std::unordered_map<key, payload, detail::rec::key_hash>;
    using ordinal_map = std::unordered_map<key, std::uint32_t, detail::rec::key_hash>;

    // One leaf of the flattened Sub, offered to descriptor D. Does nothing
    // unless x is D's payload.
    template <class D, class X, class RouterSink>
    static void take_leaf(const X& x, want_map& want, std::vector<key>& order,
                          ordinal_map& next_ordinal, plan& p, RouterSink& routers) {
        if constexpr (std::same_as<X, payload_t<D, Msg>>) {
            if constexpr (SourceDescriptor<D>) {
                auto k = D::key(x);
                if constexpr (detail::rec::numbered_source<D>) {
                    auto base    = k;
                    base.ordinal = 0;
                    k.ordinal    = next_ordinal[key{tagged_key<D>{base}}]++;
                }
                key tk{tagged_key<D>{std::move(k)}};
                if (want.contains(tk)) {
                    p.duplicates.push_back({tk});
                } else {
                    want.emplace(tk, payload{x});
                    order.push_back(std::move(tk));
                }
            } else {
                routers(x);
            }
        }
    }

    std::unordered_map<key, payload, detail::rec::key_hash> running_;
};

}  // namespace jaal

// std::hash for tagged_key's variant alternatives goes through key_hash;
// std::variant<tagged_key<...>> itself is hashed by detail::rec::key_hash.
