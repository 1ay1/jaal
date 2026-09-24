# jaal — concurrency and memory safety

This is the deep version of how jaal stops memory and concurrency bugs. It
starts from what maya does today, found by reading every thread, lock,
atomic and `thread_local` in maya's source, and builds up from there.

It's a companion to [DESIGN.md](DESIGN.md). Where the two disagree, this file
is newer and wins; DESIGN.md sections 3.6, 3.7, 4.5 and 4.6 point here.

## 1. What maya does today

### 1.1 The inventory

Counted across `maya/src` and `maya/include`:

| thing | count | where it lives |
|---|---|---|
| `thread_local` | 68 | caches, registries, scratch buffers, reactive scopes |
| `std::atomic` | 36 | counters, theme slot, markdown async flags, emergency state |
| `std::mutex` | 18 | BackgroundQueue, markdown async, signal install, theme projections |
| `std::thread` / `jthread` | 11 | BackgroundQueue workers, isolated tasks, markdown async |
| `.detach()` | 3 | isolated tasks, markdown async worker |
| `volatile sig_atomic_t` | 3 | resize pipe fd, emergency handler re-entry |

Only a few of these are real concurrency. They sort into seven patterns.

### 1.2 The seven patterns

**P1. The UI thread owns the world** (68 `thread_local`s).
maya's real concurrency model is "almost everything happens on one thread".
The component cache, layout scratch pools, the scroll-state registry, hit
regions, focus scope, reactive scopes (`Signal`), animation requests,
syntax-highlight cache and more are all `thread_local`. The comments say it
directly: *"cache is thread_local (renderer is single-threaded per app
instance)"*.

This works, and it's why maya has so few races. But the rule is **implicit**.
Nothing stops a worker from calling `render_tree` or touching a `Signal`,
and if it did, it would silently get a separate, empty copy of every cache.
That's not a crash, it's a wrong answer, which is worse.

**P2. Messages into the loop** (`BackgroundQueue`).
Workers send Msgs through a mutex-protected vector, and a wake fd rouses the
loop. The comment block on it records three real bugs and their fixes:

- a strong capture of the queue inside a task made a cycle, so the queue
  leaked at shutdown
- worse, the last reference was then held by a task, so a worker destroyed
  the queue and joined itself: `std::terminate`
- a worker could re-lock a mutex after the queue destroying it had freed it

The fix is `sink()`, which holds a `weak_ptr`, plus careful ordering in the
worker loop. It's correct now, and documented as *"READ THIS BEFORE ADDING
A CAPTURE"*. That warning is the problem: the safety depends on the next
person reading a comment.

**P3. Fire-and-forget threads** (`IsolatedTask`, markdown async).
`std::thread(...).detach()`. Chosen on purpose, so a hung syscall leaks one
thread instead of blocking the pool. But a detached thread can't be waited
for, and it outlives anything it borrowed. maya handles this by moving
everything the thread needs into a `shared_ptr` it owns. Correct, but by
convention.

**P4. Single-flight offload with a result slot** (markdown `set_content_async`).
The most intricate one:

- a `shared_ptr<AsyncSlot>` holds the source, an `atomic<bool> cancelled` and
  an `atomic<bool> ready`
- a detached worker parses, checks `cancelled` at safe points, then does a
  release-store on `ready`
- `build()` (UI thread) does an acquire-load, adopts the result, and
  `const_cast`s `this` to write it into the widget
- a newer request made while one is in flight is kept in
  `async_latest_source_` and started when the current one lands

It's hand-rolled message passing: an atomic flag standing in for a Msg, a
`const_cast` standing in for `update`. It works, but every piece of it
(orderings, the cancel checks, the const_cast) is something a reviewer has
to re-verify by hand.

