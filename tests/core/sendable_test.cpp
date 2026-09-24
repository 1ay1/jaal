// tests/core/sendable_test.cpp — static tests for jaal::Sendable.
// Compiling is passing.

#include <jaal/core/sendable.hpp>

#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

using jaal::Sendable;
using jaal::sendable_v;
using jaal::sendable_culprit_t;

// ── leaves ───────────────────────────────────────────────────────────────
enum class Color { red };
static_assert(Sendable<int>);
static_assert(Sendable<double>);
static_assert(Sendable<bool>);
static_assert(Sendable<Color>);
static_assert(Sendable<std::byte>);
static_assert(Sendable<std::monostate>);

static_assert(!Sendable<int*>);
static_assert(!Sendable<const char*>);
static_assert(!Sendable<int&>);                    // not an object type
static_assert(!Sendable<void>);
static_assert(!sendable_v<int&>);
static_assert(!sendable_v<void (*)(int)>);         // could point into a plugin that unloads
struct S0 { int x; };
static_assert(!sendable_v<int S0::*>);

// ── std wrappers ─────────────────────────────────────────────────────────
static_assert(Sendable<std::string>);
static_assert(Sendable<std::wstring>);
static_assert(Sendable<std::vector<int>>);
static_assert(Sendable<std::vector<std::string>>);
static_assert(Sendable<std::optional<std::string>>);
static_assert(Sendable<std::variant<int, std::string>>);
static_assert(Sendable<std::map<std::string, std::vector<int>>>);
static_assert(Sendable<std::unordered_map<std::string, int>>);
static_assert(Sendable<std::pair<int, std::string>>);
static_assert(Sendable<std::tuple<int, std::string, double>>);
static_assert(Sendable<std::array<std::string, 4>>);
static_assert(Sendable<std::unique_ptr<std::string>>);
static_assert(Sendable<std::expected<std::string, int>>);
static_assert(Sendable<std::expected<void, int>>);
static_assert(Sendable<std::chrono::milliseconds>);
static_assert(Sendable<std::chrono::steady_clock::time_point>);
static_assert(Sendable<std::stop_token>);

static_assert(!Sendable<std::string_view>);
static_assert(!Sendable<std::span<int>>);
static_assert(!Sendable<std::span<const int, 4>>);
static_assert(!Sendable<std::shared_ptr<int>>);
static_assert(!Sendable<std::shared_ptr<const int>>);   // Frozen's job, not Sendable's
static_assert(!Sendable<std::weak_ptr<int>>);
static_assert(!Sendable<std::vector<int*>>);
static_assert(!Sendable<std::vector<std::string_view>>);
static_assert(!Sendable<std::optional<std::string_view>>);
static_assert(!Sendable<std::variant<int, std::string_view>>);
static_assert(!Sendable<std::map<std::string, std::span<int>>>);
static_assert(!Sendable<std::unique_ptr<std::string_view>>);
static_assert(!Sendable<std::pair<int, int*>>);

// ── plain structs, checked field by field ────────────────────────────────
struct Empty {};
struct Plain { int a; std::string b; };
struct Nested { Plain p; std::vector<Plain> ps; std::optional<double> d; };
struct CArray { int xs[3]; Plain ps[2]; };
struct HiddenView { int a; std::string_view text; };
struct DeepHidden { Nested n; std::vector<HiddenView> lines; };
struct HiddenPtr { std::string s; const char* raw; };
struct RefField { int& r; };
struct MutableOk { mutable int cache; };   // mutable is Frozen's concern, not Sendable's

static_assert(Sendable<Empty>);
static_assert(Sendable<Plain>);
static_assert(Sendable<Nested>);
static_assert(Sendable<CArray>);
static_assert(Sendable<MutableOk>);
static_assert(!Sendable<HiddenView>);
static_assert(!Sendable<DeepHidden>);
static_assert(!Sendable<HiddenPtr>);
static_assert(!sendable_v<RefField>);

// A realistic Msg.
struct GotLine { std::string text; };
struct Resized { int w, h; };
struct Loaded  { std::expected<std::vector<std::string>, std::string> lines; };
using Msg = std::variant<GotLine, Resized, Loaded>;
static_assert(Sendable<Msg>);

struct BadLine { std::string_view text; };
using BadMsg = std::variant<GotLine, Resized, BadLine>;
static_assert(!Sendable<BadMsg>);

