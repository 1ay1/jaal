# jaal — design

jaal is a typed Elm runtime plus the platform layer it runs on. Nothing else.
No rendering, no input parsing, no batteries. maya becomes one host on top of
it; a GUI, a server or a test harness can be others.

*jaal* (जाल) means net or web: the thing that catches events from everywhere
and pulls them into one place.

## 0. Principles

1. **Contracts are types.** If a rule can be a concept, a type-state or a
   private constructor, it is. Breaking it should fail to compile, with a
   message that names the rule.
2. **Effects are data, and their set is in the type.** A `Cmd` says which
   effects it may contain. A host says which effects it can run. The compiler
   checks they match.
3. **The kernel never owns the thread.** It's a value you drive. jaal's own
   loop, maya, an actor scheduler, or someone's Qt app can all drive it.
4. **The platform is a set of capabilities, not an OS switch.** Each
   capability is a concept with several backends, including a simulated one.
   Every backend passes the same conformance tests.
5. **Determinism is a feature.** With the simulated platform, the same inputs
   and the same seed give the same run, every time.
6. **No hidden costs.** Plain `std::variant`s, no virtual dispatch on the hot
   path, no allocation per message in the common case.
7. **Say what C++ can't prove.** C++ can't enforce that `update` is pure, or
   that a resource is used exactly once. Where jaal relies on convention
   instead of proof, this document says so.

### Language level

- GCC and Clang: C++26. MSVC: C++23. Same as maya.
- C++26 features are used only behind feature-test macros, with a C++23 path:
  - pack indexing (`__cpp_pack_indexing`) in `meta/`
  - user-generated `static_assert` messages (`__cpp_static_assert >= 202306L`),
    so diagnostics can name the effect that's missing
- Static reflection (P2996) would remove the hand-written effect names.
  Not used until compilers ship it.

## 1. Layers

```
┌──────────────────────────────────────────────────────────────┐
│ apps           agentty, servers, tools                        │
├──────────────────────────────────────────────────────────────┤
│ hosts          maya::tui (in maya)   headless   later: gui    │
├──────────────────────────────────────────────────────────────┤
│ jaal::core     Program  Cmd<Msg,Row>  Sub<Msg,Row>  Sink      │
│ jaal::kernel   fold  interpreter  reconciler  timers  pool    │
│ jaal::platform reactor  waker  clock  signals  threads        │
│ jaal::meta     type lists, rows, diagnostics                  │
└──────────────────────────────────────────────────────────────┘
        Linux            macOS / BSD            Windows
```

Dependencies only point down. `meta` depends on nothing. `core` depends on
`meta`. `kernel` depends on `core` and `platform`. Hosts depend on `kernel`.

## 2. meta: the type-level toolkit

Everything type-theoretic in jaal is built on one small, tested module. It
has no other jaal dependency.

```cpp
namespace jaal::meta {

template <class... Ts> struct list {};

// queries
template <class T, class L> concept member_of = /* T appears in L */;
template <class A, class B> concept subset_of = /* every T in A is in B */;
template <class L>          concept unique    = /* no duplicates */;

// operations (all produce list<...>)
template <class... Ls> using concat_t     = /* ... */;
template <class L>     using dedup_t      = /* ... */;
template <class A, class B> using minus_t = /* A without B's members */;
template <class L, template <class> class F> using map_t    = list<F<Ts>...>;
template <class L, template <class> class P> using filter_t = /* ... */;

template <class T, class L> inline constexpr std::size_t index_of = /* ... */;
template <class L>          inline constexpr std::size_t size     = /* ... */;

}
```

Rules:

- Every operation has `static_assert` tests in `tests/meta/`. A test that
  compiles is a test that passed.
- Negative tests ("this must not compile") are small targets left out of
  the normal build. A ctest entry builds each one and expects it to fail,
  optionally with a specific diagnostic.
- No recursive instantiation anywhere: every operation is a fold or a single
  pack expansion, so long lists never hit the template depth limit.
- Cost: `size`, `at`, `member_of`, `index_of`, `concat`, `transform` and
  `filter` are linear. `unique`, `dedup`, `subset_of` and `minus` are
  quadratic in comparisons, which is inherent. That's fine for effect rows
  (10 to 30 entries) and would not be for hundreds of types.
- Type equality uses the `__is_same` builtin, not `std::is_same_v`. The
  variable template instantiates once per compared pair, and that made a
  600-type `unique` take 38 s on GCC 16.

## 3. core: the types apps see

### 3.1 Program

```cpp
template <class P>
concept Program =
    requires { typename P::Model; typename P::Msg; }
    && std::movable<typename P::Model>
    && Sendable<typename P::Msg>
    && (detail::has_init<P> || detail::has_init_cmd<P>)
    && requires(typename P::Model m, typename P::Msg msg) {
        { P::update(std::move(m), std::move(msg)) }
            -> detail::step_of<typename P::Model, typename P::Msg>;
    };
```

- `update` returns `std::pair<Model, Cmd<Msg, Row>>` for some row. The row
  is deduced from the return type: `fx_of<P>`. The program never has to name
  it separately.
- `view` and `subscribe` are optional hooks, found by concepts
  (`Viewable<P, Out>`, `Subscribing<P>`). A host that draws requires
  `Viewable<P, its output type>`. A headless host requires nothing.
- `Model` is passed and returned by value, and moved. That's the functional
  core: the new state is a value, not a mutation someone else can see.

What C++ can't prove: that `update` is pure. jaal can't stop it from calling
`printf`. The convention is documented, and the headless host makes
violations easy to spot (a test that records effects will miss one done by
hand).

