# jaal — reference

Every public type, what it's for, and what it refuses.

The other docs answer different questions: [design.md](design.md) is how jaal
is built, [decisions.md](decisions.md) is why it's built that way,
[concurrency.md](concurrency.md) is the safety model. **This one is what you
look things up in.**

Each entry says what the thing does, the shape you write, and — the part
worth reading — **what it won't let you write**. jaal's guarantees are
mostly refusals, and a refusal you don't know about is just a confusing
compiler error.

---

## Contents

- [The program](#the-program) — `Program`, `Model`, `Msg`, `update`, `subscribe`
- [Effects: `Cmd`](#effects-cmd) — `none` `send` `after` `quit` `now` `random` `task` `task_isolated` `batch` `map`
- [Subscriptions: `Sub`](#subscriptions-sub) — `none` `every` `stream` `on` `batch`
- [Talking back: `Sink`](#talking-back-sink)
- [Composition](#composition) — `child`, `children`, `debounce`
- [Safety types](#safety-types) — `Sendable`, `Frozen`, `shared`, `Sync`, `co_owned`, `guarded`, `loop_bound`, `scope`, `handle`, `stop_group`, `worker_group`, `native_process`
- [Hosts](#hosts) — what a host is, `headless`, `given`, `sim`
- [Testing](#testing) — `sim`, `explore`, compile-fail cases
- [Diagnostics](#diagnostics) — reading jaal's error messages
- [Cheat sheet](#cheat-sheet)

---

## The program

A jaal program is one struct. There is no base class, no registration, no
framework object to construct:

```cpp
struct Counter {
    struct Model { int n = 0; };

    struct Inc {};
    struct Reset {};
    using Msg = std::variant<Inc, Reset>;

    using Cmd = jaal::Cmd<Msg>;          // + your effects: Cmd<Msg, beep>
    using Sub = jaal::Sub<Msg>;          // optional

    static Cmd init(Model& m);           // optional
    static Cmd update(Model& m, Inc);    // one per Msg case
    static Cmd update(Model& m, Reset);
    static Sub subscribe(const Model& m);// optional
};
```

### `Model`

Your state. Any type. jaal never copies it behind your back — `update` takes
it by reference and mutates in place.

### `Msg`

A `std::variant` of everything that can happen. jaal dispatches each
alternative to the `update` overload that takes it, so **you never write a
`std::visit`**.

`Msg` may be a variant OF VARIANTS. That matters at scale: a flat variant
costs `sizeof(Msg) == sizeof(heaviest leaf)` everywhere, so one fat message
makes every message fat. Grouping by domain keeps it small.

> **Refuses:** a `Msg` that isn't `Sendable`. It crosses to workers, so it
> must be safe to hand over. See [`Sendable`](#sendable).

### `update`

```cpp
static Cmd update(Model& m, SomeLeaf leaf);
```

Mutate the model, return a description of what should happen next. The
signature is the whole contract: a reducer that returns its model can't also
describe its effects as values, which is how apps end up reaching for a
global seam instead.

One generic overload can catch a family:

```cpp
template <class M> static Cmd update(Model&, M);
```

> **Refuses:** a `Msg` case with no `update`. The error names the case and
> the line to add it — not a silently-ignored message.

### `subscribe`

```cpp
static Sub subscribe(const Model& m);
```

What the program wants to hear about *right now*, as a function of state.
The kernel diffs it against what's running and starts/stops to match. You
never call "unsubscribe"; you stop returning it.

### Optional hooks

| Hook | Shape | Without it |
|---|---|---|
| `init` | `Cmd init(Model&)` | model is value-initialised |
| `subscribe` | `Sub subscribe(const Model&)` | no events at all |
| `subs_key` | `K subs_key(const Model&)`, `K` equality-comparable | `subscribe()` re-runs after **every** message |
| `visual_hash` | `uint64_t visual_hash(const Model&)` | host redraws every wakeup |
| `needs_warmup` | `bool needs_warmup(const Model&)` | no off-screen warm pass |

A hook with the wrong signature doesn't compile. `check_hooks<P>()` runs at
`kernel::start`, and a hook that's declared but can't be called fails with
the shape it should have:

```
jaal: P::init is declared but its signature doesn't match.
Expected: static Cmd init(Model&).
```

So `std::pair<Model, Cmd> init()` left over from an older runtime can't
silently throw the model away, which it once did.

### `subs_key` in detail

`subscribe()` runs after every message unless you say otherwise. For a big
model that walks lists and snapshots panes, that's real work on the input
path.

```cpp
static auto subs_key(const Model& m) {
    return std::tuple{m.panel_kind, m.streaming, m.composer.empty()};
}
```

A **value**, not a hash: a hash can collide, and a collision keeps a stale
subscription alive — a timer that should have stopped. Equality can't be
wrong that way.

**The rule that keeps it honest:** the key must cover everything
`subscribe()` reads *and everything its routers capture*. That second half
is the one that bites. A router may capture freely:

```cpp
Sub::on(on_key{}, [text = m.composer](const Key& k) { ... })
```

That holds a **copy** of `m.composer`. Skip the rebuild and the copy goes
stale — the next key routes against old text. So a field a router captures
belongs in the key even if `subscribe()` never mentions it.

Debug builds check the source half for you (`subs_key` says unchanged,
subscribe() disagrees → fault). **They cannot check captures.** Test those.

---

## Effects: `Cmd`

A `Cmd` is a **value describing side effects**. Returning one doesn't do
anything; the kernel interprets it after `update` returns.

```cpp
using Cmd = jaal::Cmd<Msg, my_effect, another>;
```

The row — the effect list in the type — is what a host must be able to run.
An effect not in the row won't convert, and a host that can't run one in the
row fails to compile with the effect **named**.

### Core effects

| Factory | Does |
|---|---|
| `Cmd::none()` | nothing (also `{}`) |
| `Cmd::send(Msg)` | fold this message next, in the same step |
| `Cmd::after(ms, Msg)` | fold it later |
| `Cmd::quit(int = 0)` | end the program with this exit code |
| `Cmd::now(f)` | `f(now)` → Msg; the clock as an effect, so tests control it |
| `Cmd::random(f)` | `f(rng)` → Msg; seeded, so a test replays exactly |
| `Cmd::perform<Ok>(body, args...)` | run `body(args...)` off the loop; its result arrives as `Ok{result}` |
| `Cmd::attempt<Ok, Fail>(body, args...)` | same, and a throw arrives as `Fail{...}` |
| `Cmd::task(body, args...)` | run on the pool; the body sends what it likes through a Sink |
| `Cmd::task_isolated(body, args...)` | run on its own thread |
| `Cmd::cancel(key)` | stop the keyed task with this key |
| `Cmd::batch(a, b, ...)` | several, in order |

Every way to run work takes an optional key first: `Cmd::perform<Ok>("search",
body, q)`. A keyed task **replaces** the running one with the same key. See
[keyed tasks](#keyed-tasks-latest-wins-and-cancel).

#### `Cmd::perform` and `Cmd::attempt`

Most background work computes one thing and hands it back. `perform` is
that, with no Sink and no `send`:

```cpp
struct Loaded { std::string body; };
struct LoadFailed { std::string why; };

static Cmd update(Model& m, Open) {
    return Cmd::attempt<Loaded, LoadFailed>([](const std::string& url) {
        return fetch(url);                    // returns std::string, may throw
    }, m.url);
}
static Cmd update(Model& m, Loaded l)     { m.body = std::move(l.body); return {}; }
static Cmd update(Model& m, LoadFailed f) { m.error = std::move(f.why); return {}; }
```

- `perform<Ok>`: the body's return value arrives as `Ok{value}`. A `void`
  body arrives as `Ok{}`. A throw is a task fault, like any task.
- `attempt<Ok, Fail>`: a throw arrives as a message instead. `Fail` is built
  from whatever it can hold: `Fail{e.what()}` for a string field, `Fail{ptr}`
  for a `std::exception_ptr`, or `Fail{}`.
- The body may take a `std::stop_token` first, when it wants to notice a
  cancel: `[](std::stop_token st, std::string url) { ... }`.
- Parameters can be `const T&`, and the body can be a generic lambda.

> **Refuses**, each with its own message: a body that captures, an argument
> that isn't `Sendable`, a body that can't be called with the arguments
> given, an `Ok` that can't hold the result, and a `Fail` that can't be
> built from an exception.

#### Keyed tasks: latest wins, and cancel

```cpp
static Cmd update(Model& m, Typed t) {
    m.query = t.text;
    return Cmd::perform<Results>("search", &search, m.query);
}
static Cmd update(Model&, Esc) { return Cmd::cancel("search"); }
```

Starting a task with a key that's already running:

1. asks the old one to stop (its `stop_token` fires)
2. drops **everything** it sends from then on, including messages it had
   already queued before the replace

So `update` never sees a result for a query that isn't current, and there
is no token to check by hand. `Cmd::cancel(key)` does steps 1 and 2 without
starting anything; cancelling a key with nothing running is a no-op.

The dropping is decided on the loop thread, so no thread timing can let a
stale result through. `tests/kernel/keyed_task_test.cpp` runs the
search-as-you-type shape across 300 `sim` seeds to check it.

Keys are per program: a `child` or `children` entry gets its own key space,
so two tabs both running `"search"` are two tasks.

A key can carry a placement: `jaal::key("scan", jaal::fx::isolated)`.

#### `Cmd::task`

For work that sends more than one message (progress, partial results), or
none:

```cpp
return Cmd::task(
    [](jaal::Sink<Msg> out, std::stop_token st, std::string url) {
        auto body = fetch(url, st);
        out.send(Loaded{std::move(body)});
    },
    m.url);                       // arguments, BY VALUE
```

> **Refuses:**
> - **a body that captures anything.** Everything it needs is an argument.
>   A capture can outlive the call that made it; an argument is copied.
> - **an argument that isn't `Sendable`.** It crosses threads.
>
> Both fail with a named diagnostic, not a template dump.

**`task_isolated`** is identical but gets its own thread, for work that
blocks for a long time and shouldn't occupy a pool slot.

#### The stop_token

A task's `std::stop_token` fires when:

- a keyed task is replaced or cancelled
- the program shuts down

An **unkeyed** task is fire-and-forget: only shutdown stops it. Give it a
key if anything should be able to stop it.

| You want | Use |
|---|---|
| run once, result comes back | `Cmd::perform` / `Cmd::attempt` |
| run once, the newest replaces the rest | the same, with a key |
| stop it from a later step | a key, then `Cmd::cancel(key)` |
| run while the model says so, stop when it doesn't | `Sub::stream` |
| several messages over time | `Cmd::task` (keyed, if it should be stoppable) |

#### `Cmd::batch` and ordering

Effects run **in the order given**, and interpretation **stops at the first
`quit`**. So this drops the save:

```cpp
return Cmd::batch(Cmd::quit(), save_settings(m));   // ✗ never runs
return Cmd::batch(save_settings(m), Cmd::quit());   // ✓
```

#### `Cmd::map`

Retarget a child's `Cmd` at the parent's `Msg`:

```cpp
child_cmd.map([](Child::Msg m) { return Parent::ToChild{id, m}; })
```

> **Refuses:** a capturing mapper when the Cmd holds a task — the mapper
> crosses threads with it.

### Your own effects

An effect is a payload plus a name:

```cpp
struct SaveFile { std::string path, contents; };
using save_file = jaal::pure_fx<SaveFile, "save_file">;

using Cmd = jaal::Cmd<Msg, save_file>;
```

The host runs it:

```cpp
void handle(SaveFile e) { write(e.path, e.contents); }
```

An effect may **answer** — return a `Msg` or `std::optional<Msg>` from
`handle` and it's folded in the same step. That's for work that must happen
on the loop thread and produce a result: handing the terminal to `$EDITOR`
and reporting how it exited.

---

## Subscriptions: `Sub`

What the program wants to hear about, as a function of state.

```cpp
static Sub subscribe(const Model& m) {
    if (!m.running) return Sub::none();
    return Sub::batch(
        Sub::every(std::chrono::milliseconds{16}, Tick{}),
        Sub::on(on_key{}, [](const KeyEvent& k) -> std::optional<Msg> {
            return k.ctrl && k.c == 'c' ? std::optional{Msg{Quit{}}}
                                        : std::nullopt;
        }));
}
```

Two kinds, with different lifetimes:

**Routers** are stateless filters over host input: "on a key, maybe produce
this Msg". Rebuilt every `subscribe()`, run on the loop thread. **They may
capture freely** — nothing they capture outlives the call. (But see
[`subs_key`](#subs_key-in-detail).)

**Sources** have a lifetime — started, kept, stopped — and a **key**. The
reconciler keeps a running source as long as a source with the same key keeps
being returned, so a timer keeps its phase across unrelated model changes.

### `Sub::every`

```cpp
Sub::every(std::chrono::milliseconds{16}, Tick{})
```

Keyed by interval + ordinal, so two timers at the same interval are two
timers.

### `Sub::stream`

Long-running work, as a subscription. **This is the answer to "how do I
cancel a task".**

```cpp
static Sub subscribe(const Model& m) {
    if (!m.downloading) return Sub::none();
    return Sub::stream("dl:" + m.url,                       // key
        [](Sink<Msg> out, std::stop_token st, std::string url) {
            for (auto chunk : fetch(url, st)) {
                if (st.stop_requested()) return;
                out.send(Progress{chunk.size()});
            }
            out.send(Done{});
        },
        m.url);
}
```

- Same key next `subscribe()` → **kept running**, not restarted.
- Key gone → **stop_token fires**. Stopping is *not asking for it*.
- Messages from a stopped generation are **dropped**, so a late result can't
  land in a model that moved on.

Same body rules as `task`: captureless, `Sendable` arguments.

### `Sub::on`

```cpp
using on_key = jaal::router<KeyEvent, "on_key">;
Sub::on(on_key{}, [](const KeyEvent& k) -> std::optional<Msg> { ... });
```

`nullopt` means "not mine" — the event falls through.

> **Refuses:** routing an event type the host doesn't produce. The error says
> so, rather than a router that silently never fires.

---

## Talking back: `Sink`

How a worker sends messages home.

```cpp
out.send(Msg{Loaded{...}});     // returns bool
```

A Sink is a **weak** handle to the loop's mailbox:

- The loop is gone → `send` returns `false`. It never writes into freed
  memory, so a worker outliving the loop is safe rather than fatal.
- It's `Sendable`, so it can be a task argument.
- **Only the kernel mints one.** The constructor is private.

A default-constructed Sink is **closed** — `send` returns false. That's
deliberate: a closed sink is harmless.

---

## Composition

### `child`

One child program in a fixed slot.

```cpp
struct ToEditor { Editor::Msg msg; };
using Ed = jaal::child<Editor, App, ToEditor>;

struct Model { Editor::Model editor; };
static Cmd update(Model& m, ToEditor t) { return Ed::update(m.editor, t); }
static Sub subscribe(const Model& m)    { return Ed::subscribe(m.editor, "editor"); }
```

The child's Cmds and Subs are retargeted at the parent's Msg automatically.
`subscribe` takes a **key prefix** — two children of the same type would
otherwise collide on their stream keys.

> **Refuses:** a `Wrap` that isn't a case of the parent's Msg, or a child
> whose Cmd row has an effect the parent's doesn't list.

### `children`

A **keyed list** of child programs — tabs, panes, sessions.

```cpp
struct ToTab { int id; Tab::Msg msg; };
using Tabs = jaal::children<Tab, App, ToTab>;

struct Model { Tabs::map tabs; };

static Cmd update(Model& m, ToTab t)     { return Tabs::update(m.tabs, t); }
static Cmd update(Model& m, NewTab)      { return Tabs::add(m.tabs).second; }
static Cmd update(Model& m, CloseTab c)  { Tabs::remove(m.tabs, c.id); return {}; }
static Sub subscribe(const Model& m)     { return Tabs::subscribe(m.tabs); }
```

Removing a child stops its subscriptions — that's what keying is for.

> **Use it when children RUN.** A list of rows in a picker is not a list of
> programs: if the entries have no Model of their own, no timers and no
> streams, `children<>` buys you an id-wrapped Msg layer and nothing else.

### `debounce<T>`

A value whose in-flight work is invalidated by **token**, so a late answer
to a superseded question is dropped without a hand-rolled staleness stamp.

It is a *value you put in the model*, not a Cmd factory:

```cpp
struct Model { jaal::debounce<std::string> query; };

static Cmd update(Model& m, Typed t) {
    const auto tok = m.query.set(t.text);          // invalidates older tokens
    return Cmd::after(200ms, Msg{RunSearch{tok}});
}

static Cmd update(Model& m, RunSearch r) {
    if (!m.query.ready(r.token)) return {};        // superseded while waiting
    return Cmd::task(search_body, m.query.value(), r.token);
}

static Cmd update(Model& m, Results res) {
    if (!m.query.ready(res.token)) return {};      // a late answer
    m.rows = std::move(res.rows);
    return {};
}
```

| Call | Means |
|---|---|
| `set(v)` | new value; returns the only token now `ready` |
| `invalidate()` | bump the token without changing the value — "everything in flight is stale" (Esc, panel closed) |
| `ready(t)` | is `t` from the most recent `set`? |
| `value()` / `token()` / `empty()` | the current value, token, and "nothing typed yet" |

Frozen- and Sendable-clean (a value and a counter), so it sits in any model
and rides along into a task.

There is also **`throttle`** in the same header for the other shape: let
something through at most once per interval, and remember whether anything
was dropped so the trailing edge still runs. That's for a progress bar fed
by a fast stream — redraw 30 times a second, but don't lose the final 100%.

---

## Safety types

The full reasoning is in [concurrency.md](concurrency.md); this is what each
one is and what it refuses.

### `Sendable`

"Safe to hand to another thread." Checked structurally, field by field.

Refuses raw pointers, references, views (`string_view`, `span`) and
`shared_ptr`-to-mutable — anything that may point at memory another thread
frees or mutates.

Opt in when you know better, **with the reason written down**:

```cpp
// This shared_ptr is one atomic<bool>, written by the UI thread and read by
// the worker — the narrowest possible conversation.
template <>
inline constexpr bool jaal::sendable_opt_in<std::shared_ptr<std::atomic_bool>> = true;
```

When something isn't Sendable and you can't see why:

```cpp
jaal::sendable_reason<MyType>();   // names the offending field
```

### `Frozen`

Deeply immutable. `shared<T>` requires it, so many readers need no lock.

### `shared<T>`

Immutable value, many readers, no lock. No null, no raw pointer in, no write
after construction.

### `Sync`

Many threads may use the same `T&` at once. Checked field by field on a
plain struct: a field is fine when it's `const` and Frozen, a `guarded`, a
`published`, an atomic number, or a `const` owning pointer to something
Sync. Anything writable fails. A class jaal can't walk opts in with
`sync_opt_in`.

### `co_owned<T>`

State a background job shares with its owner. Like `shared<T>` (never null,
move copies, built only by `make()`), but `make()` requires `Sync<T>` and it
hands out `T&`. `co_owned<T>::of(self)` gets the handle from inside an
object that derives from `enable_shared_from_this`.

> **Refuses:** a `T` with a writable field, naming the field.

### `guarded<T, R>`

State behind a lock you cannot forget to take, taken in an order the
compiler checks:

```cpp
using cache_rank = jaal::rank<40, "app.cache">;
guarded<std::map<std::string, int>, cache_rank> cache;     // R defaults to jaal::leaf

cache.with(cap, [](auto& m, std::string k) { ++m[k]; }, key);       // exclusive
int n = cache.read(cap, [](const auto& m) { return int(m.size()); });  // shared
```

`cap` is a **capability** (core/capability.hpp): `const jaal::unlocked&`
(nothing held), `const jaal::held<Q>&` (inside a lock ranked `Q`), or a
`const jaal::below<S>&` parameter. Taking a lock ranked `R` needs one that
proves every held lock ranks lower. A body may take `const jaal::held<R>&`
first to take further locks, which must rank above `R`:

```cpp
outer.with(cap, [](const jaal::held<outer_rank>& h, Outer& o) {
    inner.with(h, [](Inner& i) { ... });   // compiles only if inner ranks above outer
});
```

Threads get their root capability at entry: a job, task, stream body or
`scope` helper may take `const jaal::unlocked&` first, and `main` uses
`jaal::thread_root([&](const jaal::unlocked& cap) { ... })`. Blocking calls
(`wait_with`, `wait_with_for`, `scope`, `pool::shutdown`,
`worker_group::stop`) take exactly `const jaal::unlocked&`.

The arguments go **after** the lambda, not into a capture: inside
`with`/`read` you can only reach what you were *given*, and a capability is
never Sendable, so the body's own `held<R>` is the only one it has.

> **Refuses:** a lock taken out of rank order, two leaves together,
> re-entry, a wait under a lock, a missing or forged capability (each a
> compile error, `tests/compile_fail/capability.cpp`); returning something
> that points *into* the guarded value; a `T` that owns a pool or
> `worker_group` (its destructor would wait under the lock).

### `loop_bound<T>`

State only the loop thread may touch. Reaching the value needs a
`loop_token`, which can be **neither copied nor moved** (so it can't be
smuggled into a task body) and which cannot be forged: `loop_key`'s
constructor is private.

The kernel arms a per-thread identity for its whole lifetime, so on the loop
you just call `with()`:

```cpp
jaal::kernel::loop_bound<Cache> g_cache;        // a render cache, say

// In update() or view() — on the loop, so this works:
g_cache.with([](Cache& c) { c.put(key, value); });
auto hit = g_cache.with([&](Cache& c) { return c.get(key); });   // a COPY out
```

On a worker there is no token to be had, and the call **aborts** naming the
rule rather than quietly returning that thread's own empty copy — the silent
wrong answer is the bug this type exists to kill. Use `on_loop()` directly if
you want the token, or `on_loop()` with no argument to just ask.

`with()` won't let a reference or pointer into the state escape (these are
usually evicting caches, so the handle would dangle); return an owning value
— a copy, or a `shared_ptr<const T>`.

Use it instead of `thread_local`.

### `scope` / `nursery`

Structured concurrency: work that cannot outlive its scope. A handle is
pinned — no moving it out to extend its life.

### `handle`

An owned OS handle. No copy, no implicit conversion from `int`, no
double-close. `borrowed_handle` for a non-owning view, and the two don't
convert into each other.

### `stop_group`

Cancel and wait for work running on threads jaal doesn't own. `join()` hands
a member a real `std::stop_token`; `stop_and_wait(grace)` stops them all and
waits for them to leave. Refuses: a `join()` after stopping has begun.

### `worker_group`

An object's background jobs, where `stop()` is a hard barrier with no grace.
When it returns, no job is running, so the owner may free what the jobs
wrote into. Use it when a job touches something a third party frees right
after stop; `pool`'s bounded shutdown abandons, which is wrong there. Refuses:
a `post()` after `stop()` (dropped). A job that never returns hangs `stop()`,
so give jobs their own timeouts.

Jobs here and on `pool` follow the task rule: `post(body, args...)` with a
captureless `body(stop_token, args...)` and Sendable args. Shared state goes
in as a `co_owned<T>`.

> **Refuses:** a capturing body, a pointer or view argument.

### `native_process`

A child process, chosen per OS (`select.hpp`): `posix_process` or
`windows_process`. Both satisfy `Process` and pass the same conformance
suite. The exit handle and the output pipes go into the reactor like any
other readiness. On Windows the child runs in a kill-on-close job object, so
`stop(_, tree)` reaches every descendant; `graceful` acts as `forceful`
there, since an arbitrary program has no SIGTERM. Read Windows pipes with
`read_some`/`write_some`, which never wait. `process_spec::windows_command_line`
passes a command line verbatim, and a `cmd /c <payload>` argv passes its
payload as written, since cmd.exe doesn't parse by CommandLineToArgvW rules.

---

## Hosts

A host is what turns the outside world into events and carries out effects.
jaal ships no host — [hosts.md](hosts.md) is the guide to writing one.

Minimum: a `handle()` for every effect in the program's row. Optionally
`attach`/`on_ready` to watch file descriptors, `present` to draw, `wait_hint`
to sleep well.

```cpp
jaal::run<App>(host);
```

> ⚠️ **Template the context hooks.** If someone derives from your host to add
> effects, the context becomes `host_context<Derived>`. A hook fixed to
> `host_context<Base>` fails the `requires` test and is **silently skipped** —
> for `attach`, that means input is never registered and the app takes no
> keys. Take the context as a template parameter.

### `headless`

Run a program with no host at all. Effects are recorded; you drive with
messages.

```cpp
jaal::headless<App> h;
h.send(Msg{Inc{}});
CHECK(h.model().n == 1);
```

> **Note:** the recorder has no task executor, so pool tasks don't complete
> the way they do in a real run. If your assertion depends on a task landing,
> use `sim` or a real host.

### `given`

Runs tasks inline, deterministically. For a test that wants the task's result
without a thread.

### `sim`

The property-testing host — see below.

---

## Testing

### Reducers need no host

`update` is a pure function. Call it:

```cpp
Model m;
Cmd c = App::update(m, Inc{});
CHECK(m.n == 1);
```

And assert on the **effects**, because they're values:

```cpp
CHECK(count<save_settings>(c) == 1);
```

That's the payoff of effects-as-data. A reducer that *performs* its effects
can only be tested by installing a fake and inspecting it afterwards.

### `sim` — the interleavings you didn't think of

```cpp
jaal::sim<App> s{seed};

s.check("cursor is in range", [](const App::Model& m) {
    return m.cursor >= 0 && m.cursor <= (int)m.text.size();
});

s.at(0ms,  Msg{Type{'h'}});
s.at(10ms, Msg{Submit{}});
s.at(25ms, Msg{Cancel{}});

auto r = s.run();
CHECK(r.ok());
```

Simulated time, seeded task-completion order, **every invariant re-checked
after every message**. `sim_options` can inject crashed and lost tasks.

For a program whose `subscribe()` routes input events, name the event type:

```cpp
jaal::sim<App, maya::terminal_events> s{seed};
```

**`explore`** runs the same scenario across many seeds — each reorders task
results — and stops at the first break:

```cpp
auto out = jaal::explore<App>(1, 200, opt, [](auto& s) { ... });
CHECK(out.ok());
```

> **Make sure it isn't vacuous.** A sim that never steps passes everything.
> Add a deliberately false invariant once and watch it break; if it doesn't,
> your scenario isn't driving the machine.

### Invariants worth asserting

Structural ones — things true after *any* message, which is what makes them
safe against a random order:

- a derived flag agrees with the state it's derived from
- an index is in range
- two representations of one fact don't disagree

### Compile-fail tests

jaal's own guarantees are tested by code that **must not build**
(`tests/compile_fail/`, driven by `jaal_compile_fail`). Each case pins the
expected diagnostic, so it can't "pass" by failing for the wrong reason —
a typo, a missing include.

**A guarantee without a compile-fail test is a claim.** If you add a
refusal, add the case. And pair it with a positive test: a negative case can
pass because of a typo, and only the control catches that.

### The ban-list

`tests/lint/banlist.cmake` keeps raw `std::thread`, `std::mutex`,
`std::atomic`, `thread_local`, `const_cast`, `sink_access` and `loop_key` out
of code that should use the safe types. An allowlist names the files that may
use each, because there they *are* the implementation.

It scans a root you give it, so **point it at your own tree too**:

```cmake
add_test(NAME concurrency_banlist
         COMMAND ${CMAKE_COMMAND}
                 -DROOT=${CMAKE_SOURCE_DIR}/src
                 -DALLOW=${CMAKE_SOURCE_DIR}/tests/lint/allowlist.txt
                 -P .../jaal/tests/lint/banlist.cmake)
```

jaal running it over jaal says nothing about *your* code.

---

## Diagnostics

jaal's errors are written to be read. Most name the rule:

| Message | Means |
|---|---|
| `a task body must not capture anything` | pass it as an argument |
| `a task argument is not Sendable` | `sendable_reason<T>()` names the field |
| `a Sink's Msg must be Sendable` | your Msg holds a pointer or view |
| `effect ... not in this Cmd's row` | add it to `Cmd<Msg, ...>` |
| `host '...' cannot run effect '...'` | add `handle()` for it |
| `a subscription routes events this host doesn't produce` | host's `event_type` doesn't cover it |
| `guarded::with/read returns something that points into` | return a copy |
| `could not convert ... to 'jaal::below<...>'` | a lock taken out of rank order, or under one ranked at/above it: reorder, or give the outer lock a lower rank |
| `... to 'const jaal::unlocked&'` from a `held<...>` | a blocking wait/join/shutdown inside a lock body: move it out |
| `subs_key() reported no change, but subscribe() now returns different sources` | a field is missing from the key |
| `a perform body must not capture anything` | pass it as an argument after the body |
| `perform<Ok>: can't build the message Ok{result}` | give `Ok` one field of the body's result type |
| `attempt<Ok, Fail>: Fail must be a case of Msg built from ...` | give `Fail` a `std::string` field |
| `P::init is declared but its signature doesn't match` | the message gives the shape to use |

---

## Cheat sheet

```cpp
// ── program ──────────────────────────────────────────────────────────
struct App {
    struct Model { /* state */ };
    using Msg = std::variant</* leaves */>;
    using Cmd = jaal::Cmd<Msg /*, effects */>;
    using Sub = jaal::Sub<Msg /*, sources */>;

    static Cmd init(Model&);                    // optional
    static Cmd update(Model&, Leaf);            // one per leaf
    static Sub subscribe(const Model&);         // optional
    static auto subs_key(const Model&);         // optional, big win
};

// ── effects ──────────────────────────────────────────────────────────
Cmd::none()  Cmd::send(msg)  Cmd::after(ms, msg)  Cmd::quit(code)
Cmd::now(f)  Cmd::random(f)  Cmd::batch(a, b)     cmd.map(f)
Cmd::perform<Ok>([key,] body, args...)        // result -> Ok{result}
Cmd::attempt<Ok, Fail>([key,] body, args...)  // ... and a throw -> Fail{what}
Cmd::task([key,] body, args...)               // body sends through a Sink
Cmd::cancel(key)                              // stop a keyed task
// a key: the latest task with it wins, older ones stop and their messages drop

// ── subscriptions ────────────────────────────────────────────────────
Sub::none()  Sub::every(ms, msg)  Sub::batch(a, b)
Sub::on(on_key{}, filter)                  // router: may capture
Sub::stream(key, body, args...)            // source: STOPS when key goes

// ── safety ───────────────────────────────────────────────────────────
Sendable<T>  Frozen<T>  shared<T>  guarded<T, R>  loop_bound<T>  scope  handle
rank<N, "name">  leaf  unlocked  held<R>  below<R>  thread_root  JoinFree<T>

// ── testing ──────────────────────────────────────────────────────────
jaal::headless<App>            // no host, records effects
jaal::given<App>               // tasks run inline
jaal::sim<App, Event>          // simulated time, seeded order, invariants
jaal::explore<App, Event>(...) // the same, across seeds
```

### Two things that cost real debugging time

1. **An unkeyed task can't be stopped.** Its stop_token only fires at
   shutdown. If anything should stop it, give it a key.
2. **`subs_key` must cover what routers CAPTURE**, not just what
   `subscribe()` reads. The debug check can't see captures.
