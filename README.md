# jaal

A typed Elm runtime for C++, plus the platform layer it runs on.

You write `update`. jaal runs the loop, the timers, the background tasks and
the OS waiting, on Linux, macOS and Windows. It doesn't draw anything:
renderers like [maya](https://github.com/1ay1/maya) plug in as hosts.

Status: core, kernel and platform layers are built and tested. maya doesn't
run on it yet.

**Docs** live in [docs/](docs/):

- [scope.md](docs/scope.md): what jaal offers, what it doesn't, what's planned
- [hosts.md](docs/hosts.md): how to write a host (a terminal, a GUI, a server)
- [decisions.md](docs/decisions.md): the design decisions and why
- [design.md](docs/design.md): the full technical design
- [concurrency.md](docs/concurrency.md): the threading and memory safety model

Headers come one per layer: `<jaal/jaal.hpp>` is everything, and
`<jaal/core.hpp>`, `<jaal/kernel.hpp>`, `<jaal/platform.hpp>`,
`<jaal/host.hpp>`, `<jaal/meta.hpp>` are the individual layers. A library of
effect descriptors needs only `core`, and never pulls in a thread or an OS
handle.

## The idea in one example

```cpp
struct Ticker {
    struct Model { int ticks = 0; };
    struct Tick {}; struct Interrupted {};
    using Msg = std::variant<Tick, Interrupted>;

    using Cmd = jaal::CoreCmd<Msg>;
    using Sub = jaal::Sub<Msg, jaal::row_union<jaal::core_src,
                                               jaal::make_row<jaal::fx::on_signal>>>;

    static Model init() { return {}; }

    static std::pair<Model, Cmd> update(Model m, Msg msg) {
        if (std::holds_alternative<Interrupted>(msg)) return {m, Cmd::quit(0)};
        ++m.ticks;
        return {m, m.ticks == 5 ? Cmd::quit(0) : Cmd::none()};
    }

    static Sub subscribe(const Model&) {
        return Sub::batch(Sub::every(500ms, Tick{}),
                          Sub::on_signal({jaal::sig::interrupt},
                                         [](jaal::sig) { return Msg{Interrupted{}}; }));
    }
};

int main() { return jaal::run<Ticker>(); }
```

- The `Cmd` type says which effects `update` can ask for. A host must be
  able to run every one of them, or it won't compile.
- Signals arrive as messages on the loop thread. A program that doesn't
  subscribe to Ctrl+C still stops on it (exit code 130), like any process.
- The same program runs on the real platform (`jaal::run`), or on a
  simulated one with a fake clock for tests (`jaal::headless`).

One behaviour to know: a signal the process inherited as ignored stays
ignored, so `nohup` keeps working. That includes a program started in the
background by a non-interactive shell (`cmd &` in a script), which the
shell starts with SIGINT ignored. In that case Ctrl+C won't reach it, the
same as for any other program.

Full example: [examples/ticker.cpp](examples/ticker.cpp).

## Finding races with a seed

`jaal::sim` runs a program with task latency, same-instant ordering and
injected crashes all picked by one seed. `explore` tries many seeds and
hands back the first one that breaks an invariant:

```cpp
auto r = jaal::explore<Search>(1, 500, {}, [](jaal::sim<Search>& s) {
    s.at(0ms, Type{1});
    s.at(2ms, Type{2});                 // fetch results come back in any order
    s.check("never show stale results", [last = 0](const Model& m) mutable {
        bool ok = m.shown >= last; last = m.shown; return ok;
    });
});
if (!r.ok()) puts(r.failure->describe().c_str());
// seed 1 broke "never show stale results" at step 7 (t=9.629ms) after 6 message(s)
```

The same seed breaks the same way every time, on every platform, and
`r.failure->messages` replays through `jaal::replay`. A run takes about a
microsecond, so hundreds of thousands of seeds a second.

Then walk the failing run step by step:

```cpp
jaal::timeline<Search> t(r.failure->messages);
auto step = t.first_bad([](const Model& m) { return m.shown >= m.query || m.shown == 0; });
puts(jaal::to_string(t.changes(*step)).c_str());   // ".1: 2 -> 1"
```

## Randomness you can replay

A program that reads its own generator can't be replayed: the bug you're
chasing never comes back. So drawing is an effect, and the kernel owns the
stream (Elm's `Random.generate`):

```cpp
static std::pair<Model, Cmd> update(Model m, Msg msg) {
    if (std::holds_alternative<Roll>(msg))
        return {m, Cmd::random([](jaal::rng& r) -> Msg {
                       return Rolled{r.in(1, 6)};       // or shuffle a deck
                   })};
    ...
}
```

`jaal::rng` is splitmix64: 8 bytes of state and the same sequence on every
platform and standard library (the distributions in `<random>` are not
portable, which would break replay). It has `next`, `below`, `in`,
`uniform`, `between`, `chance`, `pick` and `split`.

A real run picks its own seed and reports it, so a crash is reproducible:

```cpp
jaal::run_options o;
o.on_seed = [](std::uint64_t s) { log("seed %llu", s); };   // log it
o.kernel.random_seed = from_the_log;                       // replay it
```

In tests the seed is fixed instead: `headless` and `given` default to one,
and `sim` derives it from the sim seed, so one seed still controls the whole
run.

## Building out a real app

Three things every interactive app needs, that jaal has so you don't write
them again.

**A list of sub-programs.** Tabs, panes, sessions, open buffers: each with
its own model, timers and streams, and messages routed back to the right one.

```cpp
struct ToTab { int id; Tab::Msg msg; };
using Tabs = jaal::children<Tab, Msg, ToTab>;

struct Model { Tabs::map tabs; };            // id -> Tab::Model

if (auto r = Tabs::match(msg)) return {m, Tabs::update(m.tabs, *r)};
auto [id, cmd] = Tabs::add(m.tabs);          // runs the new tab's init()
Tabs::remove(m.tabs, id);                    // and its streams stop
```

The part that's easy to get wrong: two tabs both subscribing to stream key
`"fetch"` would reconcile to **one** subscription, so one tab's feed would
drive the other. `children<>` prefixes each child's keys with its id, so they
stay separate and stop independently. For a single child in a fixed slot,
`jaal::child<>` is the simpler one.

**Debouncing, without the stale-result bug.** A user types 8 characters; you
want one search, for the last text, and no late answer overwriting a newer
one.

```cpp
struct Model { jaal::debounce<std::string> query; };

// on each keystroke: newest wins
auto tok = m.query.set(text);
return {m, Cmd::after(200ms, Msg{Fire{tok}})};

// when a timer or a result arrives
if (!m.query.ready(tok)) return {m, Cmd::none()};    // superseded: drop it
```

The token is what makes it correct. Guarding on the text instead looks
equivalent and isn't: type `ab`, delete to `a`, and the stale timer for `a`
matches again. `jaal::throttle` is the by-time sibling, for redraws.

**A message back into the loop**, so one update path can reuse another:

```cpp
return {m, Cmd::send(Msg{Refresh{}})};       // folded in this step
```

Not `after(0ms, ...)`: that goes through the timer heap and arrives a step
late, which costs a frame and makes tests wait on a clock.

## Building

Needs a C++26 compiler with structured binding packs: GCC 16, clang 22, or
Apple clang 21 (Xcode 26). CMake 3.29+, Ninja. On macOS, jaal passes
`-std=c++2c` itself when CMake is older than the compiler and doesn't know
the flag yet.

```sh
cmake --preset dev          # debug; uses ccache and mold/lld when installed
cmake --build --preset dev
ctest --preset dev
```

Other presets: `clang`, `asan`, `tsan`, `release`, `mingw` (Windows cross
build with llvm-mingw; tests run under wine).

To run the whole matrix:

```sh
scripts/check.sh              # Linux: gcc, clang, asan, tsan, windows-under-wine
                             # macOS: clang, asan, tsan, release
scripts/check.sh asan tsan   # or name the presets you want
```

It prints one line per preset and exits non-zero if any fails. About
2.5 minutes on a 12-core machine. There's no hosted CI; this is the check.

## What's tested where

| | Linux | macOS | Windows |
|---|---|---|---|
| core, kernel | runs | runs | runs (wine) |
| reactor | epoll + poll, run | kqueue + poll, run | wait_reactor, run under wine |
| signals | run | run (real SIGINT/TERM/HUP) | handler run directly; not yet via a real console |
| sanitizers | asan, tsan | asan, tsan | — |

Linux and macOS both run the whole suite on real hardware, under the debug,
clang, asan, tsan and release presets. Windows has only run under wine;
that's the open one.

## Using it from CMake

```cmake
add_subdirectory(jaal)                  # or: find_package(jaal)
target_link_libraries(app PRIVATE jaal::jaal)
```

## License

MIT