### 3.2 Effects as descriptors

An effect family like "after a delay, send this Msg" depends on the app's
`Msg` type. C++ has no higher-kinded types, so jaal uses the standard trick:
**defunctionalisation**. Each effect is a plain descriptor type, with a
nested alias that applies it to a Msg:

```cpp
namespace jaal::fx {

struct after {
    static constexpr std::string_view name = "after";
    template <class Msg> struct type {
        std::chrono::milliseconds delay;
        Msg msg;
    };
    template <class F, class Msg>
    static auto fmap(F&& f, type<Msg> e) -> type<std::invoke_result_t<F, Msg>> {
        return {e.delay, std::invoke(f, std::move(e.msg))};
    }
};

}
```

The descriptor is an ordinary type, so rows are ordinary type lists and all
of `meta` works on them.

Effects that don't carry a Msg (clear the scrollback, set a title) use one
helper so they don't repeat themselves:

```cpp
template <class T, fixed_string Name>
struct pure_fx {                       // Msg-independent effect
    static constexpr std::string_view name = Name;
    template <class> using type = T;   // same payload for any Msg
    template <class F, class Msg> static T fmap(F&&, T e) { return e; }
};
```

The concept every descriptor must satisfy:

```cpp
template <class D>
concept Effect = requires {
    { D::name } -> std::convertible_to<std::string_view>;
    typename D::template type<probe_msg>;
} && requires(typename D::template type<probe_msg> e, probe_fn f) {
    { D::fmap(f, std::move(e)) }
        -> std::same_as<typename D::template type<probe_result>>;
};
```

`probe_msg`, `probe_fn` and `probe_result` are private test types. The
concept checks that `fmap` really changes the Msg type and nothing else. An
effect that carries a Msg and forgets `fmap` doesn't compile.

### 3.3 Rows

```cpp
template <Effect... Ds> requires meta::unique<meta::list<Ds...>>
struct row {
    using effects = meta::list<Ds...>;
};

template <class A, class B> concept subrow_of = meta::subset_of<...>;
template <class... Rs> using row_union_t = /* dedup(concat(...)) */;
```

The core row is the set of effects the kernel runs itself:

```cpp
using core_fx  = row<fx::quit, fx::after, fx::task, fx::now, fx::random>;
using core_src = row<fx::every, fx::stream>;   // sources the kernel runs itself
```

Hosts extend it. maya declares its terminal effects in maya:

```cpp
namespace maya::fx {
    using set_title          = jaal::pure_fx<SetTitle, "set_title">;
    using commit_scrollback  = jaal::pure_fx<CommitScrollback, "commit_scrollback">;
    // ...
}
using tui_fx = jaal::row_union_t<jaal::core_fx,
                   jaal::row<maya::fx::set_title, maya::fx::commit_scrollback, /*...*/>>;
```

This is what fixes today's layering smell, where the generic `Cmd` type
includes `render/scrollback_ledger.hpp`.

### 3.4 Cmd

```cpp
template <class Msg, class Row> class Cmd;

template <class Msg, Effect... Ds>
class Cmd<Msg, row<Ds...>> : public ctors_of<Ds, Cmd<Msg, row<Ds...>>>... {
public:
    struct None  {};
    struct Batch { std::vector<Cmd> cmds; };
    using variant = std::variant<None, Batch, typename Ds::template type<Msg>...>;
    variant inner;

    Cmd() = default;

    // any effect in the row converts implicitly
    template <class E> requires meta::member_of<E, meta::list<typename Ds::template type<Msg>...>>
    Cmd(E e);

    // row widening: a Cmd with fewer effects fits where more are allowed
    template <class R> requires subrow_of<R, row<Ds...>>
    Cmd(Cmd<Msg, R> narrower);

    static Cmd none();
    static Cmd batch(std::vector<Cmd>);
    template <class... Cs> static Cmd batch(Cs&&...);

    // functor map over the Msg
    template <std::invocable<Msg> F>
    auto map(F&& f) const& -> Cmd<std::invoke_result_t<F, Msg>, row<Ds...>>;
    template <std::invocable<Msg> F>
    auto map(F&& f) && -> Cmd<std::invoke_result_t<F, Msg>, row<Ds...>>;

    bool is_none() const noexcept;
};
```

Three design points:

**Row widening is subtyping.** A child component that only uses `after` has
type `Cmd<ChildMsg, row<fx::after>>`. After `map`, it converts into the
parent's `Cmd<Msg, tui_fx>` because `row<fx::after>` is a subrow. The
opposite direction doesn't compile. Components declare only the effects they
use, and the compiler checks the parent allows them.

**Constructor mixins keep the old API.** Each descriptor can provide
`ctors<Self>`, a small struct of static factories and nested type names:

```cpp
struct commit_scrollback_ctors<Self> {
    using CommitScrollback = maya::CommitScrollback;
    static Self commit_scrollback(int rows);
    static Self commit_scrollback(ScrollbackDebt);
};
```

`Cmd` inherits the mixins of every effect in its row. So
`maya::Cmd<Msg>::commit_scrollback(n)` and `Cmd::CommitScrollback` still
work, with **zero call-site changes in agentty**. And the factories only
exist on a `Cmd` whose row contains the effect: a headless-only `Cmd` has no
`commit_scrollback` at all. If two effects define the same name, that's an
ambiguity error, which is what you want.

**Functor laws.** `map(id) == id` and `map(f∘g) == map(f)∘map(g)`. Tested as
properties in `tests/core/`, with random Cmd trees.