**P5. Shared read-mostly state** (theme `live_slot`, projections).
`std::atomic<const Theme*>` with acquire/release, plus an epoch counter. This
is sound only because the pointed-to `Theme` is never freed while
readers might hold it. The theme commits on this branch (*"the live slot
borrowed, and trusted, and both were wrong"*) were exactly that bug: the slot
held a pointer to a `Theme` the caller could destroy.

**P6. Signal handlers** (resize pipe, emergency restore).
`volatile sig_atomic_t`, self-pipe, a `pthread_sigmask` bracket around
teardown, `SA_NODEFER` re-entry guard, an alt stack for stack overflows. All
carefully reasoned, all in comments, all platform-specific.

**P7. Relaxed counters** (component generation, pool ids, stats).
`fetch_add(relaxed)` for "a number nobody else has". Harmless and correct.

### 1.3 What that tells us

1. maya is mostly safe **because it's single-threaded**, not because the types
   make it safe. The one-thread rule should become a type, not a comment.
2. Every real cross-thread path (P2, P3, P4, P5) is **message passing done by
   hand**. Each one reinvented a mailbox, and each one had bugs that were
   later fixed and written up.
3. The recurring bug is **lifetime**: something on one thread holding a
   pointer to something another thread can free (P2 self-join, P5 theme
   slot). Races on plain data are rare. Lifetime across threads is the real
   enemy.
4. Signal handling is **its own world** and should stay walled off in the
   platform layer.

So jaal's job is: make the single-owner rule a type, make message passing the
only built-in way across threads, and make it impossible for a thread to
hold something it doesn't own.

## 2. What "impossible" can mean in C++

Being honest up front. C++ has no borrow checker and no `Send` trait. There
are three levels:

- **Level A, rejected by the compiler.** The wrong program doesn't build.
- **Level B, caught by tooling.** It builds, but CI fails: a lint, TSan, ASan.
- **Level C, convention.** Documented, reviewed, not checked.

The goal: **everything in jaal's API is Level A**. Getting around it
requires something visible (a raw pointer, `std::thread`, `const_cast`,
`reinterpret_cast`), and those are Level B through a ban-list check. Level C
is kept for the few things no tool can check (async-signal-safety of a
callback), and each one is listed in section 9.

## 3. The model

jaal has exactly three ways for code on different threads to relate, and no
fourth:

1. **Owned**: data belongs to one thread. Only that thread can touch it.
2. **Moved**: data is handed from one thread to another. The sender gives it
   up.
3. **Shared immutable**: data nobody can change is shared by reference count.

Mutable data shared between threads is **not** one of the three. When it
seems necessary, it becomes an owner (a program or service) that others send
messages to.

Everything below is how the types enforce those three.

## 4. Building blocks

### 4.1 `affine<T>`: may cross threads, by move, once

A plain `Sendable` check (DESIGN.md 3.7) says what a type is. It doesn't
stop two threads from using the same object. The ownership part comes from
how jaal APIs take their arguments:

- every cross-thread API takes `T&&` of a **non-copyable** or by-value type
  and moves from it
- `Msg` is required to be `std::movable`, and jaal never copies a Msg after
  handing it off
- `Sink<Msg>::send(Msg)` takes by value, so the caller's object is moved in
  and the caller keeps nothing

What C++ can't do is stop the caller from using the moved-from object
afterwards. `-Wuse-after-move` (clang-tidy `bugprone-use-after-move`) is on in
CI, which makes that Level B.

### 4.2 `Sendable`, deep

DESIGN.md 3.7 listed `Sendable` as a shallow lint. It can now look inside
structs. C++26 structured binding packs (P1061) let a template name every
field type of an aggregate without reflection. Tested and working on GCC 16,
clang 22 and llvm-mingw clang 23:

```cpp
template <class T>
auto fields(const T& t) {
    auto& [...xs] = t;
    return meta::list<std::remove_reference_t<decltype(xs)>...>{};
}
```

So `Sendable<T>` is defined by structure:

| T | Sendable when |
|---|---|
| arithmetic, enum, `std::byte`, `bool` | always |
| `std::string`, `std::vector<U>`, `std::optional<U>`, `std::variant<Us...>`, `std::pair`, `std::tuple`, `std::array`, `std::map`, `std::unordered_map`, `std::unique_ptr<U>` | when every `U` is |
| `std::shared_ptr<const U>` | when `U` is `Frozen` (4.3) |
| `std::shared_ptr<U>` (non-const) | never |
| `T*`, `T&`, `std::string_view`, `std::span`, iterators, `std::function` | never |
| `std::move_only_function<Sig>` | never as data. Tasks use a separate path (4.6) |
| an aggregate struct | when every field is, checked recursively |
| a class with private fields | never, unless it opts in |

Opt-in for classes jaal can't look inside:

```cpp
template <> inline constexpr bool jaal::sendable_opt_in<MyHandle> = true;
```

An opt-in is grep-able and shows up in review. It's the one Level C hole in
this table.

With this, the example that defeated the shallow version is now rejected:

```cpp
struct GotLine { std::string_view text; };   // borrowed view hidden in a struct
using Msg = std::variant<GotLine>;
// error: jaal: Msg is not Sendable: field 'GotLine::text' is std::string_view,
//        which borrows memory another thread may free
```

**Where binding packs aren't available** (MSVC today; GCC and clang accept
them in C++23 mode as an extension, with a warning): `Sendable` falls back
to the shallow rules for aggregates, so it still rejects pointers, views and
non-const `shared_ptr` at the top level and in std containers, but it can't
see inside a user struct. That's a real weakening on that one toolchain.
The CI matrix builds on GCC and clang too, so a violation added on Windows
still fails CI on Linux. The feature is chosen by
`__cpp_structured_bindings >= 202411L`, never by compiler name.

