# jaal

A typed Elm runtime for C++, plus the platform layer it runs on.

You write `update`. jaal runs the loop, the timers, the background tasks and
the OS waiting, on Linux, macOS and Windows. It doesn't draw anything:
renderers like [maya](https://github.com/1ay1/maya) plug in as hosts.

Status: core, kernel and platform layers are built and tested. maya doesn't
run on it yet.

**Docs** live in [docs/](docs/):

- [scope.md](docs/scope.md): what jaal offers, what it doesn't, what's planned
- [decisions.md](docs/decisions.md): the design decisions and why
- [design.md](docs/design.md): the full technical design
- [concurrency.md](docs/concurrency.md): the threading and memory safety model

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

## Building

Needs GCC 16 or clang 22 (C++26), CMake 3.29+, Ninja.

```sh
cmake --preset dev          # debug; uses ccache and mold/lld when installed
cmake --build --preset dev
ctest --preset dev
```

Other presets: `clang`, `asan`, `tsan`, `release`, `mingw` (Windows cross
build with llvm-mingw; tests run under wine).

To run the whole matrix (gcc, clang, asan, tsan, windows under wine):

```sh
scripts/check.sh
```

It prints one line per preset and exits non-zero if any fails. About
2.5 minutes on a 12-core machine. There's no hosted CI; this is the check.

## What's tested where

| | Linux | Windows | macOS |
|---|---|---|---|
| core, kernel | runs | runs (wine) | compiles |
| reactor | epoll + poll, run | wait_reactor, run under wine | kqueue, compiles only |
| signals | run | handler run directly; not yet via a real console | compiles only |

macOS has not been run on real hardware yet, and Windows has only run
under wine. Both are open.

## Using it from CMake

```cmake
add_subdirectory(jaal)                  # or: find_package(jaal)
target_link_libraries(app PRIVATE jaal::jaal)
```

## License

MIT