maya keeps its spelling with an alias:

```cpp
namespace maya { template <class Msg> using Cmd = jaal::Cmd<Msg, tui_fx>; }
```

### 3.5 Sub

A subscription has two different kinds of source. The type keeps them apart.

- **Routers** are stateless filters over the host's event type:
  `on_key`, `on_mouse`, `on_paste`. They come from the host. Concept:

  ```cpp
  template <class D, class Event, class Msg>
  concept Router = requires(const typename D::template type<Msg>& r, const Event& ev) {
      { r.route(ev) } -> std::same_as<std::optional<Msg>>;
  };
  ```

- **Sources** have a lifetime: they're started, kept and stopped. `every` is
  the core one. A future `watch(path)` would be another. Concept:

  ```cpp
  template <class D, class Msg>
  concept Source = requires(const typename D::template type<Msg>& s) {
      typename D::key_type;
      requires std::regular<typename D::key_type>;
      requires hashable<typename D::key_type>;
      { s.key() } -> std::same_as<typename D::key_type>;
  };
  ```

`Sub<Msg, Row>` works like `Cmd`: descriptor rows, `map`, `batch`,
widening, constructor mixins (so `Sub<Msg>::on_key(...)` keeps working).

**The key space is a sum type.** The reconciler keys running sources by

```cpp
using any_key = std::variant<std::pair<Ds, typename Ds::key_type>...>;  // Ds = sources in the row
```

Two sources of different kinds can never have the same key, even if their
key values look equal. This generalises maya's `(interval, ordinal)` timer
key: `every`'s key type is exactly that pair.

The contract, per turn: when the model changes, `subscribe` runs, and the
kernel diffs the new key set against the running one:

- key in both: keep it running (timer phase kept), swap in the new Msg
- key only in new: start it
- key only in old: stop it
- the same key twice in one result: a reported error, never silent

### 3.6 Sink

```cpp
template <class Msg>
class Sink {
public:
    bool send(Msg) const;            // false once the kernel is gone
private:
    std::weak_ptr<detail::mailbox<Msg>> box_;
    friend class kernel_access;      // only the kernel mints sinks
};
```

- The only way work outside the loop can talk to the loop.
- Holds a weak reference. That's maya's fix for the queue-owns-task-owns-queue
  cycle (leak on shutdown, or a worker joining itself), made into the type.
  There's no way to get a strong reference to the mailbox from user code.
- Copyable and thread-safe. Sending after shutdown is a no-op that returns
  false.

### 3.7 Sendable

A Msg crosses threads, so it must not carry anything borrowed. `Sendable` is
deep: it looks inside aggregate structs field by field using C++26
structured binding packs, so a `string_view` hidden in a nested struct is
rejected. Full rules, the `Frozen` / `shared<T>` types for sharing
immutable data, and the opt-in for classes jaal can't look inside are in
[concurrency.md](concurrency.md) section 4.

### 3.8 Errors

```cpp
namespace jaal {
struct error {
    std::errc          code;     // portable
    std::int32_t       native;   // errno / GetLastError
    std::string_view   what;     // static string, no allocation
};
template <class T> using result = std::expected<T, error>;
}
```

No exceptions cross the platform boundary. Every platform call returns
`result<T>`. Failures that reach the app arrive as Msgs.

## 4. kernel: the loop as a value

Concurrency and memory safety for everything in this section (Sink, tasks,
the mailbox, `loop_bound`, `scope`, `guarded`) is designed in detail in
[concurrency.md](concurrency.md), starting from what maya does today.

### 4.1 Shape

```cpp
template <Program P, Platform Pl, class Row = fx_of<P>, class Src = src_of<P>>
class kernel {
public:
    using model_type = typename P::Model;
    using msg_type   = typename P::Msg;

    // the only constructor: runs init and the init Cmd. There is no
    // "constructed but not started" state to misuse.
    template <HostFor<P> H>
    static auto start(H& host, Pl& platform) -> result<kernel>;

    // feed one host event through the current subscriptions
    template <class Event> void route(const Event& ev);

    // one turn: drain bg messages, fire due timers, fold, run effects,
    // reconcile subs. Returns what the host should do next.
    template <HostFor<P> H> auto step(H& host) -> turn;

    auto next_deadline() const -> std::optional<typename Pl::clock::time_point>;
    auto waker() const -> typename Pl::waker::handle;
    auto sink() -> Sink<msg_type>;
    auto model() const -> const model_type&;

    // quitting is a move: a finished kernel can't be stepped again
    auto finish() && -> exit_status;
};

struct turn {
    bool model_changed;    // a view host should consider redrawing
    bool quit;             // stop driving; call finish()
};
```

The kernel is a template over `Platform` (section 6). The real platform and
the simulated one run the exact same kernel code.

### 4.2 The fold

maya's `drain_pending`, unchanged in behaviour:

```
while mailbox not empty:
    batch = take all
    for msg in batch:
        (model, cmd) = update(move(model), move(msg))
        subs_dirty = true
        interpret(cmd)
        if quit requested: drop the rest of the batch, return
```

- Messages from one source keep their order.
- A `quit` stops the batch. Effects of Msgs queued after it never run.
- Flood control: `step` folds at most `budget` Msgs (default 4096) before
  returning, so a Msg storm can't starve input and drawing. The next turn
  continues where this one stopped.

### 4.3 The interpreter

```cpp
template <class Msg, Effect D, class H>
concept handles = requires(H& h, typename D::template type<Msg> e, context<Msg>& cx) {
    h.handle(std::type_identity<D>{}, std::move(e), cx);
};
```

