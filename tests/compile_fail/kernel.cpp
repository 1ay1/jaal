// Compile-fail cases for the kernel's host checks.

#include <jaal/core/core_fx.hpp>
#include <jaal/core/child.hpp>
#include <jaal/host/headless.hpp>
#include <jaal/kernel/teardown.hpp>
#include <jaal/kernel/timeline.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <variant>

struct Key { char c; };
struct Click { int x, y; };
struct Scroll { int dy; };

struct on_scroll {
    static constexpr std::string_view name = "on_scroll";
    using event_type = Scroll;
    template <class Msg> struct type { std::function<std::optional<Msg>(const Scroll&)> f; };
    template <class F, class M>
    static auto fmap(F&& f, type<M> e) -> type<std::invoke_result_t<F, M>> {
        using B = std::invoke_result_t<F, M>;
        return {[g = std::move(e.f), f = std::forward<F>(f)](const Scroll& s) -> std::optional<B> {
            if (auto m = g(s)) return f(std::move(*m));
            return std::nullopt;
        }};
    }
    template <class M>
    static std::optional<M> route(const type<M>& p, const Scroll& s) { return p.f(s); }
};

struct App {
    struct Model {};
    struct S {};
    using Msg = std::variant<S>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg, on_scroll>;
    static Cmd update(Model&, S) { return {}; }
    static Sub subscribe(const Model&) {
        return Sub(on_scroll::type<Msg>{[](const Scroll&) -> std::optional<Msg> { return S{}; }});
    }
};

#if JAAL_CASE == 1
// the program wants scroll events; the host only produces keys and clicks
jaal::headless<App, std::variant<Key, Click>> h;
#elif JAAL_CASE == 2
// a model that shares a mutable Doc: a copy isn't a snapshot, so a
// timeline over it would show the wrong past
struct Shared {
    struct Doc { int words = 0; };
    struct Model { std::shared_ptr<Doc> doc = std::make_shared<Doc>(); };
    struct Edit {};
    using Msg = std::variant<Edit>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model& m, Edit) { ++m.doc->words; return {}; }
};
jaal::timeline<Shared> t({});
#elif JAAL_CASE == 3
// Wrap isn't a case of the parent's Msg: the child's messages would have
// nowhere to go
struct Stray { App::Msg msg; };
struct Parent {
    struct Model { App::Model child; };
    using Msg = std::variant<int>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model&, int) { return {}; }
};
auto c = jaal::child<App, Parent, Stray>::init(*new App::Model);
#elif JAAL_CASE == 4
// a Msg case with no update: the error names the case and the line to add
struct Counter {
    struct Model { int n = 0; };
    struct Inc {};
    struct Reset {};
    using Msg = std::variant<Inc, Reset>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model& m, Inc) { ++m.n; return {}; }
};
jaal::headless<Counter> h;
#elif JAAL_CASE == 5
// update returns an effect the program's Cmd doesn't list
struct Beep {};
using beep = jaal::pure_fx<Beep, "beep">;
struct Noisy {
    struct Model {};
    struct Go {};
    using Msg = std::variant<Go>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model&, Go) { return Beep{}; }
};
#elif JAAL_CASE == 6
// shutting the kernel down by hand: finish() needs a teardown_key, and only
// kernel::teardown can make one, so the order (signals off, host.release(),
// finish) can't be taken apart. A kernel that shuts down with its signal
// handlers still installed is unkillable by ^C for the whole grace (D34/D35).
void f() {
    struct Counter {
        struct Model { int n = 0; };
        struct Inc {};
        using Msg = std::variant<Inc>;
        using Cmd = jaal::Cmd<Msg>;
        static Cmd update(Model& m, Inc) { ++m.n; return {}; }
    };
    jaal::recorder rec;
    auto k = jaal::kernel::kernel<Counter>::start(rec);
    (void)std::move(k).finish();
}
#elif JAAL_CASE == 7
// forging the key: its constructor is private, so a caller can't make one
// to get at finish().
void f() { (void)jaal::kernel::teardown_key{}; }
#elif JAAL_CASE == 8
// a missing leaf two levels down: the error must name the LEAF and the PATH
// it was reached by, not just "Msg".
struct CEnter {}; struct CBack {}; struct CSubmit {};
using ComposerMsg = std::variant<CEnter, CBack, CSubmit>;
struct Nested {
    struct Model { int n = 0; };
    using Msg = std::variant<ComposerMsg>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model&, CEnter) { return {}; }
    static Cmd update(Model&, CBack)  { return {}; }
    // CSubmit: no handler, and ComposerMsg didn't opt into group handling
};
jaal::headless<Nested> h;
#elif JAAL_CASE == 9
// a host effect that answers with something that isn't the program's Msg:
// named, rather than a wall of std::optional template errors.
struct Ping {};
using ping = jaal::pure_fx<Ping, "ping">;
struct Pinger {
    struct Model { int n = 0; };
    struct Go {};
    using Msg = std::variant<Go>;
    using Cmd = jaal::Cmd<Msg, ping>;
    static Cmd update(Model&, Go) { return Cmd(Ping{}); }
};
struct bad_host { std::string handle(Ping) { return "pong"; } };
void f() {
    bad_host h;
    auto k = jaal::kernel::kernel<Pinger, jaal::kernel::no_events,
                                  jaal::platform::sim_clock>::start(h);
    k.dispatch(Pinger::Go{});
    k.step(h);
}
#elif JAAL_CASE == 10 || JAAL_CASE == 11
// A child that reports (D41).
struct Kid {
    struct Model { int n = 0; };
    struct Poke {};
    using Msg = std::variant<Poke>;
    struct Done {};
    struct Other {};
    using Out = std::variant<Done>;
    using Cmd = jaal::Cmd<Msg, jaal::fx::report<Out>>;
#  if JAAL_CASE == 10
    static Cmd update(Model&, Poke) { return Cmd::report(Done{}); }
#  else
    // 11: reporting something the child's Out doesn't list
    static Cmd update(Model&, Poke) { return Cmd::report(Other{}); }
#  endif
};
struct Parent {
    struct ToKid { Kid::Msg msg; };
    struct FromKid { Kid::Out out; };
    using Msg = std::variant<ToKid, FromKid>;
    using Cmd = jaal::Cmd<Msg>;
#  if JAAL_CASE == 10
    // 10: embedded WITHOUT saying where its reports go
    using K = jaal::child<Kid, Parent, ToKid>;
#  else
    using K = jaal::child<Kid, Parent, ToKid, FromKid>;
#  endif
    struct Model { Kid::Model kid; };
    static Cmd update(Model& m, ToKid t) { return K::update(m.kid, std::move(t)); }
    static Cmd update(Model&, FromKid) { return {}; }
};
void f() {
    Parent::Model m;
    (void)Parent::update(m, Parent::ToKid{Kid::Poke{}});
}
#else
#  error "unknown JAAL_CASE"
#endif
