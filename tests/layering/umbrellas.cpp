// tests/layering/umbrellas.cpp — every layer umbrella compiles ALONE.
//
// One translation unit per umbrella would be the thorough version; this file
// is the cheap guard that catches the common rot: a new header added to a
// layer directory but not to its umbrella, or an umbrella that only works
// because something else was included first.
//
// Each umbrella is included FIRST in its own scope-free block below, in
// dependency order, so if <jaal/core.hpp> secretly needed the kernel this
// stops compiling.
//
// This is a static test: compiling is passing (tests/CMakeLists.txt).

#include <jaal/meta.hpp>       // no dependencies at all
#include <jaal/core.hpp>       // meta only
#include <jaal/platform.hpp>   // meta + core
#include <jaal/kernel.hpp>     // meta + core + platform
#include <jaal/host.hpp>       // all of the above
#include <jaal/jaal.hpp>       // the everything header

#include <string>
#include <variant>

namespace {

// The umbrellas must actually EXPORT what they claim, not merely parse.
// A few names from each layer, so a header dropped from an umbrella fails
// here rather than in a user's build.

// meta
static_assert(jaal::meta::list<int, char>::size == 2);

// core: the types, the effects, the composition helpers, the model values
struct Msg { int v; };
using Cmd = jaal::CoreCmd<Msg>;
using Sub = jaal::CoreSub<Msg>;
static_assert(jaal::core_fx::size == 6);          // quit send after task now random
static_assert(jaal::Sendable<Msg>);
static_assert(jaal::Frozen<Msg>);
static_assert(jaal::Sendable<jaal::debounce<std::string>>);
static_assert(jaal::Frozen<jaal::throttle>);
static_assert(std::same_as<jaal::rng::result_type, std::uint64_t>);

// A program, so child<> and children<> are instantiable from the umbrella.
struct Leaf {
    struct Model { int n = 0; };
    using Msg = ::Msg;
    using Cmd = jaal::CoreCmd<Msg>;
    static Model init() { return {}; }
    static std::pair<Model, Cmd> update(Model m, Msg) { return {m, Cmd::none()}; }
};
static_assert(jaal::Program<Leaf>);

struct ToLeaf { int id; Leaf::Msg msg; };
struct One    { Leaf::Msg msg; };
using Parent  = std::variant<ToLeaf, One>;
using Kids    = jaal::children<Leaf, Parent, ToLeaf>;
using Kid     = jaal::child<Leaf, Parent, One>;
static_assert(std::same_as<Kids::id_type, int>);
static_assert(std::same_as<Kid::model_type, Leaf::Model>);

// platform
static_assert(jaal::platform::Clock<jaal::platform::steady_clock>);
static_assert(jaal::platform::Clock<jaal::platform::sim_clock>);
static_assert(jaal::platform::Reactor<jaal::platform::native_reactor>);

// kernel + host
using K = jaal::kernel::kernel<Leaf, jaal::kernel::no_events,
                               jaal::platform::sim_clock>;
using HL = jaal::headless<Leaf>;
using G  = jaal::given<Leaf>;
using S  = jaal::sim<Leaf>;

}  // namespace

// The assertions above are all at compile time; this exists only so the
// target links like any other.
int main() { return 0; }