- Effects in `core_fx` are run by the kernel.
- Every other effect in the row is sent to `host.handle(tag, effect, cx)`.
- `HostFor<P>` requires `handles<Msg, D, H>` for every `D` in
  `minus_t<fx_of<P>, core_fx>`. If a host is missing one, the error names it:

  ```
  error: static assertion failed: jaal: host 'headless' cannot run effect
         'commit_scrollback' required by program 'agentty::App'
  ```

- Dispatch is `std::visit` over a closed variant. No virtual calls, no
  type erasure, no lookup table.

Two core effects exist only because the kernel owns the thing they read:

- `now(f)` reads the kernel's `Clock`, not `std::chrono`, so a test host
  controls it (`sim_clock`).
- `random(f)` draws from the kernel's `jaal::rng`, seeded from
  `options::random_seed` or, when that's 0, from the OS. The seed actually
  used is reported by `seed_used()`, and `run_options::on_seed` hands it to
  the program at start-up: log it, pass it back, and the run's draws repeat
  exactly. `sim` derives it from the sim seed, so ONE seed still controls
  the whole run; `headless` and `given` fix it, so tests are deterministic.
  Both draw synchronously on the loop thread, in the order the effects were
  returned.

### 4.4 Timers

- One 4-ary min-heap of deadlines, taken from `~/projects/tea`: cache-friendly,
  O(log n) insert, O(1) cancel through a stable handle.
- `after` goes in as one-shot. `every` sources are owned by the reconciler
  and keep their phase across model changes.
- Missed ticks: if the loop stalls for ten periods, an `every` fires **once**
  and re-arms from now. Catch-up storms are never what an app wants.
- Deadlines are computed with `saturate_add`, so `after(years)` can't
  overflow.
- When a deadline is turned into a wait timeout, it's rounded **up** (see
  6.4). Rounding down caused a hot spin in maya: the loop woke just before
  the timer was due, found nothing to do, and slept for 0 ms, over and over.

### 4.5 Worker pool and tasks

maya's `BackgroundQueue`, with cancellation added, and with captures
removed from task bodies.

```cpp
// in update: a captureless body, plus the values it needs, moved in
return fx::task<Msg>(path, [](Sink<Msg> out, std::stop_token st, std::string path) {
    out.send(Loaded{read_file(path, st)});
});
```

- The body must be captureless (checked by conversion to a function
  pointer), and every argument must be `Sendable`. So a task owns all its
  inputs and can't hold `this`, the model, a raw pointer or the mailbox.
  Why, and the one hole left (globals), are in concurrency.md 4.6.
- Internally the body and its arguments are stored together, type-erased
  once, in the effect. The erasure is jaal's, not the user's, so nothing
  unchecked goes in.
- Workers start lazily, up to `max(4, hardware_concurrency)`, and live for
  the process, so bursts reuse warm threads.
- Each task gets a `std::stop_token`. It's triggered when the kernel shuts
  down, or when the source that spawned it is removed from the subscription.
- `isolated_task` gets its own detached thread, so a hung syscall leaks one
  thread instead of blocking the pool. If the OS refuses a new thread, it
  falls back to the pool. Since the body owns all its inputs, a detached
  thread has nothing borrowed to outlive.
- Every thread body is wrapped so an exception can't reach
  `std::terminate`. It's reported as an error Msg instead (the approach of
  agentty's `isolated_thread`).
- A task sees only its `Sink` and its stop token, never the kernel.

### 4.6 Mailbox

- Multi-producer, single-consumer.
- `send` signals the waker only on the empty → non-empty transition. The
  consumer drains under the same lock. So wakes merge and none are lost.
- If the waker can't be created, the kernel drains every turn anyway.
  Slower, never lost.
- v1 uses a mutex and a vector, which is proven in maya. The lock-free
  Vyukov queue from `~/projects/tea` replaces it only if a benchmark says so.

### 4.7 Shutdown

`finish() &&` runs in a fixed order, each step a test:

1. stop all sources
2. request stop on every running task
3. drop the mailbox (sinks now return false)
4. join pool workers, with a deadline. Isolated threads are not joined.
5. return the exit status

## 5. Hosts

### 5.1 The concept

```cpp
template <class H, class P>
concept HostFor = Program<P> && requires(H& h) {
        typename H::event_type;                 // what its routers see
        typename H::fx;                         // effects it adds
        typename H::src;                        // sources and routers it adds
    }
    && subrow_of<fx_of<P>,  row_union_t<core_fx,  typename H::fx>>
    && subrow_of<src_of<P>, row_union_t<core_src, typename H::src>>
    && handles_all<H, typename P::Msg, minus_t<fx_of<P>, core_fx>>;
```

The payoff: **a program can't run on a host that can't carry out its
effects.** agentty returns terminal effects. A headless ACP host therefore
has to decide what `commit_scrollback` means for it, even if the answer is
"nothing". It can't be forgotten.

### 5.2 Driving protocol

The generic loop is a free function over the kernel:

```cpp
template <Program P, HostFor<P> H, Platform Pl = native_platform>
auto run(H host, Pl platform = {}) -> exit_status {
    auto k = kernel<P, Pl>::start(host, platform).value();
    for (;;) {
        auto wake = platform.reactor.wait(earliest(k.next_deadline(),
                                                   host.next_deadline()));
        host.pump(wake, k);                 // host's events → k.route(...)
        auto t = k.step(host);
        if (t.quit) return std::move(k).finish();
        host.present(k, t);                 // draw if it wants to
    }
}
```

