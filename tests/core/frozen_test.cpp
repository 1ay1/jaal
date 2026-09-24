// tests/core/frozen_test.cpp — Frozen and shared<T>. Compiling is passing,
// plus a small runtime part for shared<T>'s lifetime behaviour.

#include <jaal/core/frozen.hpp>
#include <jaal/core/sendable.hpp>
#include <jaal/core/shared.hpp>

#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

using jaal::Frozen;
using jaal::Sendable;
using jaal::Shareable;
using jaal::frozen_culprit_t;
using jaal::frozen_fails_on_mutable_v;

// ── leaves and std ───────────────────────────────────────────────────────
enum class Kind { a };
static_assert(Frozen<int>);
static_assert(Frozen<Kind>);
static_assert(Frozen<std::string>);
static_assert(Frozen<std::vector<std::string>>);
static_assert(Frozen<std::optional<int>>);
static_assert(Frozen<std::variant<int, std::string>>);
static_assert(Frozen<std::map<std::string, std::vector<int>>>);
static_assert(Frozen<std::array<int, 3>>);
static_assert(Frozen<std::chrono::milliseconds>);
static_assert(Frozen<std::expected<std::string, int>>);

// pointer-like: the pointee changes through const
static_assert(!Frozen<int*>);
static_assert(!Frozen<const int*>);          // what it points at may change elsewhere
static_assert(!Frozen<std::unique_ptr<int>>);
static_assert(!Frozen<std::unique_ptr<const int>>);
static_assert(!Frozen<std::shared_ptr<const int>>);
static_assert(!Frozen<std::string_view>);
static_assert(!Frozen<std::span<const int>>);
static_assert(!Frozen<std::stop_token>);
static_assert(!Frozen<int&>);

// ── mutable: the whole reason Frozen exists ─────────────────────────────
struct Plain      { int a; std::string b; };
struct WithMut    { int a; mutable int cache; };
struct DeepMut    { Plain p; std::vector<WithMut> items; };
struct MutString  { std::string s; mutable std::string memo; };
static_assert(Frozen<Plain>);
static_assert(!Frozen<WithMut>);
static_assert(!Frozen<DeepMut>);
static_assert(!Frozen<std::optional<WithMut>>);
static_assert(frozen_fails_on_mutable_v<WithMut>);
static_assert(frozen_fails_on_mutable_v<DeepMut>);
static_assert(std::is_same_v<frozen_culprit_t<WithMut>, int>);
static_assert(std::is_same_v<frozen_culprit_t<MutString>, std::string>);

// Sendable and Frozen are different questions:
static_assert(Sendable<WithMut> && !Frozen<WithMut>);             // owned: fine to move, not to share
static_assert(Sendable<std::unique_ptr<int>> && !Frozen<std::unique_ptr<int>>);
static_assert(!Sendable<std::string_view> && !Frozen<std::string_view>);
static_assert(Sendable<std::stop_token> && !Frozen<std::stop_token>);
static_assert(Sendable<Plain> && Frozen<Plain>);

// ── structs with pointers ────────────────────────────────────────────────
struct HasUnique { std::unique_ptr<std::string> title; };
struct HasRef    { const int& r; };
static_assert(!Frozen<HasUnique>);
static_assert(!frozen_fails_on_mutable_v<HasUnique>);
static_assert(std::is_same_v<frozen_culprit_t<HasUnique>, std::unique_ptr<std::string>>);
static_assert(!jaal::frozen_v<HasRef>);

// ── recursive ────────────────────────────────────────────────────────────
struct Tree { std::string label; std::vector<Tree> kids; };
struct BadTree { std::vector<BadTree> kids; mutable int visits; };
static_assert(Frozen<Tree>);
static_assert(!Frozen<BadTree>);

// ── classes jaal can't see inside ────────────────────────────────────────
class Opaque { [[maybe_unused]] int x_ = 0; public: Opaque() = default; };
static_assert(!Frozen<Opaque>);

