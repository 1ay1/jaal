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
#else
#  error "unknown JAAL_CASE"
#endif