Hosts own everything that is about output: when to draw, skipping unchanged
frames (`visual_hash`), animation frames, the nav-key frame split, writer
backpressure. The kernel doesn't know what a frame is.

### 5.3 Built-in host: headless

`jaal/host/headless.hpp`. The test host, and the base for server-style
programs with no screen.

- Runs on `sim_platform` by default.
- Records every non-core effect in order, for assertions.
- `inject(event)`, `advance(duration)`, `run_until_idle()`.
- A program only needs `update`. `view` is ignored if present.

### 5.4 Embedding

An app with its own loop (Qt, a game engine) drives the kernel directly:

- register `k.waker()` with its loop
- arm its own timer for `k.next_deadline()`
- call `k.step(host)` when either fires

That's the whole embedding story, and it's why the kernel never owns the
thread.

## 6. platform: the capability layer

### 6.1 Idea

Most cross-platform layers are one big `#ifdef` per function. jaal's platform
is a set of **capabilities**. Each capability is:

- a **concept** that says exactly what it guarantees
- several **backends**, one per OS mechanism, plus a simulated one
- a **conformance suite**: one templated test file that every backend must
  pass, including the simulation

A **platform** is a struct that bundles one backend per capability. The
kernel is written against the `Platform` concept, never against an OS.

```cpp
template <class Pl>
concept Platform = requires {
        typename Pl::clock;
        typename Pl::reactor;
        typename Pl::waker;
        typename Pl::signals;
        typename Pl::threads;
    }
    && Clock<typename Pl::clock>
    && Reactor<typename Pl::reactor>
    && Waker<typename Pl::waker, typename Pl::reactor>
    && SignalSource<typename Pl::signals, typename Pl::reactor>
    && ThreadSpawner<typename Pl::threads>;
```

```cpp
struct linux_platform {
    using clock   = steady_clock;
    using reactor = epoll_reactor;
    using waker   = eventfd_waker;
    using signals = signalfd_signals;
    using threads = std_threads;
};

struct darwin_platform  { /* kqueue_reactor, kqueue_user_waker (EVFILT_USER), kqueue_signals (EVFILT_SIGNAL) */ };
struct windows_platform { /* wait_reactor, event_waker, console_ctrl_signals */ };
struct poll_platform    { /* poll_reactor, pipe_waker, self_pipe_signals: any POSIX */ };
struct sim_platform     { /* sim_clock, sim_reactor, sim_waker, sim_signals, sim_threads */ };

using native_platform = /* picked in select.hpp */;
static_assert(Platform<native_platform>);
```

`native_platform` is picked at compile time. Nothing in the kernel or the
hosts ever names an OS.

### 6.2 Handles

OS handles are typed and owned. A socket fd, an event HANDLE and a pipe fd
are different types, even when they're all `int`.

```cpp
template <class Traits>
class unique_handle {                  // move-only, closes in the destructor
public:
    using native_type = typename Traits::native_type;
    explicit unique_handle(native_type) noexcept;
    auto get() const noexcept -> native_type;
    auto release() && noexcept -> native_type;     // consuming: can't release twice
};

struct fd_traits     { using native_type = int;    static constexpr int invalid = -1; static void close(int) noexcept; };
struct handle_traits { using native_type = void*;  static constexpr void* invalid = nullptr; static void close(void*) noexcept; };

using unique_fd     = unique_handle<fd_traits>;
using unique_handle_w = unique_handle<handle_traits>;
```

- A handle can only be closed by its owner, exactly once.
- `release() &&` consumes the owner, so a released handle can't be used again
  without the compiler seeing the move.
- Borrowed handles are a separate type, `handle_ref<Traits>`, which can't
  close.

### 6.3 Clock

```cpp
template <class C>
concept Clock = requires {
        typename C::time_point;
        typename C::duration;
    }
    && C::is_steady
    && requires(C& c) { { c.now() } -> std::same_as<typename C::time_point>; };
```

- `C::is_steady` must be true. `system_clock` doesn't satisfy `Clock`, so
  wall-clock time can't be used for timers by accident.
- `now()` is an instance call, not static. That's what lets `sim_clock` be
  per-simulation instead of a global.
- `sim_clock::advance(d)` moves time forward. Nothing else does.

### 6.4 Deadlines

```cpp
template <Clock C>
class deadline {
public:
    static deadline never();
    static deadline at(typename C::time_point);
    static deadline after(C&, typename C::duration);       // saturating

    // the only way to get a timeout for a syscall: always rounds UP
    template <class Unit>
    auto remaining_ceil(C&) const -> std::optional<Unit>;   // nullopt = never
};
```

The rounding rule lives in one function, on the type. No backend converts a
deadline to milliseconds by hand, so the hot-spin bug can't come back in one
backend.

### 6.5 Reactor

The reactor waits on handles until something is ready or a deadline passes.

```cpp
enum class interest : std::uint8_t { read = 1, write = 2 };

template <class R>
concept Reactor = requires(R& r, handle_ref<typename R::traits> h, interest i,
                           deadline<typename R::clock> d) {
    typename R::traits;
    typename R::clock;
    { r.watch(h, i) } -> std::same_as<result<registration<R>>>;
    { r.wait(d) }     -> std::same_as<result<ready_set<R>>>;
};
```

- `watch` returns a `registration`: a move-only RAII token. Dropping it stops
  watching. There's no "unwatch" call to forget, and no watching a handle
  twice by mistake.
- `wait` returns the set of registrations that became ready. It never
  returns early for `EINTR`; backends retry inside, with the remaining time
  recomputed from the deadline.
