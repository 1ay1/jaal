// tests/core/sync_test.cpp — jaal::Sync and co_owned<T>, checked at compile
// time. If this file builds, the test passed.
#include <jaal/core/co_owned.hpp>
#include <jaal/core/sync.hpp>
#include <jaal/kernel/guarded.hpp>
#include <jaal/kernel/pool.hpp>

#include <atomic>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

using jaal::Sync;

// ── what is Sync ─────────────────────────────────────────────────────────
struct AllConst   { const std::string url; const int port; };
struct Locked     { jaal::guarded<std::vector<int>> v; };
struct Flags      { std::atomic<bool> stop{false}; std::atomic<int> n{0}; };
struct Stops      { const std::stop_source src; };
struct Nested     { AllConst cfg; Locked st; };
struct Mixed      { const std::string name; jaal::guarded<int> n; std::atomic<bool> live{true}; };
struct HoldsOwned { const jaal::co_owned<Flags> flags; };
struct ConstShared { const std::shared_ptr<Flags> f; const std::unique_ptr<Locked> l; };

static_assert(Sync<AllConst>);
static_assert(Sync<Locked>);
static_assert(Sync<Flags>);
static_assert(Sync<Stops>);
static_assert(Sync<Nested>);
static_assert(Sync<Mixed>);
static_assert(Sync<HoldsOwned>);
static_assert(Sync<ConstShared>);
static_assert(Sync<jaal::guarded<std::string>>);

// ── what isn't ───────────────────────────────────────────────────────────
struct Writable   { std::string s; };                       // anyone can write it
struct ConstPtr   { const std::shared_ptr<int> p; };        // the int is writable
struct ReseatPtr  { std::shared_ptr<Flags> p; };            // the pointer can change
struct WithRef    { int& r; };
struct ReseatHandle { jaal::co_owned<Flags> flags; };       // the handle can change
struct AtomicPtr  { std::atomic<int*> p; };                 // the pointee is unguarded
struct LooseStop  { std::stop_source src; };                // reassignable
class  Opaque     { int x = 0; public: int get() const { return x; } };

static_assert(!Sync<Writable>);
static_assert(!Sync<ConstPtr>);
static_assert(!Sync<ReseatPtr>);
static_assert(!Sync<WithRef>);
static_assert(!Sync<ReseatHandle>);
static_assert(!Sync<AtomicPtr>);
static_assert(!Sync<LooseStop>);
static_assert(!Sync<Opaque>);
static_assert(!Sync<int>);           // a bare int is a writable field
static_assert(std::is_same_v<jaal::sync_culprit_t<Writable>, std::string>);

// An opt-in is a human's claim.
class Counter { std::atomic<int> n_{0}; public: void bump() { ++n_; } };
template <> inline constexpr bool jaal::sync_opt_in<Counter> = true;
static_assert(Sync<Counter>);

// ── co_owned ─────────────────────────────────────────────────────────────
static_assert(jaal::Sendable<jaal::co_owned<Flags>>);
static_assert(!std::is_default_constructible_v<jaal::co_owned<Flags>>);

// A class that hands out handles to itself opts in.
class Server : public std::enable_shared_from_this<Server> {
    std::atomic<int> n_{0};
public:
    void bump() { ++n_; }
    int  n() const { return n_; }
};
template <> inline constexpr bool jaal::sync_opt_in<Server> = true;

// ── the job gate ─────────────────────────────────────────────────────────
using jaal::kernel::CheckedJob;
using flags_t = jaal::co_owned<Flags>;

inline auto captureless = [](std::stop_token, flags_t) {};
inline int  g_local     = 0;
inline auto capturing   = [p = &g_local](std::stop_token) { (void)p; };
inline auto takes_ptr   = [](std::stop_token, int*) {};
inline auto no_token    = [] {};

static_assert(CheckedJob<decltype(captureless), flags_t>);
static_assert(!CheckedJob<decltype(capturing)>);
static_assert(!CheckedJob<decltype(takes_ptr), int*>);
static_assert(!CheckedJob<decltype(no_token)>);

int main() {
    auto f = flags_t::make();
    f->n = 3;
    auto g = std::move(f);           // move copies: f still works
    if (f->n != 3 || g->n != 3) return 1;

    auto s = jaal::co_owned<Server>::make();
    jaal::co_owned<Server>::of(*s)->bump();
    return s->n() == 1 ? 0 : 2;
}