class Point {
    int x_ = 0, y_ = 0;
public:
    constexpr Point() = default;
    constexpr int x() const { return x_; }
    constexpr int y() const { return y_; }
};
template <> inline constexpr bool jaal::frozen_opt_in<Point> = true;
template <> inline constexpr bool jaal::sendable_opt_in<Point> = true;
static_assert(Frozen<Point>);
static_assert(Shareable<Point>);

// ── agentty's real shape: LazyBytes, reduced ────────────────────────────
// A private class with mutable fields written by a const getter. Frozen
// rejects it even after someone opts it into Sendable, because the two
// opt-ins are separate claims.
class LazyBytesLike {
    std::string source_;
    mutable std::string bytes_;
    mutable bool resolved_ = false;
public:
    const std::string& bytes() const {
        if (!resolved_) { bytes_ = source_; resolved_ = true; }
        return bytes_;
    }
};
template <> inline constexpr bool jaal::sendable_opt_in<LazyBytesLike> = true;
static_assert(Sendable<LazyBytesLike>);
static_assert(!Frozen<LazyBytesLike>);
static_assert(!Shareable<LazyBytesLike>);

// ── shared<T> ────────────────────────────────────────────────────────────
struct Doc {
    std::string title;
    std::vector<std::string> lines;
    bool operator==(const Doc&) const = default;
};
struct Plainest { int x; };   // no ==: shared<Plainest> must not offer value comparison
static_assert(!std::equality_comparable<jaal::shared<Plainest>>);
struct Node { std::string label; std::vector<jaal::shared<Node>> kids; };   // recursive via shared

static_assert(Shareable<Doc>);
static_assert(Sendable<jaal::shared<Doc>>);
static_assert(Frozen<jaal::shared<Doc>>);
static_assert(Shareable<Node>);
static_assert(!std::default_initializable<jaal::shared<Doc>>);   // never null
static_assert(std::copyable<jaal::shared<Doc>>);
static_assert(std::is_same_v<decltype(*std::declval<jaal::shared<Doc>&>()), const Doc&>);
static_assert(std::is_same_v<decltype(std::declval<jaal::shared<Doc>&>().get()), const Doc&>);

// Messages can carry shared values now, and stay Sendable.
struct Parsed { jaal::shared<Doc> doc; };
static_assert(Sendable<std::variant<Parsed, Plain>>);

// make() is only callable for Shareable types
template <class T> concept can_make = requires { jaal::shared<T>::make(); };
static_assert(can_make<Doc>);

// ── runtime: lifetime and threads ────────────────────────────────────────
int main() {
    // move copies: the source is still valid (no moved-from null state)
    auto a = jaal::shared<Doc>::make(Doc{"t", {"x", "y"}});
    auto b = std::move(a);
    if (a->title != "t" || !a.same_as(b)) return 1;

    // recursive shared tree
    auto leaf = jaal::shared<Node>::make(Node{"leaf", {}});
    auto root = jaal::shared<Node>::make(Node{"root", {leaf, leaf}});
    if (root->kids.size() != 2 || !root->kids[0].same_as(leaf)) return 2;

    // value equality vs identity
    auto c = jaal::shared<Doc>::make(Doc{"t", {"x", "y"}});
    if (!(c == b) || c.same_as(b)) return 3;

    // many threads read the same value; the last holder frees it. Under
    // the tsan preset this is the race check.
    std::vector<std::jthread> readers;
    std::array<std::size_t, 8> seen{};
    for (std::size_t i = 0; i < seen.size(); ++i) {
        readers.emplace_back([d = b, &slot = seen[i]] {
            std::size_t n = 0;
            for (int k = 0; k < 1000; ++k) n += d->lines.size() + d->title.size();
            slot = n;
        });
    }
    readers.clear();   // join
    for (auto n : seen) if (n != 3000) return 4;
    return 0;
}
