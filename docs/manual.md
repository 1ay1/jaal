# The jaal manual

This is the guided path: build one small app, a search box, a step at a
time, and meet each part of jaal where you need it. When you want every
detail of one type, look it up in [reference.md](reference.md).

Every example in this manual is compiled and run by
`tests/docs/manual_examples.cpp`, so it can't drift from the library.

**Contents**

1. [What jaal is](#1-what-jaal-is)
2. [Your first program](#2-your-first-program)
3. [Messages and update](#3-messages-and-update)
4. [Effects: asking for things](#4-effects-asking-for-things)
5. [Background work](#5-background-work)
6. [Latest wins: keys](#6-latest-wins-keys)
7. [Timers and debouncing](#7-timers-and-debouncing)
8. [Input](#8-input)
9. [Subscriptions: things that keep running](#9-subscriptions-things-that-keep-running)
10. [Splitting an app into parts](#10-splitting-an-app-into-parts)
11. [Testing](#11-testing)
12. [Running it for real](#12-running-it-for-real)
13. [When the compiler says no](#13-when-the-compiler-says-no)
14. [Rules of thumb](#14-rules-of-thumb)

---

## 1. What jaal is

jaal is the Elm loop for C++. Your program is three things:

- a **Model**: all of its state, in one struct
- **Messages**: everything that can happen, as plain structs
- **update**: given the model and one message, change the model and say
  what should happen next

jaal does the rest: the loop, timers, background threads, waiting on the
OS, signals. It never draws anything. Drawing, and turning raw input into
typed events, belongs to a **host** (a terminal UI, a GUI, a server). This
manual uses the test hosts that ship with jaal, so everything runs without
a screen.

Why this shape: `update` runs on one thread, one message at a time, so your
model has no locks and no races. Everything that touches the outside world
comes back as a message, so you can test the whole app by feeding it
messages and looking at the model.

Include everything with one header:

```cpp
#include <jaal/jaal.hpp>
```

## 2. Your first program

A counter:

```cpp
struct Counter {
    struct Model { int n = 0; };

    struct Inc {};
    struct Reset {};
    using Msg = std::variant<Inc, Reset>;

    using Cmd = jaal::Cmd<Msg>;

    static Cmd update(Model& m, Inc)   { ++m.n; return {}; }
    static Cmd update(Model& m, Reset) { m.n = 0; return {}; }
};
```

That's a complete program. Each part:

- `Model` is the state. It must be default-constructible.
- `Msg` is a `std::variant` of message structs. Each case is a plain struct.
- `update` has one overload per case. It changes the model in place and
  returns a `Cmd`: what should happen next. `{}` means nothing.

Drive it with the `headless` test host:

```cpp
jaal::headless<Counter> h;
h.send(Counter::Inc{});
h.send(Counter::Inc{});
assert(h.model().n == 2);
```

Forget an overload and it doesn't compile. The error names the case and
the line to add:

```
'Counter' has no update for message 'Counter::Reset';
add: static Cmd update(Model&, Counter::Reset)
```

## 3. Messages and update

From here on we build a search box. Start with the model and the messages
for typing:

```cpp
struct Search {
    struct Model {
        std::string query;
        std::vector<std::string> results;
        bool searching = false;
        std::string error;
    };

    struct Typed  { char c; };
    struct Clear  {};
    using Msg = std::variant<Typed, Clear /* more below */>;
    // ...
};
```

A few rules that keep `update` easy to reason about:

- **update does the state change and nothing else.** No I/O, no sleeping,
  no threads. Anything that touches the world is an effect (next section).
- **Messages are values.** They're copied or moved around, possibly
  between threads, so a message must own what it holds: `std::string`, not
  `std::string_view`; a `std::vector`, not a pointer. jaal checks this (the
  rule is called `Sendable`), and a message that borrows doesn't compile.
- **Group with nested variants** when the list gets long. A `Msg` case can
  itself be a variant, and you write `update` for the leaves; jaal routes
  down to them:

```cpp
using Edit = std::variant<Typed, Clear>;             // a group
using Msg  = std::variant<Edit, Quit>;

static Cmd update(Model& m, Typed t);                // the leaves
static Cmd update(Model& m, Clear);
```

## 4. Effects: asking for things

`update` can't do I/O, so it *asks*. It returns a `Cmd`, which is data
describing what to do. The kernel carries it out and, when there's a
result, sends it back as a message.

The core effects:

| | |
|---|---|
| `Cmd::send(msg)` | fold `msg` next, in the same step |
| `Cmd::after(200ms, msg)` | fold `msg` later |
| `Cmd::quit(code)` | end the program |
| `Cmd::now(f)` | read the clock: `f(now)` becomes a message |
| `Cmd::random(f)` | roll dice from a seeded generator |
| `Cmd::batch(a, b, ...)` | several, in order |

```cpp
static Cmd update(Model& m, Clear) {
    m.query.clear();
    m.results.clear();
    return Cmd::send(Cleared{});            // tell the rest of the app
}
```

Because effects are values, a test can check what `update` asked for
without running any of it. Section 11 shows how.

**Your own effects.** A host can add effects (write a file, ring a bell).
A program lists the extra ones in its `Cmd`, and a host that can't run one
of them won't compile:

```cpp
struct Bell {};
using ring = jaal::pure_fx<Bell, "ring">;
using Cmd = jaal::Cmd<Msg, ring>;           // core effects + ring
```

## 5. Background work

Searching takes time, so it runs off the loop. `perform` runs a function on
a worker thread and delivers what it returns as a message:

```cpp
static std::vector<std::string> run_search(const std::string& q);   // slow

struct Found { std::vector<std::string> items; };

static Cmd update(Model& m, Typed t) {
    m.query += t.c;
    m.searching = true;
    return Cmd::perform<Found>(&run_search, m.query);
}
static Cmd update(Model& m, Found f) {
    m.results = std::move(f.items);
    m.searching = false;
    return {};
}
```

`perform<Found>(body, args...)` calls `body(args...)` on a worker and sends
`Found{result}` back. A body that returns nothing sends `Found{}`.

When the work can fail, use `attempt`. A throw becomes a message instead of
a crash:

```cpp
struct Failed { std::string why; };

return Cmd::attempt<Found, Failed>(&run_search, m.query);
// throws std::runtime_error("offline") -> Failed{"offline"}
```

**The one rule for background bodies.** A body runs on another thread,
possibly after the code that started it is gone. So:

- **it captures nothing.** `[&]`, `[this]`, `[m]` don't compile. Pass what
  it needs as arguments after the body; they're moved in.
- **every argument is Sendable**: it owns its data.
- it can be a lambda, a generic lambda, or a plain function
- parameters can be `T` or `const T&`
- it can take a `std::stop_token` first, if it wants to notice a cancel

```cpp
Cmd::perform<Found>([](std::stop_token st, const std::string& q) {
    std::vector<std::string> out;
    for (auto& row : rows_matching(q)) {
        if (st.stop_requested()) break;      // cancelled: stop early
        out.push_back(row);
    }
    return out;
}, m.query);
```

**More than one result.** `Cmd::task` gives the body a `Sink`, which it can
send through as often as it likes (progress, partial results):

```cpp
Cmd::task([](jaal::Sink<Msg> out, std::string q) {
    for (int pct = 0; pct <= 100; pct += 25)
        out.send(Progress{pct});
}, m.query);
```

A Sink never keeps the program alive. Sending after the program ended just
returns `false`.

## 6. Latest wins: keys

The search box has a bug now. Type `j`, `ja`, `jaa`: three searches start,
and they finish in any order. If the search for `j` finishes last, it
overwrites the results for `jaa`.

Give the work a **key**. Starting a task with a key that's already running
replaces the old one:

```cpp
static Cmd update(Model& m, Typed t) {
    m.query += t.c;
    return Cmd::perform<Found>("search", &run_search, m.query);
}
```

When a keyed task is replaced:

1. the old task's `stop_token` fires, so it can stop early
2. **nothing it sends reaches `update`**, including a result it had already
   finished and queued

So `Found` always belongs to the current query. There's no counter or token
to check by hand, and the decision is made on the loop thread, so thread
timing can't let a stale result through.

Stop it outright with `cancel`:

```cpp
static Cmd update(Model& m, Esc) {
    m.searching = false;
    return Cmd::cancel("search");
}
```

An **unkeyed** task can't be stopped: its stop_token only fires at
shutdown. If anything might need to stop a task, give it a key.

A key can also choose where the work runs. `jaal::fx::isolated` gives it a
thread of its own, for work that might hang (a stuck network call can't
block the shared pool):

```cpp
Cmd::perform<Found>(jaal::key("search", jaal::fx::isolated), &run_search, m.query);
```

## 7. Timers and debouncing

Searching on every keystroke is wasteful. Better: wait until the user stops
typing for 200 ms, then search once.

A keyed `after` does exactly that. Each new one with the same key replaces
the pending timer:

```cpp
struct Settled {};

static Cmd update(Model& m, Typed t) {
    m.query += t.c;
    return Cmd::after("settle", 200ms, Settled{});    // restarts on every key
}
static Cmd update(Model& m, Settled) {
    m.searching = true;
    return Cmd::perform<Found>("search", &run_search, m.query);
}
```

Type `jaal` quickly and `Settled` arrives once, 200 ms after the last `l`.
Tasks and timers share keys, so `Cmd::cancel("settle")` stops a pending
one.

A plain `Cmd::after(ms, msg)` with no key is a one-shot timer that always
fires.

## 8. Input

Keys, clicks and signals come from the host as **events**: plain structs
the host defines. A program turns them into messages with `on` overloads,
one per kind of event, the same idea as `update`:

```cpp
struct Key { char c; };                     // what the host sends

static std::optional<Msg> on(const Model& m, const Key& k) {
    if (k.c == '\x1b') return Esc{};
    if (m.searching && k.c == '\n') return std::nullopt;   // ignore Enter while busy
    return Typed{k.c};
}
```

- `on` sees the model **as it is when the event arrives**. If one key opens
  a picker, the next key in the same burst already sees the picker open.
- Return a message, or `std::nullopt` to ignore the event.
- An event kind with no `on` overload is ignored.

**Signals are input too.** Ctrl+C arrives as `jaal::sig`:

```cpp
static std::optional<Msg> on(const Model&, jaal::sig s) {
    if (s == jaal::sig::interrupt) return Quit{};
    return std::nullopt;
}
```

A program that doesn't handle Ctrl+C still stops on it (exit code 130), so
you can't make an app that can't be killed by forgetting this.

**Say what input you take.** Optionally, list the event kinds:

```cpp
using Event = std::variant<Key, jaal::sig>;
```

Then a kind with no `on` overload is a compile error instead of a silently
ignored event, and the test hosts know what you take: `headless<Search>`
accepts `Key` events with no extra template argument.

## 9. Subscriptions: things that keep running

Some work isn't one request; it runs as long as the app is in some state. A
clock ticking while a screen is open. A log tail while a pane is visible. A
download while `downloading` is true.

For those, describe what should be running **as a function of the model**:

```cpp
using Sub = jaal::Sub<Msg>;

static Sub subscribe(const Model& m) {
    if (!m.live) return Sub::none();
    return Sub::batch(
        Sub::every(1s, Tick{}),
        Sub::stream("feed", [](jaal::Sink<Msg> out, std::stop_token st, std::string q) {
            while (!st.stop_requested()) out.send(Update{poll(q)});
        }, m.query));
}
```

jaal runs `subscribe` after each change and compares it with what's
running, by key:

- same key as before: **kept** (a timer keeps its rhythm)
- key gone: **stopped** (the stream's stop_token fires, and its late
  messages are dropped)
- new key: **started**

You never write "start" or "stop". You say what should be running now.

**Task, or stream?** A task is one request ("search for this"). A stream is
a state ("while live, keep polling"). If you find yourself restarting a
task every time it ends, it wants to be a stream.

**Speed.** `subscribe` runs after every message that changes the model. If
it's expensive, add `subs_key`, returning the fields `subscribe` reads, and
jaal skips `subscribe` when they haven't changed:

```cpp
static auto subs_key(const Model& m) { return std::tuple{m.live, m.query}; }
```

Debug builds check that the key covers everything `subscribe` reads.

## 10. Splitting an app into parts

A real app has parts with their own state: an editor, a sidebar, tabs. Each
part is a program of its own, and the parent embeds it.

**One part in a fixed place** is a `child`:

```cpp
struct App {
    struct ToSearch { Search::Msg msg; };              // wraps the child's messages
    using Msg = std::variant<ToSearch /* , ... */>;
    using Cmd = jaal::Cmd<Msg>;
    using Box = jaal::child<Search, App, ToSearch>;

    struct Model { Search::Model search; };

    static Cmd update(Model& m, ToSearch w) { return Box::update(m.search, w); }
    static std::optional<Msg> on(const Model& m, const Key& k) {
        return Box::on(m.search, k);                   // input goes to the child
    }
};
```

The child's effects, subscriptions and input are wrapped into the parent's
messages for you. Its keys live under the child's name, so a child's
`"search"` can't replace the parent's.

**A list of parts** (tabs, sessions, rows that run) is `children`, keyed by
an id:

```cpp
struct ToTab { int id; Search::Msg msg; };
using Tabs = jaal::children<Search, App, ToTab>;

struct Model { Tabs::map tabs; int focused = 0; };

static Cmd update(Model& m, ToTab w)     { return Tabs::update(m.tabs, w); }
static Cmd update(Model& m, NewTab)      { return Tabs::add(m.tabs).second; }
static Cmd update(Model& m, CloseTab c)  { Tabs::remove(m.tabs, c.id); return {}; }
static std::optional<Msg> on(const Model& m, const Key& k) {
    return Tabs::on(m.tabs, m.focused, k);             // keys go to the focused tab
}
```

Removing a tab stops its timers and streams. Two tabs both searching run
two separate searches, because each child's keys live under its id.

## 11. Testing

Because everything is messages and values, testing is just driving the
program and looking at it. Three tools, from smallest to largest.

**Call `update` directly.** It's a function:

```cpp
Counter::Model m;
Counter::update(m, Counter::Inc{});
assert(m.n == 1);
```

**`given`: a reducer and its effects, no threads.** It runs `perform` and
`task` bodies inline, so a test sees their results without waiting:

```cpp
jaal::given<Search> t;
t.when(Search::Typed{'j'});
t.expect_effect<jaal::fx::after>(1);               // asked for the settle timer
t.when(Search::Settled{});
t.settle();                                        // runs the search inline
t.expect("results shown", [](auto& m) { return !m.results.empty(); });
assert(t.ok());
```

**`headless`: the real kernel, with a clock you move.** Timers fire when
you advance time; nothing waits for real:

```cpp
jaal::headless<Search> h;
for (char c : std::string("jaal")) { h.send(Search::Typed{c}); h.advance(50ms); }
h.advance(200ms);                  // typing settled: one search starts
h.run_until_idle();                // let the worker finish
assert(h.model().results.size() > 0);
```

`h.event(Key{'j'})` sends input through your `on` overloads.

**`sim` and `explore`: the orders you didn't think of.** Background work
finishes in whatever order the threads happen to run. `sim` makes that
order come from a seed, re-checks your invariants after every message, and
`explore` tries many seeds:

```cpp
auto r = jaal::explore<Search>(1, 500, {}, [](jaal::sim<Search>& s) {
    int t = 0;
    for (char c : std::string("jaal")) s.input(std::chrono::milliseconds(t += 30), Key{c});
    s.check("results match the query", [](const Search::Model& m) {
        return m.results.empty() || m.results.front().starts_with(m.query);
    });
});
if (!r.ok()) puts(r.failure->describe().c_str());   // the seed, the step, the reason
```

A failing seed fails the same way every time, on every platform, so it
becomes a regular test. A run takes about a microsecond.

`sim_options` can also crash or lose background tasks at a chosen rate, to
test the paths where work never comes back.

## 12. Running it for real

`jaal::run<App>()` runs a program with no host: timers, tasks, signals,
until it quits. With a host (a terminal UI, a server):

```cpp
int main() {
    my_host host;
    return jaal::run<App>(host);
}
```

`run` returns the program's exit code. Options:

```cpp
jaal::run_options o;
o.kernel.max_workers = 8;                         // pool size
o.on_seed = [](std::uint64_t s) { log("seed %llu", s); };   // replay a crash
return jaal::run<App>(host, o);
```

**When update throws.** By default the program stops with an error. With
`o.kernel.on_fault = jaal::fault_policy::skip`, the message that threw is
dropped and the model is left as it was before it.

Writing a host (turning a terminal or a socket into events, drawing the
model) is its own guide: [hosts.md](hosts.md).

## 13. When the compiler says no

jaal turns most mistakes into compile errors that say what's wrong in a
sentence. The common ones:

| the error says | what to do |
|---|---|
| `has no update for message 'X'; add: ...` | add the overload it names |
| `a task body must not capture anything` | move the capture to an argument after the body |
| `a task argument is not Sendable` | pass an owning type (`std::string`, not `std::string_view` or a pointer) |
| `a perform body must be callable as ...` | the arguments after the body don't match its parameters |
| `perform<Ok>: can't build the message Ok{result}` | give `Ok` one field of the body's return type |
| `P::Event lists a kind of input with no on() for it` | add the `on` overload, or remove it from `Event` |
| `P::on is declared, but no overload takes an event this host sends` | the parameter type isn't one the host sends |
| `this effect is not in the Cmd's row` | add the effect to `Cmd<Msg, ...>` |
| `P::init is declared but its signature doesn't match` | the message shows the shape to use |

[reference.md](reference.md#diagnostics) has the full list.

## 14. Rules of thumb

- **Put state in the model, and only there.** If it's not in the model, a
  test can't see it and a replay can't restore it.
- **Keep update boring.** Change fields, return effects. The interesting
  work happens in bodies and hosts.
- **Key anything that might be stale or stopped.** Searches, loads,
  debounce timers. It costs one string.
- **A task is a request; a stream is a state.**
- **Let `on` read the model.** Don't keep "which pane has focus" anywhere
  but the model.
- **Test with `given` first, `sim` when threads are involved.** If a bug
  depends on timing, `explore` will find a seed for it, and that seed
  becomes your regression test.