// ── culprit: the innermost offender ──────────────────────────────────────
static_assert(std::is_void_v<sendable_culprit_t<Msg>>);
static_assert(std::is_same_v<sendable_culprit_t<BadMsg>, std::string_view>);
static_assert(std::is_same_v<sendable_culprit_t<DeepHidden>, std::string_view>);
static_assert(std::is_same_v<sendable_culprit_t<HiddenPtr>, const char*>);
static_assert(std::is_same_v<sendable_culprit_t<std::vector<std::shared_ptr<int>>>,
                             std::shared_ptr<int>>);

// ── recursive types terminate, and still find bad fields ────────────────
struct Node { int v; std::vector<Node> kids; };
struct Tree { std::unique_ptr<Tree> left, right; std::string label; };
struct BadNode { std::vector<BadNode> kids; int* p; };
struct MutualA;
struct MutualB { std::vector<MutualA> as; };
struct MutualA { std::vector<MutualB> bs; std::string s; };
static_assert(Sendable<Node>);
static_assert(Sendable<Tree>);
static_assert(!Sendable<BadNode>);
static_assert(Sendable<MutualA>);

// ── classes jaal can't see into ──────────────────────────────────────────
class Opaque {
    [[maybe_unused]] int fd_ = -1;
public:
    Opaque() = default;
};
struct DerivedBoth : Plain { int extra; };   // fields in base and derived: can't bind
union Raw { int i; float f; };

static_assert(!Sendable<Opaque>);
static_assert(!Sendable<DerivedBoth>);
static_assert(!Sendable<Raw>);
static_assert(std::is_same_v<sendable_culprit_t<std::vector<Opaque>>, Opaque>);

// opt-in / opt-out
class OwnedHandle {
    [[maybe_unused]] int fd_ = -1;
public:
    OwnedHandle() = default;
    OwnedHandle(OwnedHandle&&) noexcept = default;
    OwnedHandle& operator=(OwnedHandle&&) noexcept = default;
};
template <> inline constexpr bool jaal::sendable_opt_in<OwnedHandle> = true;
static_assert(Sendable<OwnedHandle>);
static_assert(Sendable<std::vector<OwnedHandle>>);

struct ThreadBound { int tid; };             // structurally fine, semantically not
template <> inline constexpr bool jaal::sendable_opt_out<ThreadBound> = true;
static_assert(!Sendable<ThreadBound>);
static_assert(!Sendable<std::optional<ThreadBound>>);

// ── must be movable ──────────────────────────────────────────────────────
// A struct is an aggregate only without user-declared constructors, so
// "structurally fine but not movable" needs a non-movable FIELD, not a
// deleted constructor on the struct itself.
struct NoMove {
    NoMove() = default;
    NoMove(NoMove&&) = delete;
    NoMove& operator=(NoMove&&) = delete;
};
template <> inline constexpr bool jaal::sendable_opt_in<NoMove> = true;
struct Pinned { int x; NoMove m; };
static_assert(sendable_v<Pinned>);      // structure is fine...
static_assert(!Sendable<Pinned>);       // ...but it can't be moved, so it can't be sent

// ── cv on the outside doesn't matter ─────────────────────────────────────
static_assert(sendable_v<const Plain>);
static_assert(!sendable_v<const HiddenView>);

// ── diagnostics text ─────────────────────────────────────────────────────
constexpr auto why = jaal::sendable_reason<BadMsg>();
static_assert(why.view().find("is not Sendable") != std::string_view::npos);
static_assert(why.view().find("string_view") != std::string_view::npos);
static_assert(why.view().find("borrows memory") != std::string_view::npos);

constexpr auto why_opaque = jaal::sendable_reason<std::vector<Opaque>>();
static_assert(why_opaque.view().find("sendable_opt_in") != std::string_view::npos);

constexpr auto why_shared = jaal::sendable_reason<std::shared_ptr<int>>();
static_assert(why_shared.view().find("jaal::shared") != std::string_view::npos);

// A huge Msg variant must not push the reason off the end of the message.
template <int I> struct Wide { std::string s; };
template <class Seq> struct wide_variant;
template <int... Is> struct wide_variant<std::integer_sequence<int, Is...>> {
    using type = std::variant<Wide<Is>..., BadLine>;
};
using HugeMsg = typename wide_variant<std::make_integer_sequence<int, 150>>::type;
static_assert(jaal::meta::type_name<HugeMsg>().size() > 1000);
constexpr auto why_huge = jaal::sendable_reason<HugeMsg>();
static_assert(why_huge.view().find("...") != std::string_view::npos);
static_assert(why_huge.view().find("a view that borrows memory") != std::string_view::npos);

int main() { return 0; }