- Spurious wakeups are allowed by the concept and handled by callers. The sim
  reactor injects them on purpose so that stays true.

Backends:

| backend | OS | notes |
|---|---|---|
| `epoll_reactor` | Linux | edge cases for EPOLLHUP on ttys, as maya has today |
| `kqueue_reactor` | macOS, BSD | |
| `poll_reactor` | any POSIX | fallback, and a reference implementation |
| `wait_reactor` | Windows | `WaitForMultipleObjects` + console input, and the MSYS2 pipe case from maya |
| `sim_reactor` | all | driven by the test, deterministic |

v1 is readiness-based, because that's what maya's terminal input needs and
what's proven. Completion-based I/O (IOCP, io_uring) becomes a second
concept, `Proactor`, when networking arrives. It's a separate concept, not a
mode flag, so code can say which one it needs.

### 6.6 Waker

```cpp
template <class W, class R>
concept Waker = requires(W& w, const W& cw, R& r) {
    { W::create(r) } -> std::same_as<result<W>>;
    { cw.wake() }    noexcept;           // thread-safe, async-signal-safe
    { w.drain() }    noexcept;           // consumer side
};
```

- `wake()` is safe from any thread and from signal handlers.
- Several `wake()`s before a `drain()` count as one wakeup.
- Backends: `eventfd_waker` (Linux), `kqueue_user_waker` (macOS, EVFILT_USER),
  `pipe_waker` (any POSIX), `event_waker` (Windows), `sim_waker`.

### 6.7 Signals

```cpp
enum class signal : std::uint8_t { interrupt, terminate, hangup, resize, child, suspend };
using signal_set = enum_set<signal>;

template <class S, class R>
concept SignalSource = requires(S& s, R& r, signal_set set) {
    { S::install(r, set) } -> std::same_as<result<S>>;   // RAII: uninstalls on destroy
    { s.take() }           -> std::same_as<signal_set>;  // what arrived since last take
};
```

- The portable enum is the only thing callers see. `SIGWINCH` and
  `CTRL_C_EVENT` are backend details.
- Signals arrive as reactor events, never as code running inside a handler.
  The async-signal-safety problem is solved once, in the backend.
- Backends: `signalfd_signals` (Linux), `kqueue_signals` (macOS),
  `self_pipe_signals` (any POSIX), `console_ctrl_signals` (Windows), `sim_signals`.

### 6.8 Threads

```cpp
template <class T>
concept ThreadSpawner = requires(T& t, std::string_view name, move_only_function<void(std::stop_token)> f) {
    { t.spawn(name, std::move(f)) }    -> std::same_as<result<std::jthread>>;
    { t.spawn_detached(name, std::move(f)) } -> std::same_as<result<void>>;
    { t.hardware_concurrency() }       -> std::same_as<unsigned>;
};
```

- Threads are named (visible in `top`, debuggers and crash dumps).
- `result` instead of an exception when the OS refuses a thread (EAGAIN), so
  the pool can fall back.
- `sim_threads` runs "threads" as steps on one real thread, in an order
  chosen by a seed. That's what makes concurrent tests deterministic.

### 6.9 Restore guard

Apps that change OS state (raw terminal mode, alt screen) must restore it
even on a crash.

```cpp
class restore_guard {
public:
    template <std::invocable F> requires std::is_nothrow_invocable_v<F>
    static auto install(F f) -> result<restore_guard>;   // RAII, LIFO order
};
```

- Runs on normal exit, `std::terminate`, and fatal signals (SIGSEGV, SIGABRT)
  or Windows unhandled exceptions.
