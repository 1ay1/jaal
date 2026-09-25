// tests/docs/reference_examples.cpp — the reference's code samples, compiled.
//
// docs/reference.md is the page people copy from. A sample that doesn't
// compile is worse than no sample: it costs the reader the time to discover
// that the docs are wrong, and then the trust for every other line on the
// page.
//
// So the samples live here too. Compiling IS passing for most of this;
// main() checks the handful with runtime behaviour.
//
// When you change an API, this breaks — and that is the point. Fix the doc
// in the same commit.

#include <jaal/jaal.hpp>
#include <jaal/host/headless.hpp>
#include <jaal/kernel/guarded.hpp>
#include <jaal/kernel/loop.hpp>

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <variant>

using namespace std::chrono_literals;

// ── "The program" ────────────────────────────────────────────────────────
struct Counter {
    struct Model { int n = 0; };

    struct Inc {};
    struct Reset {};
    using Msg = std::variant<Inc, Reset>;

    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;

    static Cmd init(Model& m) { m.n = 0; return {}; }
    static Cmd update(Model& m, Inc)   { ++m.n;    return {}; }
    static Cmd update(Model& m, Reset) { m.n = 0;  return {}; }
    static Sub subscribe(const Model&) { return Sub::none(); }
};
static_assert(jaal::Program<Counter>);

// The static_assert the reference recommends for the "absent hook" trap.
static_assert(requires(Counter::Model& m) {
                  { Counter::init(m) } -> std::convertible_to<Counter::Cmd>;
              },
              "init must be Cmd init(Model&)");

// ── "Effects: Cmd" ───────────────────────────────────────────────────────
struct Fx {
    struct Model { std::string url; int n = 0; };

    struct Go {};
    struct Loaded { std::string body; };
    struct Tick {};
    using Msg = std::variant<Go, Loaded, Tick>;
    using Cmd = jaal::Cmd<Msg>;

    // Cmd::task — captureless body, Sendable arguments passed after it.
    static Cmd update(Model& m, Go) {
        return Cmd::task(
            [](jaal::Sink<Msg> out, std::stop_token, std::string url) {
                out.send(Msg{Loaded{std::move(url)}});
            },
            m.url);
    }

    static Cmd update(Model& m, Loaded l) {
        m.url = std::move(l.body);
        // batch order matters: interpretation stops at the first quit.
        return Cmd::batch(Cmd::send(Msg{Tick{}}), Cmd::quit(0));
    }

    static Cmd update(Model& m, Tick) {
        ++m.n;
        return Cmd::batch(Cmd::after(10ms, Msg{Tick{}}),
                          Cmd::now([](auto) { return Msg{Tick{}}; }),
                          Cmd::random([](auto&) { return Msg{Tick{}}; }));
    }
};
static_assert(jaal::Program<Fx>);

// ── "Your own effects" ───────────────────────────────────────────────────
struct SaveFile { std::string path, contents; };
using save_file = jaal::pure_fx<SaveFile, "save_file">;

struct WithFx {
    struct Model { int n = 0; };
    struct Save {};
    using Msg = std::variant<Save>;
    using Cmd = jaal::Cmd<Msg, save_file>;

    static Cmd update(Model&, Save) {
        return Cmd(SaveFile{"/tmp/x", "hello"});
    }
};
static_assert(jaal::Program<WithFx>);

// ── "Composition: child" ─────────────────────────────────────────────────
struct Editor {
    struct Model { std::string text; };
    struct Typed { char c; };
    using Msg = std::variant<Typed>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model& m, Typed t) { m.text.push_back(t.c); return {}; }
};

struct WithChild {
    struct ToEditor { Editor::Msg msg; };
    using Msg = std::variant<ToEditor>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;
    using Ed  = jaal::child<Editor, WithChild, ToEditor>;

    struct Model { Editor::Model editor; };

    static Cmd update(Model& m, ToEditor t) { return Ed::update(m.editor, t); }
    // subscribe takes a key PREFIX: two children of the same type would
    // otherwise collide on their stream keys.
    static Sub subscribe(const Model& m) {
        return Ed::subscribe(m.editor, "editor");
    }
};
static_assert(jaal::Program<WithChild>);

// ── "Composition: children" ──────────────────────────────────────────────
struct Tab {
    struct Model { int n = 0; };
    struct Bump {};
    using Msg = std::variant<Bump>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model& m, Bump) { ++m.n; return {}; }
};

