# Design decisions

Each entry is a choice that shaped jaal: what was decided, what else was on
the table, why, and what it costs. Where a decision came from a real bug, the
bug is named. Newer entries at the bottom.

For the full technical design see [design.md](design.md); for the threading
model see [concurrency.md](concurrency.md); for scope see
[scope.md](scope.md).

---

## D1. jaal is a runtime, not a framework

**Decision.** jaal is the Elm loop plus the platform layer. No rendering, no
widgets, no protocols.

**Alternatives.** Grow maya's loop into a full application framework
(rendering + networking + storage), or ship "batteries included".

**Why.** The loop, the timers and the OS waiting are the same for a
terminal app, a GUI and a server; the drawing and the protocols aren't.
Pulling the shared part out lets maya stay a renderer and lets a server use
jaal without a terminal library. Batteries are easier to add later as
separate libraries (they're just effects) than to remove from a core.

**Cost.** Anyone building a real app needs a host: maya for terminals, their
own for servers. That's more pieces than one framework.

## D2. Effects live in the type (rows)

**Decision.** A `Cmd` carries the set of effects it may contain:
`Cmd<Msg, row<quit, after, beep>>`. A host must handle every effect in the
program's row or it doesn't compile.

**Alternatives.** One closed `Cmd` variant (maya's), or type-erased effects
checked at runtime.

**Why.** maya's closed variant forced its generic `Cmd` to know about
terminal scrollback, and made any other host impossible. Runtime-checked
effects fail in production instead of at build time. Rows let each host
bring its own effects, and let a component say "I only use `after`", which
the compiler then holds it to.

**Cost.** Longer types (mitigated by `jaal::program<>` aliases), and every
`Cmd` type is an instantiation. Rows are canonicalised (sorted by name,
deduplicated) so spelling and order never create distinct types.

## D3. Rows widen, never narrow

**Decision.** `Cmd<Msg, A>` converts to `Cmd<Msg, B>` when A ⊆ B, implicitly.
The other direction is `= delete("reason")`.

**Why.** It's subtyping: a child component using fewer effects fits into a
parent that allows more. Narrowing would let a parent smuggle effects past a
component's declared set.

## D4. The kernel never owns the thread

**Decision.** The kernel is a value you drive: `start`, `route(event)`,
`step`, `next_deadline`, `finish`. `run<P>()` is a thin loop over it.

**Alternatives.** A `run()` that owns `main()`, as maya's does.

**Why.** GUI toolkits own the main thread (and macOS requires it). Servers
may already have an event loop. Tests want to step by hand. A kernel that
owns the thread can't be embedded in any of them.

**Cost.** Hosts write a little more glue. `run<P>(host)` covers the common
case.

## D5. The kernel is not movable

**Decision.** `kernel` can't be copied or moved; `start()` returns it by
guaranteed copy elision.

**Why.** Tasks, streams and the host hold references into it (sinks point at
its mailbox). A move would leave them pointing at the old address. Making it
immovable turns that into a compile error.

## D6. One message at a time, on one thread

**Decision.** `update` runs on the loop thread, one message at a time. Work
off the loop comes back only as messages through a `Sink`.

**Why.** It's what makes `update` lock-free by construction, and replay
possible. maya already worked this way informally (68 `thread_local`s
relying on it); jaal makes it the only way.

**Cost.** Heavy computation in `update` blocks the loop. The answer is a
task or stream, which is the Elm answer anyway.

## D7. Deep `Sendable`, via structured binding packs

**Decision.** Before a value crosses threads, jaal checks it holds nothing
borrowed, looking inside plain structs field by field (C++26 P1061).

**Alternatives.** A shallow check (pointers and views at the top level only),
or no check.

**Why.** The shallow version missed a `string_view` one struct deep, which
is exactly how real bugs hide. P1061 made the deep check possible without
reflection. Classes jaal can't see inside must opt in explicitly, so every
unchecked type is a visible, commented claim.

**Cost.** Needs GCC 16 / clang 22. It found a real race in agentty
(`LazyBytes`, 19 TSan reports → 0) on its first run against real code.

## D8. `Frozen` is separate from `Sendable`

**Decision.** Two properties: `Sendable` (safe to *move* to another thread)
and `Frozen` (can't change through `const`). Sharing requires both.

**Why.** They really are different: a `unique_ptr<T>` is safe to move but not
to share (every holder could mutate `*p`); a `string_view` is immutable
through itself but borrows memory. One property can't express both.

## D9. Task bodies can't capture

**Decision.** A task or stream body must be captureless; everything it
needs is passed as `Sendable` arguments.

**Alternatives.** Allow captures and trust the author.

**Why.** Every lifetime bug in maya's background queue came from what a task
captured (a strong reference to the queue caused a cycle, then a worker
joining itself at shutdown). Checked by conversion to a function pointer,
which the standard only allows for captureless lambdas.

**Cost.** Slightly wordier task code. The one hole: a captureless function
can still touch globals, which the banlist lint and TSan cover.

## D10. `Sink` is weak by type

**Decision.** A `Sink` holds a `weak_ptr` to the mailbox and can't be made
strong. Only the kernel creates sinks.

**Why.** maya's queue-owns-task-owns-queue cycle. Here it can't be written:
a task can never keep the runtime alive, and a send after shutdown just
returns `false`.

## D11. Subscriptions are reconciled by key

**Decision.** `subscribe(model)` returns a description; the kernel diffs it
against what's running by key. Same key: kept (a timer keeps its phase). Key
gone: stopped. New key: started.

**Why.** This is Elm's model, and it's what makes long-lived work
declarative: the program says what should be running, never "start" or
"stop". maya keyed timers by interval only, which silently starved every
second timer with the same interval; jaal keys `every` by (interval,
position).

## D12. Routers vs sources

**Decision.** Two kinds of subscription. Routers are stateless filters over
host events, rebuilt every `subscribe()`, and may capture. Sources have a key
and a lifetime.

**Why.** They have different lifetimes and different safety rules. A router
runs on the loop thread inside dispatch, so capturing is safe. A source runs
work that outlives a single call, so it follows the task rules.

## D13. Re-subscribe between events

**Decision.** Host events are routed one at a time; the program is
re-subscribed between them.

**Why.** maya's "^T m o" bug: a fast terminal delivers `^T m o` in one read.
Routing all three through the subscription from before `^T` sends `m` and
`o` to the editor instead of the picker `^T` just opened.

## D14. One reactor concept, per-OS backends, one conformance suite

**Decision.** Waiting is a `Reactor` concept with backends (epoll, kqueue,
poll, Windows); every backend runs the same conformance suite.

**Alternatives.** `#ifdef` per call site, or one lowest-common-denominator
backend.

**Why.** Per-OS behaviour is where the bugs are (a timeout cast that wraps,
EINTR ending a wait early, a hangup that looks like "readable"). One suite
that every backend must pass turns "works on my OS" into a test.

**Cost.** macOS is compiled but not yet run; Windows is run under wine.

## D15. Level-triggered readiness

**Decision.** Every reactor reports a handle as ready for as long as it is,
not only on the edge.

**Why.** Edge-triggered loses data if a caller doesn't drain a handle fully.
The suite first missed this (epoll with `EPOLLET` passed); a dedicated check
now fails it.

## D16. Timeouts round up, and conversions saturate

**Decision.** A deadline becomes a wait timeout by rounding up, in one
function; duration conversions clamp instead of overflowing.

**Why.** Rounding down made maya wake just before a timer, find nothing, and
spin. And `duration_cast<nanoseconds>(milliseconds::max())` wraps to −1 ms,
so a "never" timer fired at once — found by a test, one step before the
overflow-safe addition that was already there.

## D17. Signals are events

**Decision.** No user code runs inside a signal handler. Signals become
events on the loop thread. A program that doesn't handle Ctrl+C still stops
(exit 130).

**Why.** Async-signal-safety is hard and belongs in one place. And a program
must never become un-killable because it forgot to ask for Ctrl+C.

## D18. The mailbox wakes under its lock

**Decision.** `post()` signals the waker while holding the mailbox lock.

**Why.** A poster paused between unlocking and waking could run its wake
after shutdown closed the reactor fd and the OS reused the number, writing
into the wrong file. TSan didn't see it (a use-after-close, not a data race);
a test holding the poster in that window did: 20/20 rounds before, 0/20
after.

## D19. Faults are contained, with a policy

**Decision.** A throwing `update`, `subscribe`, effect or task is caught and
reported. `stop` (default) quits with 70; `skip` restores the model exactly
and drops the message.

**Why.** `update` takes the model by value; a throw left it moved-from (a
3-line model came back with 0). Tasks' exceptions were swallowed, so the
program waited forever. Keeping the model needs a copy before each `update`,
which is only paid under `skip`, and only for copyable models.

## D20. Shutdown is bounded

**Decision.** Shutdown waits a grace period for workers, then abandons
stragglers and reports them.

**Why.** A task ignoring its stop token hung `finish()` forever. Abandoning
is only safe because the pool's state is co-owned by every worker, so an
abandoned worker returning later touches memory it still owns (checked
under ASan/TSan).

## D21. Streams are core sources

**Decision.** `Sub::stream(key, body, args...)`: long-running work that runs
while subscribed and is cancelled when not. The kernel runs it; no host code.

**Why.** "Keep this running while I'm in this state" is the most common
thing real apps need (downloads, watchers, readers), and tasks can't be
cancelled per call. A stopped stream's late sends are dropped by generation,
so a cancelled stream can't land a message in a model that no longer
expects it.

## D22. Hosts produce a variant of event kinds

**Decision.** A host's event type can be a `std::variant`; each router takes
one alternative. A router for a kind the host never produces is a compile
error.

**Why.** A real host has several kinds (keys, mouse, resize). The first
version only matched one type, so `on_key` and `on_mouse` couldn't coexist.

## D23. Replay folds messages, not effects

**Decision.** Record the messages `update` folds; replay them straight
through `update` with no effects run.

**Why.** Effects are the run's outputs; re-running them (network, files) is
what a replay must not do. The recorded messages already hold what those
effects produced. It works because `update` is pure, and fails loudly when
it isn't, which is useful in itself.

## D24. No hosted CI

**Decision.** `scripts/check.sh` runs the whole matrix locally.

**Why.** The maintainer's choice. The script covers gcc, clang, ASan, TSan
and Windows under wine in about two minutes, and exits non-zero on any
failure.

## D25. The smallest core that can express everything

**Decision.** An effect or source is built in only if it can't be written
outside jaal, because it needs the kernel's clock, worker pool, fold or
subscription lifecycle. The core is: `quit`, `after`, `task` (pool or
isolated), `now`, `every`, `stream`, plus `on_signal` from `run()`.

**Alternatives.** Ship common effects (HTTP, files, processes, logging) in
the core.

**Why.** Every built-in is a permanent promise that must behave the same on
every OS and in the test host. With `task` and `stream` in the core, a
watcher, a socket reader or a download is a library, not a kernel change.
`isolated_task` was merged into `task` as a placement, since it differed
only in which thread runs it. `now` was added because a task reading
`std::chrono` reads the real clock, which the test host can't control.

## D26. Invalid states are unrepresentable

**Decision.** Where a struct allowed combinations that mean nothing, it
becomes a type that can only hold valid ones.

**Examples.**
- `turn` had `bool quit` and `int exit_code`. An exit code without a quit
  was representable. It's now `std::optional<int> exit`: set exactly when
  quitting, with its code. The kernel's own `quit_` + `exit_code_` pair
  became one `std::optional<int>` the same way.
- Task placement is a closed enum (`pool` / `isolated`) rather than a bool,
  so the choice is spelled at the call site.
- The kernel isn't movable, a `loop_token` can't be copied or made, a
  `nursery` can't escape its `scope`, and a `shared<T>` can't be null.

## D27. Fast paths are measured, then built

**Decision.** Optimisations go where a profile says, and the before/after
number is recorded where the change is.

**Examples.**
- An idle `step` took the task-fault lock and the mailbox lock every time:
  75% of its cost. Both now check an atomic hint first and only lock when
  there's work. 36 ns → 3.6 ns.
- Reconciling a subscription built three hash maps and five vectors per
  call. Subscription sets are small, so it now uses flat vectors reused
  across calls. 1150 ns → 660 ns for an 8-timer Sub.

**Cost.** Two more atomics (on the allowlist, with reasons). The lock-free
checks are hints: a false "empty" is corrected by the wake that came with
the message, so nothing is delayed or lost (TSan-clean).

## D28. Simulation runs background work on the loop thread

**Decision.** `sim<P>` swaps the kernel's executor for one that runs each
task body on the loop thread at `now + random latency`. One seed picks the
latencies, breaks ties between things due at the same instant, and decides
injected crashes and hangs. Stream bodies aren't run; the test feeds them.

**Why.** The bugs worth finding are ordering bugs: a result arriving after
a newer one, a timer firing between two replies. With real threads they
show up once a month in production. With the seed in charge, `explore()`
tries hundreds of orders in milliseconds and hands back one seed that
breaks, which reproduces every time and replays through `update`.

**Alternatives.**
- Real threads with random sleeps: slow, and a failure can't be replayed.
- Deterministic threads (a scheduler that hands out turns): would run
  blocking bodies, but needs every lock and wait to go through it. jaal's
  task bodies can call anything, so that can't be enforced.
- `<random>` distributions: their output differs between standard
  libraries, so a seed from a Linux CI run wouldn't reproduce on Windows.
  jaal uses splitmix64 and its own range reduction.

**Cost.** A task body that blocks (waits on its stop token, reads a
socket) hangs the sim; such work is tested by scripting its result
messages with `at()`. The kernel got one indirection: tasks go through a
virtual `executor` call. The host picks it (`make_executor`), so the
default path is still the pool and nothing in `options` changed.

**Measured.** A full run (kernel start, 3 inputs, 3 tasks, 2 invariants,
shutdown) is ~1.1 us: about 900k seeds a second on one core.