- The callback must be `noexcept`, and the docs say it must be
  async-signal-safe. (C++ can't check that part.)
- maya's terminal restore moves onto this.

### 6.10 Conformance and simulation

One test file, `tests/platform/conformance.cpp`, is a template over
`Platform`. It's instantiated for every backend that builds on the machine,
and always for `sim_platform`:

- a wake from another thread interrupts a wait
- many wakes before a wait count as one
- `wait` with a past deadline returns immediately; with `never` it blocks
- a deadline 0.3 ms away never produces a 0 ms timeout
- `EINTR` during a wait doesn't return early
- dropping a registration stops events for that handle
- signals arrive as reactor events, merged

The simulated platform adds a seed and fault injection:

- spurious wakeups
- delayed wakes
- thread interleavings chosen by the seed
- `EAGAIN` on thread spawn

A failing seed is printed, and re-running with it reproduces the exact run.

## 7. File structure

One rule decides where a file goes: **its path says what it depends on.**
A file under `core/` can only include `meta/` and `core/`. A file under
`platform/linux/` is the only place Linux headers appear. CI checks this with
an include-graph test (section 9).

```
jaal/
├── CMakeLists.txt
├── CMakePresets.json             dev, release, asan, tsan, sim, mingw-cross
├── design.md                     this file
├── concurrency.md                memory and concurrency safety, from maya's code up
├── README.md
├── LICENSE                       MIT
├── cmake/
│   ├── jaal-config.cmake.in
│   ├── warnings.cmake
│   └── compile_fail.cmake        negative-compile test harness
│
├── include/jaal/
│   ├── jaal.hpp                  umbrella: core + kernel + native platform
│   │
│   ├── meta/                     no dependencies at all
│   │   ├── list.hpp              list, member_of, subset_of, unique, index_of
│   │   ├── algo.hpp              concat, dedup, minus, map, filter
│   │   ├── fixed_string.hpp      NTTP strings for effect names
│   │   └── diagnose.hpp          named static_assert helpers
│   │
│   ├── core/                     the types apps write against
│   │   ├── program.hpp           Program, Viewable, Subscribing, fx_of, src_of
│   │   ├── effect.hpp            Effect concept, pure_fx
│   │   ├── row.hpp               row, subrow_of, row_union_t
│   │   ├── cmd.hpp               Cmd<Msg, Row>
│   │   ├── sub.hpp               Sub<Msg, Row>, Router, Source, any_key
│   │   ├── sink.hpp              Sink<Msg>
│   │   ├── sendable.hpp          deep Sendable (structured binding packs), opt-in
│   │   ├── frozen.hpp            Frozen, shared<T>
│   │   ├── error.hpp             error, result<T>
│   │   └── fx/                   the core effect and source descriptors
│   │       ├── quit.hpp
│   │       ├── after.hpp
│   │       ├── task.hpp          task, isolated_task
│   │       └── every.hpp
│   │
│   ├── kernel/
│   │   ├── kernel.hpp            kernel<P, Pl>
│   │   ├── fold.hpp              the Msg fold, budgets
│   │   ├── interpret.hpp         effect dispatch, handles, HostFor
│   │   ├── reconcile.hpp         keyed source diffing
│   │   ├── timer_heap.hpp        4-ary heap with stable handles
│   │   ├── mailbox.hpp           MPSC queue + waker protocol
│   │   ├── pool.hpp              worker pool, stop tokens
│   │   ├── loop.hpp              loop_token, loop_bound<T>
│   │   ├── scope.hpp             scope + nursery (structured concurrency)
│   │   ├── guarded.hpp           guarded<T>
│   │   └── run.hpp               the generic run<P>(host) loop
│   │
│   ├── platform/
│   │   ├── concepts.hpp          Platform, Clock, Reactor, Waker, SignalSource, ThreadSpawner
│   │   ├── handle.hpp            unique_handle, handle_ref, traits
│   │   ├── deadline.hpp          deadline<Clock>, ceil rounding
│   │   ├── signal.hpp            portable signal enum, signal_set
│   │   ├── restore_guard.hpp
│   │   ├── select.hpp            native_platform, the only #if chain
│   │   ├── linux/                epoll_reactor, eventfd_waker, signalfd_signals
│   │   ├── darwin/               kqueue_reactor, kqueue_user_waker, kqueue_signals
│   │   ├── posix/                poll_reactor, pipe_waker, self_pipe_signals, std_threads
│   │   ├── windows/              wait_reactor, event_waker, console_ctrl_signals
│   │   └── sim/                  sim_clock, sim_reactor, sim_waker, sim_signals, sim_threads
│   │
│   └── host/
│       ├── concepts.hpp          HostFor, handles_all
│       └── headless.hpp          recording host on sim_platform
│
├── src/
│   └── platform/                 one .cpp per backend; OS headers live only here
│       ├── linux/
│       ├── darwin/
│       ├── posix/
│       └── windows/
│
├── tests/
│   ├── meta/                     static_assert tests: compiling = passing
│   ├── core/                     Cmd/Sub laws, row widening, Sendable
│   ├── kernel/                   the carried-over maya bugs (section 9.2)
│   ├── platform/
│   │   ├── conformance.cpp       one template, run against every backend
│   │   └── sim_faults.cpp
│   ├── compile_fail/             programs that must NOT compile
│   └── layering/                 include-graph check
│
├── examples/
│   ├── counter.cpp               headless counter
│   ├── ticker.cpp                every + after, real clock
│   └── embed.cpp                 driving the kernel from someone else's loop
│
└── bench/
    ├── fold.cpp                  Msgs per second through update
    └── wake.cpp                  cross-thread wake latency
```

Notes:

- **Headers declare, `src/` implements, for platform code.** OS headers
  (`<sys/epoll.h>`, `<windows.h>`) are included only in `src/platform/`, so
  `windows.h` macros never leak into user code. The backend classes are
  declared in headers with no OS includes.
- **core and kernel are header-only**, because they're templates over the
  user's `Msg`.
- **`select.hpp` is the only file with an `#if` chain over OSes.**
- The library target is `jaal::jaal`. There is one optional extra target,
  `jaal::sim`, so production builds don't link simulation code.

## 8. Build

- CMake 3.28+, same as maya.
- Targets:
  - `jaal::jaal`: core, kernel, native platform, headless host
  - `jaal::sim`: the simulated platform (tests and users' own tests)
- Options:
  - `JAAL_BUILD_TESTS`, `JAAL_BUILD_EXAMPLES`, `JAAL_BUILD_BENCH`
  - `JAAL_PLATFORM=auto|linux|darwin|posix|windows`: force a backend, so the
    `poll_platform` can be tested on Linux
  - `JAAL_WERROR`
- Warnings: `-Wall -Wextra -Wpedantic -Wconversion -Wshadow` and `/W4`.
- Consumed by maya as a git submodule, with `find_package(jaal)` as the
  installed fallback.

## 9. Testing

### 9.1 Kinds of tests

| kind | where | what it proves |
|---|---|---|
| static | `tests/meta`, `tests/core` | type-level facts; if it compiles, it passed |
| compile-fail | `tests/compile_fail` | wrong programs are rejected, with the expected message |
| law | `tests/core` | functor laws for Cmd and Sub map, random trees |
| kernel | `tests/kernel` | behaviour, on the headless host and sim platform |
| conformance | `tests/platform` | every backend meets its concept's contract |
| simulation | `tests/platform/sim_faults.cpp` | seeded fault injection, reproducible |
| layering | `tests/layering` | no file includes a layer above it |
| sanitizer | CI presets | ASan, UBSan and TSan clean |

### 9.2 Carried-over maya bugs

Each of these was a real bug in maya. Each is a kernel test before maya
switches over:

1. `quit` in the middle of a batch: later Msgs' effects never run.
2. Events are routed again after every Msg, against the new subscriptions
   (the `^T m o` key sequence bug).
3. `subscribe` runs only when the model changed.
4. A timer keeps its phase across model changes.
5. Two `every`s with the same interval both fire (the old same-interval
   starvation).
6. `after` with a huge delay doesn't overflow.
7. A deadline under 1 ms away gives a 1 ms timeout, never 0.
8. A task holding a sink doesn't keep the kernel alive; shutdown with queued
   tasks neither leaks nor self-joins.
9. Background Msgs still arrive when the waker failed to create.

### 9.3 Compile-fail examples

```cpp
// test-only effect: something the headless host doesn't run
using beep = pure_fx<struct Beep{}, "beep">;

// a component using an effect its parent's row doesn't allow
Cmd<Msg, row<fx::after>> c = Cmd<Msg, row<fx::after, beep>>::none();   // must fail: widening only

// a program run on a host that can't run one of its effects
struct Beeper { /* update returns Cmd<Msg, row<beep>> */ };
jaal::run<Beeper>(jaal::headless{});                                     // must fail, naming 'beep'

// an effect that carries a Msg but has no fmap
struct bad { static constexpr std::string_view name = "bad";
             template <class Msg> struct type { Msg m; }; };
using r = row<bad>;                                                      // must fail: not an Effect

// system_clock as a timer clock
static_assert(Clock<std::chrono::system_clock>);                         // must fail: not steady
```

### 9.4 CI matrix

- Linux: GCC and Clang, x86-64 and aarch64
- macOS: Apple Clang
- Windows: MSVC, and llvm-mingw cross-compile (already used for agentty)
- Linux forcing `poll_platform`, so the generic POSIX backends stay honest
- sanitizers on Linux Clang

## 10. Diagnostics

Template-heavy libraries are only usable if their errors are readable.

- Every concept that users meet has a matching `static_assert` wrapper with
  a plain-English message (C++26 formatted messages, C++23 fixed text).
- Effect names come from `D::name`, so messages name effects, not mangled
  types.
- Checks happen at the outermost call (`run`, `kernel::start`), so the error
  points at the user's line, not deep inside jaal.

## 11. Performance targets

Measured in `bench/`, checked before each release:

| path | target |
|---|---|
| one Msg through the fold, trivial update | under 50 ns |
| cross-thread send to wake | under 10 µs |
| idle process | 0% CPU, no wakeups |
| `step` with no work | no allocation, no syscall |
| subscription reconcile with no model change | skipped entirely |

## 12. Migration: maya onto jaal

1. **jaal skeleton.** This document, CMake, CI, `meta/` with tests.
2. **core.** Effects, rows, `Cmd`, `Sub`, `Sink`, `Sendable`, with static,
   law and compile-fail tests.
3. **platform.** Port maya's `platform/` wakers, event sources and signals
   into capability backends, plus `sim/`. Conformance suite passing on
   Linux, and cross-compiling for Windows.
4. **kernel and headless host.** Carried-over bug tests passing.
5. **maya on jaal.**
   - add jaal as a submodule of maya
   - declare `maya::fx::*` descriptors and `tui_fx`, with constructor mixins
     matching today's API
   - `template <class Msg> using Cmd = jaal::Cmd<Msg, tui_fx>;`, same for
     `Sub`
   - `detail::Runtime` becomes the terminal host; `run<P>` calls
     `jaal::run`
   - maya's tests pass unchanged
6. **agentty.** Bump the maya submodule, build the binary, run focused
   tests. Because of the constructor mixins, agentty's source shouldn't need
   changes. If a few test files poke at `Cmd::Variant` internals, they get a
   one-line fix.
7. **Later.** agentty's ACP server mode moves onto the headless host, so
   there's one turn loop instead of two. Then actors, when a real program
   needs them.

## 13. Not in v1

- Actors, mailboxes per program, the work-stealing scheduler. The kernel
  design leaves room: `Sink<Msg>` becomes `Addr<Msg>`, and a scheduler
  drives many kernels.
- Proactor (IOCP, io_uring) and networking.
- Any battery. They're separate libraries that add effect descriptors, and
  the row system is what lets them do that without touching jaal.
- Hot reload, time-travel debugging.

## 14. Open questions

Answered since this was written (kept so the reasoning is visible):

1. ~~**Row order.**~~ Settled: `make_row` sorts by `D::name` and
   deduplicates, so rows are sets and spelling never creates a new type
   ([decisions.md](decisions.md) D2).
2. ~~**Where routers live.**~~ Settled: a router names its `event_type`; a
   host's event type may be a variant, and each router takes one
   alternative ([decisions.md](decisions.md) D22).
3. ~~**`Sendable` strictness.**~~ Settled: strict, with explicit per-type
   opt-ins. It found a real race in agentty on its first run
   ([decisions.md](decisions.md) D7).

Still open:

4. **Compile time.** agentty's `Msg` is a large variant, and every Cmd
   instantiation multiplies it by the row size. Measured so far: the deep
   `Sendable` check on agentty's real `Msg` costs about 4 s in one TU,
   mostly agentty's own headers. The full build-time cost of moving maya
   onto jaal still needs measuring.