### 4.3 `Frozen` and `shared<T>`: shared, never mutated

Some data really should be shared without copying: a parsed document, a
theme, a large config. `jaal::shared<T>` is the only built-in way:

```cpp
template <Frozen T>
class shared {
public:
    template <class... A> static shared make(A&&...);     // builds, then freezes
    auto operator->() const noexcept -> const T*;
    auto get() const noexcept -> const T&;
    // no non-const access, no .reset() that could free under a reader
};
```

`Frozen<T>` means "nothing in T can change through a const reference":

- `T` is `Sendable`
- no field is declared `mutable` (checked: structured bindings on a
  `const T&` give non-const types for `mutable` fields, tested on all three
  compilers)
- recursively, the same for every field
- no `std::atomic`, no mutex (those exist to allow changes)

This fixes the theme slot bug (P5) by construction. `live_slot` becomes an
atomic `shared<const Theme>`, and a reader's copy keeps the theme alive for
as long as it holds it. Nobody can free a theme out from under a reader,
because nobody holds a raw pointer.

### 4.4 `Sink<Msg>`: the only way back into a loop

As in DESIGN.md 3.6, plus what the maya bugs teach:

```cpp
template <Sendable Msg>
class Sink {
public:
    auto send(Msg m) const -> bool;               // false once the kernel is gone
private:
    std::weak_ptr<detail::mailbox<Msg>> box_;
    explicit Sink(std::weak_ptr<...>);            // only the kernel mints sinks
    friend struct kernel_access;
};
```

- **Weak by type.** There's no API that turns a Sink into a strong reference,
  so a task can never keep the mailbox alive. The P2 cycle, leak and
  self-join can't be written.
- **The mailbox is never destroyed on a worker.** The kernel holds the only
  strong reference and destroys it on the kernel thread during `finish()`.
  A `static_assert` in the mailbox's destructor path can't check threads,
  so a debug-build assert does (Level B), and the type makes it unreachable
  (Level A).
- **Msg must be Sendable**, so nothing borrowed rides along.

### 4.5 `loop_bound<T>`: the one-thread rule as a type

P1 is maya's real safety net, and today it's a comment. `loop_bound<T>`
makes it a type:

```cpp
template <class T>
class loop_bound {
public:
    auto get(loop_token) -> T&;               // need proof you're on the loop
    auto get(loop_token) const -> const T&;
};

class loop_token {                            // proof "this code runs on the loop thread"
    loop_token() = default;                   // private
    friend class kernel_access;               // only the kernel creates one
public:
    loop_token(const loop_token&) = delete;   // can't be copied into a lambda
    loop_token(loop_token&&) = delete;        // or moved into one
};
```

