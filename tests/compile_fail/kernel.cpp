// Compile-fail cases for the kernel's host checks.

#include <jaal/core/core_fx.hpp>
#include <jaal/core/child.hpp>
#include <jaal/host/headless.hpp>
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
    using Cmd = jaal::CoreCmd<Msg>;
    using Sub = jaal::Sub<Msg, jaal::make_row<on_scroll>>;
    static Model init() { return {}; }
    static std::pair<Model, Cmd> update(Model m, Msg) { return {m, Cmd::none()}; }
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
    struct Model { std::shared_ptr<Doc> doc; };
    struct Edit {};
    using Msg = std::variant<Edit>;
    using Cmd = jaal::CoreCmd<Msg>;
    static Model init() { return {std::make_shared<Doc>()}; }
    static std::pair<Model, Cmd> update(Model m, Msg) { ++m.doc->words; return {m, Cmd::none()}; }
};
jaal::timeline<Shared> t({});
#elif JAAL_CASE == 3
// Wrap isn't one of the parent's Msg alternatives: the child's messages
// would have nowhere to go
struct Stray { App::Msg msg; };
using Parent = std::variant<int>;
jaal::child<App, Parent, Stray> c;
#else
#  error "unknown JAAL_CASE"
#endif