struct WithChildren {
    struct ToTab   { int id; Tab::Msg msg; };
    struct NewTab  {};
    struct CloseTab{ int id; };
    using Msg  = std::variant<ToTab, NewTab, CloseTab>;
    using Cmd  = jaal::Cmd<Msg>;
    using Sub  = jaal::Sub<Msg>;
    using Tabs = jaal::children<Tab, WithChildren, ToTab>;

    struct Model { Tabs::map tabs; };

    static Cmd update(Model& m, ToTab t)    { return Tabs::update(m.tabs, t); }
    static Cmd update(Model& m, NewTab)     { return Tabs::add(m.tabs).second; }
    static Cmd update(Model& m, CloseTab c) { Tabs::remove(m.tabs, c.id); return {}; }
    static Sub subscribe(const Model& m)    { return Tabs::subscribe(m.tabs); }
};
static_assert(jaal::Program<WithChildren>);

// ── "debounce<T>" ────────────────────────────────────────────────────────
struct Search {
    struct Typed     { std::string text; };
    struct RunSearch { std::uint64_t token; };
    struct Results   { std::uint64_t token; int rows; };
    using Msg = std::variant<Typed, RunSearch, Results>;
    using Cmd = jaal::Cmd<Msg>;

    struct Model { jaal::debounce<std::string> query; int rows = 0; };

    static Cmd update(Model& m, Typed t) {
        const auto tok = m.query.set(std::move(t.text));
        return Cmd::after(200ms, Msg{RunSearch{tok}});
    }
    static Cmd update(Model& m, RunSearch r) {
        if (!m.query.ready(r.token)) return {};
        return Cmd::task(
            [](jaal::Sink<Msg> out, std::stop_token, std::string, std::uint64_t tok) {
                out.send(Msg{Results{tok, 3}});
            },
            m.query.value(), r.token);
    }
    static Cmd update(Model& m, Results res) {
        if (!m.query.ready(res.token)) return {};
        m.rows = res.rows;
        return {};
    }
};
static_assert(jaal::Program<Search>);

// ── "subs_key" ───────────────────────────────────────────────────────────
struct Keyed {
    struct Model { int panel = 0; bool streaming = false; std::string composer; };
    struct Ping {};
    using Msg = std::variant<Ping>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg>;

    static Cmd update(Model&, Ping) { return {}; }
    static Sub subscribe(const Model& m) {
        if (!m.streaming) return Sub::none();
        return Sub::every(16ms, Msg{Ping{}});
    }
    static auto subs_key(const Model& m) {
        return std::tuple{m.panel, m.streaming, m.composer.empty()};
    }
};
static_assert(jaal::Program<Keyed>);
static_assert(jaal::HasSubsKey<Keyed>);

int main() {
    // ── headless: no host, drive with messages ───────────────────────────
    {
        jaal::headless<Counter> h;
        h.send(Counter::Msg{Counter::Inc{}});
        h.send(Counter::Msg{Counter::Inc{}});
        h.run_until_idle(std::chrono::seconds(1));
        if (h.model().n != 2) return 1;
    }

    // ── guarded<T>: arguments after the lambda ───────────────────────────
    {
        jaal::guarded<std::map<std::string, int>> cache;
        const std::string key = "k";
        cache.with([](auto& m, std::string k) { ++m[k]; }, key);
        const int n = cache.read([](const auto& m) { return int(m.size()); });
        if (n != 1) return 2;
    }

    // ── loop_bound<T>: the kernel's own path ─────────────────────────────
    {
        jaal::kernel::loop_bound<int> state{41};
        jaal::kernel::loop_token tok{jaal::kernel::loop_key{}};
        state.with(tok, [](int& v) { v += 1; });
        if (state.get(tok) != 42) return 3;
    }

    // ── debounce: a superseded token is not ready ────────────────────────
    {
        jaal::debounce<std::string> q;
        if (!q.empty()) return 4;
        const auto first  = q.set("ab");
        const auto second = q.set("abc");
        if (q.ready(first)) return 5;          // superseded
        if (!q.ready(second)) return 6;        // current
        const auto bumped = q.invalidate();
        if (q.ready(second)) return 7;         // invalidated
        if (!q.ready(bumped)) return 8;
        if (q.value() != "abc") return 9;
    }

    return 0;
}