- The kernel passes `loop_token&` into `update`, `view` and host callbacks,
  on the loop thread only.
- A task body never receives one, and it can't capture one: the token is
  neither copyable nor movable, and a lambda that captures it by reference is
  rejected because task bodies must be captureless (4.6).
- **There is no static or global way to get a token.** No `loop_token::current()`,
  no singleton. This matters: a captureless lambda can still call any
  function and read any global, so a static accessor would hand the token
  straight to a worker. The only tokens that exist are the ones the kernel
  passes down the call stack. (Prototyped: a captureless lambda calling a
  static token getter passes the captureless check, which is why there
  isn't one.)
- So code running on a worker has no way to reach loop-bound state.

What changes for maya: its `thread_local` caches become `loop_bound` state
owned by the host, reached through the token. Code that ran on a worker
today would stop compiling instead of silently getting an empty cache.

This is the largest migration item and doesn't need to happen at once.
`thread_local` keeps working. `loop_bound` is how new code, and code that's
touched, gets the check.

### 4.6 Task bodies: captureless, arguments by move

The P2 and P3 bugs came from what a task captured. jaal removes captures
entirely:

```cpp
// a task is a captureless function plus the values it's given, by move
template <Sendable Msg, class... Args>
    requires (Sendable<Args> && ...)
auto task(Args... args, auto body) -> Cmd<...>
    requires captureless<decltype(body), Sink<Msg>, std::stop_token, Args...>;

// in update:
return fx::task<Msg>(path, [](Sink<Msg> out, std::stop_token st, std::string path) {
    out.send(Loaded{read_file(path, st)});
});
```

- **Captureless** is checked by convertibility to a plain function pointer,
  which the standard guarantees only for captureless lambdas. Verified on all
  three compilers.
- **Everything the body needs is an argument**, and every argument must be
  `Sendable`. The body owns its inputs outright.
- So a task can't hold `this`, a reference to the model, a raw pointer,
  a `loop_token`, or the mailbox. None of those can be named inside a
  captureless lambda except through its arguments, and the arguments are
  checked.

The hole, stated plainly: a captureless lambda can still read and write
**globals and statics**. Captureless-ness stops borrowing from the caller;
it does not stop sharing through a global. That's why jaal itself has no
global mutable state reachable from user code (the `loop_token` point in
4.5), and why the ban-list check (section 7) flags `static` mutable
variables and `thread_local` in app code. TSan catches what slips through.
(Prototyped on GCC 16, clang 22 and llvm-mingw clang 23.)

That rules out every lifetime bug in maya's history of tasks, at compile
time.

The cost: slightly more verbose task code than `[&]`. That's the trade.

### 4.7 `scope`: structured concurrency for helpers

Sometimes a blocking job needs helper threads: read stdout and stderr at
once, run a watchdog next to a request. agentty does this with raw threads
and `[&]` captures, which is only safe if every helper is joined before the
function returns.

`jaal::scope` makes that the only possible shape:

```cpp
jaal::scope(st, [&](jaal::nursery& n) {
    auto out = n.spawn([&] { return drain(proc.stdout_fd()); });
    auto err = n.spawn([&] { return drain(proc.stderr_fd()); });
    return Result{out.join(), err.join()};
});   // every spawned helper has finished by this line, even on exception
```

- `scope` is a function that runs a block and joins every helper before it
  returns. There's no handle that escapes it.
- `nursery` is neither copyable nor movable, and only lives inside the block,
  so it can't be stored or returned.
- Borrowing with `[&]` is fine here, because nothing spawned can outlive the
  block. This is the one place in jaal where captures are allowed, and it's
  safe by structure.
- If a helper throws, the others are asked to stop through the shared stop
  token, all are joined, and the first exception is re-thrown.
- The parent's stop token propagates, so cancelling the task cancels its
  helpers.

This is the Trio/Kotlin "nursery" pattern, which has a strong track record
for eliminating leaked threads.

What C++ can't stop: a helper that writes to a shared local from two helpers
at once. Two helpers both doing `++count` on a captured `int` is a race.
TSan catches it (Level B). The discipline is: helpers return values through
`join()`, and don't write to shared locals.

### 4.8 `guarded<T>`: when there must be a lock

Sometimes a lock is the right answer (a cache shared by many workers).
`guarded<T>` makes the unsafe forms impossible to write:

```cpp
template <class T>
class guarded {
public:
    template <class F> requires Sendable<std::invoke_result_t<F, T&>>
    auto with(F&& f) -> std::invoke_result_t<F, T&>;          // exclusive

    template <class F> requires Sendable<std::invoke_result_t<F, const T&>>
    auto read(F&& f) const -> std::invoke_result_t<F, const T&>;   // shared
private:
    mutable std::shared_mutex m_;
    T value_;
};
```

- **The data is only reachable while the lock is held.** There's no getter,
  no raw mutex, nothing to forget to lock.
- **Nothing escapes the lock.** The lambda's result must be Sendable, which
  bans references, pointers and views into `T`. You get a copy out, never a
  handle in.
- **No two locks at once, by construction.** Calling `with` on a second
  `guarded` from inside the first's lambda would allow lock-order
  deadlocks. A thread-local "lock held" flag rejects that at runtime in debug
  builds (Level B). A stricter version (a `lock_level<N>` parameter that must
  increase) can make it Level A later if it's needed.
- Used rarely. The preferred answer is always an owner you send messages to.

### 4.9 `once<T>` and `latch`-style results

For P4 (single-flight offload with a result slot), jaal gives the pieces so
nobody hand-rolls atomics again:

- **The result is a Msg.** The worker sends `Parsed{doc}` through a Sink. The
  loop's `update` adopts it. No `atomic<bool> ready`, no acquire/release, no
  `const_cast` into a widget.
- **Single-flight** is a keyed source: the key is "parse for widget X", so
  starting a new parse for the same key cancels the old one (its stop token
  fires) and the reconciler starts the new one. That's exactly
  `async_latest_source_` coalescing, done by the kernel.
- **Cancellation checks** use the task's `stop_token`, which is also what the
  kernel triggers at shutdown.

So maya's markdown async path becomes a task plus a Msg, with no atomics in
widget code at all.

### 4.10 Signals stay in the platform layer

P6 is correct and it stays where it is, moved into `jaal/platform/<os>/`.
The rule from DESIGN.md 6.7 holds: user code never runs inside a signal
handler, it only ever sees signals as events. The async-signal-safety
reasoning stays in one place, maintained by one person, tested by the
conformance suite.

`restore_guard` callbacks are the one place user code does run at crash
time. They must be `noexcept` (Level A) and async-signal-safe (Level C,
documented).

## 5. What each maya pattern becomes

| maya today | jaal | level |
|---|---|---|
| P1 `thread_local` caches | `loop_bound<T>` reached with a `loop_token` | A |
| P2 BackgroundQueue + weak sink | `Sink<Msg>`, weak by type, mailbox destroyed only on the loop | A |
| P3 detached isolated threads | `isolated_task`: captureless body, Sendable args, owned by jaal's detach path | A |
| P4 markdown async slot | task + Msg, single-flight as a keyed source | A |
| P5 theme `atomic<const Theme*>` | atomic `shared<const Theme>`, readers hold a reference | A |
| P6 signal handlers | unchanged, moved into `platform/` | C (documented) |
| P7 relaxed counters | unchanged; jaal provides `id_source<Tag>` so the pattern has one home | A |

## 6. The bug classes, one by one

| bug | how it's prevented | level |
|---|---|---|
| use-after-free across threads | tasks own all inputs (captureless, Sendable args); `shared<T>` refcounts; Sink is weak | A |
| a thread outlives what it borrowed | only `scope` allows borrowing, and it joins first | A |
| data race on plain data | nothing mutable is shared: owned, moved, or frozen. `guarded<T>` for the rare lock | A |
| forgot to lock | `guarded<T>` has no unlocked access | A |
| reference escapes a lock | `with`/`read` results must be Sendable | A |
| lock-order deadlock | one lock per call; nested `with` rejected | B (debug) |
| cycle keeps runtime alive (P2) | Sink can't be upgraded to strong | A |
| worker destroys runtime, self-join (P2) | mailbox's only strong owner is the kernel | A |
| exception kills process from a worker | every thread body is wrapped (from agentty's `isolated_thread`) | A |
| detached thread leaks work at exit | isolated tasks get the stop token; the kernel requests stop | A |
| stale state read from another thread | loop state is `loop_bound`; workers can't get a token | A |
| borrowed view inside a Msg | deep `Sendable` via structured binding packs | A |
| mutable field inside "immutable" shared data | `Frozen` rejects `mutable` fields | A |
| a task shares data through a global or static | ban-list flags mutable statics; jaal exposes none | B |
| use after move | `bugprone-use-after-move` | B |
| raw `std::thread` / `.detach()` / `std::async` in app code | ban-list test | B |
| race on a global or captured local jaal can't see | TSan in CI | B |
| async-signal-unsafe code in a restore callback | documented | C |

## 7. The escape hatches, and keeping them honest

C++ lets anyone reach around a type with a raw pointer or a cast. jaal can't
stop that. It can make it visible:

1. **A ban-list check** (like the layering test): fails on `std::thread`,
   `.detach()`, `std::async`, `std::mutex`, `std::atomic`, `const_cast` and
   `thread_local` outside an allowlist file. The allowlist is the list of
   places a human has checked by hand. jaal's own `platform/` and `kernel/`
   are on it. In maya and agentty, that list starts long and shrinks.
2. **TSan and ASan presets** run the whole test suite. They already exist in
   CMakePresets.json.
3. **The simulated platform** runs concurrent tests under a seeded scheduler,
   so a race that shows up once in a thousand real runs shows up on a
   specific seed, every time.
4. **`sendable_opt_in`** is grep-able, and each use needs a comment saying
   why the type is safe to send.

## 8. Costs

- **Verbosity.** Captureless tasks with explicit arguments are wordier than
  `[&]`. That's the price of the lifetime guarantee, and it only applies to
  code that crosses threads.
- **Compile time.** Deep `Sendable` walks every field of every Msg type. It's
  one expansion per struct and memoised, but agentty's Msg is big. Measure at
  migration step 5 (DESIGN.md 12).
- **Copies.** Moving data across threads instead of sharing it costs moves,
  and occasionally copies. `shared<T>` covers the cases where that matters.
- **Aggregates only.** Deep `Sendable` works on plain structs. Classes with
  private fields need an opt-in, and that's a real hole a careless opt-in
  opens.

## 9. What's still convention (Level C)

Kept short on purpose:

1. `restore_guard` callbacks must be async-signal-safe.
2. `sendable_opt_in` must only be used for types that are actually safe to
   move between threads.
3. `update` and `view` must be pure. (DESIGN.md 3.1.)
4. Code outside jaal that uses raw threads anyway, if it's on the allowlist.

## 10. Build order

These go into `core/` and `kernel/`, in order:

1. `core/sendable.hpp`: deep Sendable with structured binding packs, opt-in
2. `core/frozen.hpp` and `core/shared.hpp`
3. `core/sink.hpp`, weak by type, only minted by the kernel
4. `core/fx/task.hpp`: captureless body, Sendable arguments
5. `kernel/loop.hpp`: `loop_token`, `loop_bound<T>`
6. `kernel/scope.hpp`: `scope` and `nursery`
7. `kernel/guarded.hpp`
8. `tests/lint/`: the ban-list check
9. Compile-fail tests for every Level A row in section 6. Each row gets a
   program that tries the bug and must not build.

Row 9 is what makes this real. A guarantee without a compile-fail test is a
claim.
