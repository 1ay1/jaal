# What jaal offers, and what it doesn't

This is the scope contract. If you're deciding whether jaal fits, read this
first. Anything planned but not built is marked **planned**. Everything
else below is built and passes the full test matrix (gcc, clang, ASan,
TSan, Windows under wine).

## In one paragraph

jaal is a typed Elm runtime for C++26 plus the platform layer it runs on. You
write a `Model`, a `Msg`, and a pure `update`. jaal runs the loop, the timers,
background work, OS waiting, and signals, on Linux, macOS and Windows. It
draws nothing and speaks no protocol: renderers (maya) and servers plug in as
hosts. Its distinguishing feature is that the contracts of an Elm program
(effects are data, state changes only in `update`, work crossing threads
can't hold borrowed memory) are checked by the compiler, not by convention.

## Is it still Elm?

The core is: `init`, `update : Msg -> Model -> (Model, Cmd)`, `subscribe :
Model -> Sub`, effects as data the runtime runs, one message at a time,
subscriptions diffed by key. The newer pieces sit AROUND that loop, not in
it. `sim` swaps who runs the effects, `timeline` and `replay` just fold
recorded messages through `update`, `diff` compares two models.

Where it differs from Elm, and why:

| Elm | jaal | why |
|---|---|---|
| `update` is pure by the language | pure by convention; replay and sim show violations | C++ can't prove purity |
| models are immutable values | models are values you move through `update`; `Frozen` is required where it matters (`timeline`, `shared<T>`) | C++ has no deep immutability by default |
| `view` is part of the program | optional; a host that draws asks for it | jaal also runs servers and tools with no screen |
| `Task` / `Cmd` from a fixed library | `task` runs your C++ on another thread; `stream` is a keyed subscription running your code | the effects are open-ended, but what crosses threads is checked (`Sendable`) |
| no exceptions | a throwing `update` is a fault; `skip` keeps the old model, `stop` quits | C++ code can throw |
| effects run by the runtime only | a host adds its own effect types (rows) | terminals, sockets, GUIs each need their own |

What jaal won't add, because it would stop being Elm: re-entrant `update`,
`await` inside `update`, shared mutable state between `update` and
background work, or effects that run during replay.

## Offers

### The Elm core

| | |
|---|---|
| `Program` concept | `Model`, `Msg`, `init`, `update`; `subscribe` and `view` optional |
| `Cmd<Msg, Row>` | effects as data; the row (set of allowed effects) is in the type |
| `Sub<Msg, Row>` | subscriptions as data; diffed by key against what's running |
| row subtyping | a `Cmd` with fewer effects converts to one with more, never back |
| `map` | re-target a child's `Cmd`/`Sub` at the parent's `Msg` (components) |
| `child<C, ParentMsg, Wrap>` | embed program `C` in a parent: `match`, `update` in place, mapped `init`/`subscribe`, stream keys prefixed so two copies don't clash. One fixed slot per `child<>`; lists of children aren't supported yet |
| `jaal::program<>` | declare effects once, get `Cmd`, `Sub`, `step` aliases |
| `step` | `update` can return just a model, meaning "no effects" |

### Built-in effects and sources (no host code needed)

The core is deliberately small. An effect is built in only if it can't be
written outside jaal: it needs the kernel's clock, pool, fold or
subscription lifecycle. Everything else is a library on top of `task` and
`stream` ([decisions.md](decisions.md) D25).

| name | kind | what it does |
|---|---|---|
| `quit(code)` | effect | stop after the current batch |
| `send(msg)` | effect | a message straight back into the loop, folded this step |
| `after(d, msg)` | effect | one-shot timer |
| `task(body, args...)` | effect | run once on the worker pool, result as a Msg |
| `task(isolated, body, args...)` | effect | same, on its own thread (for work that may hang) |
| `now(f)` | effect | the kernel's clock as a Msg (simulated in tests) |
| `random(f)` | effect | draws from the kernel's seeded generator as a Msg (reproducible) |
| `child<C, Msg, Wrap>` | composition | one child program in a fixed slot |
| `children<C, Msg, Wrap>` | composition | a keyed LIST of child programs; stream keys are per child |
| `debounce<T>` / `throttle` | model values | "settle before acting" and "at most once per interval" |
| `every(d, msg)` | source | repeating timer; keeps its phase across model changes |
| `stream(key, body, args...)` | source | long-running work; runs while subscribed, cancelled when not |
| `on_signal(set, f)` | router | Ctrl+C, terminate, hangup, resize, child as messages |

### Extension points (for hosts and libraries)

| | |
|---|---|
| `pure_fx<T, "name">` | a new effect in one line |
| `router<Event, "name">` | a new event subscription in one line; `Sub::on(tag, f)` |
| custom sources | anything with a key, started and stopped by the host |
| hosts | supply events (a variant of kinds), run extra effects, draw |
| `HostFor<H, P>` | compile error when a host can't run an effect the program uses |

### Runtime behaviour you can rely on

- One message at a time, on one thread. `update` never needs a lock.
- Events are routed one at a time with a re-subscribe between them, so a
  key that changes the mode is seen by the next key's subscription.
- `subscribe` only runs when the model changed.
- A `quit` stops the rest of the current batch; later effects don't run.
- A long stall fires a repeating timer once, not in a burst.
- Timeouts round up, never down (no busy-spin just before a deadline).
- An idle program sleeps: no polling, near-zero CPU.
- A message storm is folded in bounded batches, so input and drawing still
  get turns.

### Safety, checked by the compiler

- `Sendable<T>`: deep check that a value holds nothing borrowed (views,
  pointers, shared mutable ownership) before it crosses a thread. Looks
  inside plain structs field by field.
- `Frozen<T>`: `const` really is deep (no `mutable` fields, no pointee
  reachable through `const`).
- `shared<T>`: the one way to share data between threads; requires both.
- Task and stream bodies can't capture anything; their inputs are
  `Sendable` arguments.
- `Sink<Msg>`: the only way back into the loop; weak, can't keep the
  runtime alive.
- `scope`: helper threads always joined before the block returns.
- `guarded<T>`: data only reachable under its lock; nothing escapes it.
- `loop_bound<T>`: loop-thread-only state, reachable only with a token only
  the kernel makes.

Most have compile-fail tests that pin the error message; `loop_bound` is
covered by static assertions.

### Production behaviour

- **Faults**: a throwing `update`, `subscribe`, loop-side effect or task is
  contained and reported. Policy `stop` (default) quits with 70; policy
  `skip` restores the model exactly and drops the message.
- **Bounded shutdown**: a task ignoring its stop token can't hang exit.
- **Bounded mailbox** (optional): block / drop newest / drop oldest, with
  counters. A sender blocked on a full mailbox is released at shutdown.
- **Tracing hook**: fold, effect, subscribe, fault and step events, with
  timings. One branch per event when off.
- **Record and replay**: record the messages a run folds; replay them
  through `update` with no effects, and get the same model.

### Platform

| | Linux | macOS / BSD | Windows |
|---|---|---|---|
| reactor | epoll (+ poll) | kqueue | WaitForMultipleObjects |
| signals | self-pipe | self-pipe | console control events |
| byte-mode pipes (MSYS2 stdin) | n/a | n/a | yes, polled |
| **tested** | run | compiled only | run under wine |

The conformance suite runs the same checks against every reactor backend.

### Testing

- `given<P>`: test `update` with no kernel at all. `when(msg)` folds,
  `expect(name, pred)` checks the model, `expect_effect<fx::task>(n)`
  checks the returned Cmd as data (no mocks), `settle()` runs tasks and
  `now` inline and folds what they send. `after`/`quit` are recorded, not
  run. Failures are collected with a field diff of what the last message
  changed, so one `report()` lists everything.
- `headless<P>`: no screen, fake clock, records every effect instead of
  running it. Timers fire when a test says `advance()`.
- `sim<P>`: deterministic simulation. One seed controls task latency,
  the order of anything due at the same instant, and injected task crashes
  and hangs. Invariants are checked after every message. `explore()` runs
  a scenario over many seeds and returns the first one that breaks, with
  the message log that replays it through `jaal::replay`. Same seed, same
  run, on every platform (the RNG is jaal's own, not `<random>`'s).
  Task bodies run on the loop thread; stream bodies don't run at all, the
  test feeds a stream through `stream(key)`.
- `timeline<P>`: walk a recorded run (a sim failure's messages, or a
  `recording`) like a video. `at(step)` is the model after that many
  messages, `message(step)` is what caused it, `changes(step)` is a field
  diff. `first_bad(pred)` finds the first step that breaks a check;
  `bisect(pred)` does it in O(log n) checks for properties that stay bad.
  Snapshots every 64 steps, so any jump folds at most 63 messages.
- `diff(a, b)`: which fields of two structs differ, printed with
  `std::format` where possible. Fields are named by position (`.2.1`).
- `kernel::executor<Msg>`: where task and stream bodies run. The kernel
  asks the host for one (`make_executor`) and falls back to the real pool.
- `scripts/check.sh`: gcc, clang, ASan, TSan, and Windows under wine.

### Measured cost

Release build, one core, from `bench/` (`jaal_bench`):

| path | cost |
|---|---|
| one message through `update`, no effects | ~19 ns |
| one message with an effect (`after`) | ~45 ns |
| cross-thread send + drain + fold, 4 producers | ~195 ns |
| a `step` with nothing to do | ~3.6 ns |
| a message that changes the model, plus re-subscribe of 8 timers | ~660 ns |
| a whole sim run: start, 3 inputs, 3 tasks, 2 invariants, shutdown | ~1.1 us (~900k seeds/s) |

For comparison, the C prior art (`~/projects/tea`) measured ~54 ns for its
fold round trip and ~234 ns cross-thread with 4 producers. These are
numbers for this machine and this benchmark, not a claim about every
workload.

## Doesn't offer

These are **deliberate** — each one is someone else's job or a later layer.

- **Rendering.** No widgets, no layout, no terminal or GUI drawing. That's
  maya (terminals) or a GUI library, plugged in as a host.
- **Protocols and I/O batteries.** No HTTP client, no TCP helpers, no file
  watching, no database. They're effects and sources a library can add
  with `pure_fx` / `router` / custom sources. jaal's core stays small.
- **Actors.** One program per kernel. Many programs messaging each other,
  with supervision, fits the design (a `Sink` becomes an address) but isn't
  built.
- **Async/await or coroutines.** Background work is tasks and streams with
  messages back. There's no `co_await` in `update`; that would make it
  impure.
- **A stable ABI.** jaal is header-heavy templates plus a small static
  library. Pin a version.
- **Pre-C++26 compilers.** Needs GCC 16, clang 22, or Apple clang 21 for the
  deep `Sendable` check (structured binding packs). MSVC isn't supported yet.
- **Guaranteed purity.** C++ can't prove `update` doesn't print or read a
  global. jaal can't stop it; replay makes violations show up.

## Planned, not built

- **A real run on Windows outside wine.** macOS is done: the whole suite,
  including the kqueue conformance tests and real signal delivery, runs on
  arm64 macOS under debug, asan, tsan and release.
- **Simulated I/O** (sockets, files) for the sim, once there are I/O
  effects in a library to simulate.
- **Fuzzing** of the reconciler, timer heap and routing.
- **A user guide** (the README has a tour; there's no step-by-step guide).
- **maya as a jaal host**, then agentty on top.
