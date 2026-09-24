# jaal

A typed Elm runtime for C++, plus the platform layer it runs on.

You write `update`. jaal runs the loop, the timers, the background tasks and
the OS waiting, on Linux, macOS and Windows. It doesn't draw anything:
renderers like [maya](https://github.com/1ay1/maya) plug in as hosts.

Status: design stage. See [DESIGN.md](DESIGN.md).

## The idea in one example

```cpp
struct Counter {
    struct Model { int n = 0; };
    struct Tick {};
    using Msg = std::variant<Tick>;

    using Cmd = jaal::Cmd<Msg, jaal::row<jaal::fx::quit>>;
    using Sub = jaal::Sub<Msg, jaal::core_src>;

    static Model init() { return {}; }

    static auto update(Model m, Msg) -> std::pair<Model, Cmd> {
        ++m.n;
        return {m, m.n == 10 ? Cmd::quit() : Cmd::none()};
    }

    static auto subscribe(const Model&) -> Sub {
        return Sub::every(100ms, Tick{});
    }
};

int main() { return jaal::run<Counter>(jaal::headless{}); }
```

- The `Cmd` type says which effects `update` can ask for. Here, only `quit`.
- A host must be able to run every effect a program uses, or it won't
  compile.
- The same program runs on a real platform, or a simulated one with a fake
  clock for tests.

## License

MIT
