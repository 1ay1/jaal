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

**Cost.** Longer types (mitigated: `jaal::Cmd<Msg, extra...>` names only the extras), and every
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
subscription lifecycle. The core is: `quit`, `send`, `after`, `task` (pool
or isolated), `now`, `random`, `every`, `stream`, plus `on_signal` from
`run()`.

**Alternatives.** Ship common effects (HTTP, files, processes, logging) in
the core.

**Why.** Every built-in is a permanent promise that must behave the same on
every OS and in the test host. With `task` and `stream` in the core, a
watcher, a socket reader or a download is a library, not a kernel change.
`isolated_task` was merged into `task` as a placement, since it differed
only in which thread runs it. `now` was added because a task reading
`std::chrono` reads the real clock, which the test host can't control.
`random` was added for the same reason: a program drawing from its own
generator isn't reproducible, so a sim seed wouldn't reproduce a run and a
bug found by `explore()` couldn't be replayed. The kernel owns the stream
and reports the seed it used (`seed_used()`), so a real crash comes back.
`send` was added because the fold is the kernel's: the alternative,
`after(0ms, m)`, goes through the timer heap and doesn't arrive until the
next step, so every "and also do this" cost a frame and made tests wait on a
clock for something that isn't about time.

By the same rule, `debounce<T>`, `throttle`, `child<>` and `children<>` are
NOT effects. They need no clock, thread or subscription of their own, so
they're plain values and type aliases in `core/` — which means they replay,
fold under `given`, and cost nothing at runtime.

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

## D29. Composition carries the id by value, not in a capture

**Decision.** `Cmd::map` and `Sub::map` take a mapper that must be a plain
function pointer whenever the effect holds background work, because that
mapper runs on the worker or stream thread. To make a keyed LIST of children
possible, `map_with(id, f)` calls `f(id, msg)` with an owned, `Sendable` id
stored in the effect. `children<Child, Parent, Wrap>` is built on it.

**Alternatives.**
- Let `map` capture. That's the obvious fix and it's wrong: the capture
  would have to cross a thread, and `Sendable` exists precisely to stop
  that. A captured `this` or a reference into the model is a use-after-free
  the moment the child is removed.
- Make the id part of `Child::Msg`, so the child stamps its own id. Then
  every child has to know its position in its parent, which is backwards,
  and a child reused in two places needs two Msg types.
- Type-erase the mapper into a `std::function`. Allocates per effect, and
  still permits a capture that isn't `Sendable`.

**Why.** The id is data, so it can travel like data: by value, as a task
argument does. That keeps one rule ("nothing crosses a thread unless it's
`Sendable`") instead of carving an exception into it. The check is local: an
effect provides `fmap_with` or it can't be mapped with an id, and the error
says which effect is missing it.

**Cost.** Every effect that carries a `Msg` now needs `fmap_with` beside its
`fmap` — five lines for a data effect, and for `task`/`stream` a second
`static_assert` pair. A host adding its own effect only needs it if that
effect should work inside a `children<>` list.

**The bug it prevents.** Two children subscribing to the same stream key.
The reconciler keys streams by string, so unprefixed they reconcile to ONE
subscription: one child's feed silently drives the other, and closing either
stops both. `children<>` prefixes each child's keys with its id, so they
stay distinct and stop independently (`tests/core/children_test.cpp`).

## D30. Resume from a model; the journal is the app's

**Decision.** `kernel::start_from(host, model, resume_cmd)` starts a kernel
from a model the caller already has, instead of `init()`. `run<P>` takes it
through `durable<P>{resume, resume_cmd, journal}`; `headless` through the
`resume_from` tag. `replay_from(model, msgs)` folds a journal tail onto a
snapshot.

**Why it's core (D25).** Everything else a durable program needs can be
written outside jaal: the journal hook already existed (`record`), and
storage is the app's. Starting a live kernel from a given model could not:
`start()` always ran `init()`. So this is the only piece jaal adds.

**The rules.** `init()` and its Cmd are not re-run: they are outputs of the
run that produced the model, and effects are never re-run (the replay
rule). `subscribe(model)` does run, so timers, streams and routers the
model asks for are live again: subscriptions are a function of the model,
not history. Work a restart must redo is explicit, in `resume_cmd`.

**Not done.** No journal format, no serializer, no fsync policy. Those
depend on the app's storage and its Msg, and a generic codec would have to
guess at both. (`tests/kernel/resume_test.cpp`)

## D31. One program shape: per-case update, in-place model, declared Cmd

**Decision.** A program is written exactly one way:

```cpp
struct Model { ... };  using Msg = std::variant<A, B>;
using Cmd = jaal::Cmd<Msg, extra...>;   using Sub = jaal::Sub<Msg, extra...>;   // Sub optional
static Cmd init(Model&);                // optional
static Cmd update(Model&, A);  static Cmd update(Model&, B);
static Sub subscribe(const Model&);     // optional
```

Removed, because each was a second way to say the same thing:
`jaal::program<Model, Msg, fx_list<>, src_list<>>` (a base class that
generated the aliases), `step` (a second return type), by-value
`std::pair<Model, Cmd> update(Model, Msg)`, two `init` signatures,
`CoreCmd`/`CoreSub` (a third spelling of `Cmd<Msg, core_fx>`), row-taking
`Cmd<Msg, row_union<core_fx, make_row<...>>>` in programs, `overload{}`,
and `child::match` / `children::match` (routing is an update overload now).

**Why.**
- *Single source of truth.* The effect set is stated once, in `Cmd`.
  Before, it was deduced from whatever `update` returned, so a program's
  row lived in its return type and in `program<>`'s lists and in any
  `row_union` a user spelled by hand; they could disagree.
- *The compiler does the dispatch.* A `Msg` case is matched to its `update`
  by overload resolution. The hand-written `std::visit` (or chain of
  `get_if`) was the most repeated boilerplate in every program and the
  place a case got silently dropped. Now a missing case is a compile error
  that names it: `'Counter' has no update for message 'Counter::Reset';
  add: static Cmd update(Model&, Counter::Reset)`.
- *Nothing to forget to return.* With `pair<Model, Cmd>` by value, every
  branch rebuilt `{m, cmd}`; returning a stale copy of the model was a
  real bug class. In place, the model is simply what `update` left.
- *Encapsulation.* One internal namespace, `prog::init/update/subscribe`,
  is the only code that calls a program. Kernel, `given`, `replay`,
  `timeline`, `child` and `children` all go through it, so the shape can't
  drift between them. The class templates are `basic_cmd`/`basic_sub`
  (exact row, for generic code); apps see only the `Cmd`/`Sub` aliases.

**What in-place costs.** `update` taking `Model&` looks less functional.
Purity in jaal was always "no effects except the returned Cmd", which is
unchanged: `update` still can't reach the world, and replay/sim still
reproduce a run exactly. What changed is fault handling: a throwing
`update` may leave the model half-changed. `fault_policy::skip` already
copied the model first and restores it; under `stop` the program is
quitting anyway (kernel/fault.hpp). Measured by the existing fault tests,
which pass unchanged.

**Alternatives.**
- Keep both shapes. Rejected: two ways to write a program means two ways
  to read one, two sets of diagnostics, and docs that have to explain when
  to use which.
- `update(const Model&, Msg) -> pair<Model, Cmd>` (pure by signature).
  Rejected: it keeps the copy-and-return ceremony and the stale-copy bug,
  and C++ can't enforce purity through a signature anyway.
- A single `update(Model&, Msg)` with the user visiting. Rejected: it's the
  boilerplate this removes, and it can't name a missing case.

(`tests/compile_fail/kernel.cpp` cases 4 and 5 pin the two diagnostics a
newcomer hits first.)

## D32. Deadlocks and stale messages: unrepresentable, or refused at runtime

**Decision.** Every way jaal knows of to deadlock, or to deliver a message
from a stopped subscription, is either a compile error or refused by the
runtime on every call, in every build. No debug-only checks.

**The four holes, and what closed each.**

1. *A stopped stream's message reached `update` (a real bug, reproduced:
   `features_test` failed 75 runs in 300 on clang).* Two windows. (A) The
   message was still in the mailbox when the stream stopped; liveness had
   been checked by the SENDER, at send time, which can never close that
   window. (B) The message had been drained into the same fold batch as the
   message that stopped its stream: `[Rekey, Item(0)]` folded `Rekey`, then
   `Item(0)`, and only re-subscribed after the batch. Fix: every message
   carries its ORIGIN (a stream run or `every` timer gets one). The mailbox
   drops a retired origin's messages at drain; the kernel carries the
   origin to the fold, re-subscribes first if an earlier message in the
   batch changed the model, and drops the message if its subscription is
   gone. Liveness is loop state and is decided on the loop, at the last
   moment, so no thread interleaving reaches `update`. The cross-thread
   `live_streams_` set and its lock are gone. `stream_stop_test` reproduces
   both windows deterministically; the old kernel fails it.

2. *Lock-order deadlock through `guarded<T>` (was Level B, debug only).*
   `with`/`read` now take a captureless function plus Sendable arguments.
   A second `guarded` can't be captured, and can't be passed (it isn't
   Sendable; neither is a pointer, reference or `reference_wrapper`). The
   second lock has no name inside the first. Compile-fail tests pin both
   routes.

3. *Join cycle in a scope.* A helper could reach a sibling's `handle`
   through `[&]` and join it while the sibling joined back. Now only the
   thread that opened the scope may `spawn` or `join`, so the only waits
   are owner → helper: a tree, no cycle. The same rule gives the nursery's
   helper list a single writer, which removes a data race. A helper that
   tries gets `scope_misuse`, surfaced from `scope()`.

4. *A scope helper blocking on the full mailbox of the loop that joins it.*
   Only the loop drains; the loop waits for the helper. The mailbox already
   refused a blocking post from the loop thread itself; it now also refuses
   one from any thread the loop is (transitively) waiting on, tracked by
   `kernel/waits.hpp` (each helper inherits its owner's waiters plus the
   owner). Counted in `mailbox_stats::loop_full`. A thread the loop is NOT
   waiting on still gets real backpressure.

**Why runtime for 3 and 4.** C++ can't forbid a `[&]` lambda from reaching
the nursery without forbidding the borrowing `scope` exists for, and it
can't see which thread a Sink is used on. But the runtime can decide both
exactly, every time, at O(depth) cost. That's "A-runtime" in
docs/concurrency.md §2: the program compiles, the bad outcome can't happen.

**Measured.** `features_test` + `stream_stop_test`, 200 runs each on clang
and tsan: 0 failures (was 75/300 on clang, ~1/40 on tsan).

## D33. `registration::modify(interest)` is part of the Reactor concept

**Decision.** A registration can change what its handle waits for, keeping
its token: `reg.modify(interest::read_write)`. It's in the `Reactor`
concept, so every backend has it and the conformance suite holds all four to
the same behaviour.

**Why.** Found by writing a real socket host (`examples/server.cpp`). A
write to a socket takes what fits and returns `EAGAIN`; the host must then
wait for writability, and stop waiting once its buffer drains. Without
`modify` the only way is to drop the registration and watch again, which:

- costs two syscalls instead of one, per direction change, per connection;
- leaves the handle unwatched in between, so readiness arriving in the gap
  is lost;
- forces the host to re-derive the token and re-insert into its own map.

Every non-trivial socket host hits this on its first partial write, so it
belongs in the concept rather than in each host.

**On the registration, not the reactor.** The registration already owns the
handle's place in the reactor (its slot, its token, its lifetime). Putting
`modify` on the reactor would mean naming the handle again, and would let a
host modify a registration it doesn't own.

**Per backend.** kqueue: delete the filters no longer wanted, then add the
new ones (delete first, so a failed add can't leave the old filter live).
epoll: one `EPOLL_CTL_MOD`, same `data.u64`. poll: the pollfd array is built
per `wait()`, so it's a field update. Windows
(`WaitForMultipleObjects`): there is no read/write interest, so it validates
the registration and succeeds, changing nothing — host code that adds write
interest on `EAGAIN` stays portable and simply keeps being woken.

**The trap it introduces, and the test for it.** An idle socket is always
writable, so a host that adds write interest and forgets to remove it spins
at 100% CPU. That's a symptom nobody notices in a unit test, so
`tests/platform/modify_test.cpp` fills a real socket buffer, checks
writability is reported after `modify`, and then checks the reactor goes
QUIET after modifying back — plus that pending readability survives a
modify (which a drop-and-re-watch can lose).

## D34. Signal handlers come off before shutdown

**Decision.** `run()` drops its signal source when the loop ends, *before*
`host.release()` and `kernel::finish()`. From then on every signal has the
disposition it had before jaal started, so a second Ctrl+C kills the process
the ordinary way.

**The bug.** Handlers used to stay installed for the whole of `run()`,
shutdown included. Shutdown is not instant: `finish()` waits up to
`options::shutdown_grace` (2 s by default) for workers, and a task that
ignores its stop token holds it for all of it. During that window the
handler was still catching SIGINT and writing it into a pipe that nothing
drained any more. Measured: 20 SIGINTs over a 10 s shutdown, every one
swallowed, with the process unkillable by ^C throughout.

That contradicted the promise at the top of `kernel/run.hpp` — "a program
that doesn't subscribe to interrupt/terminate/hangup stops with 128 + signo,
so it's never left un-killable because it forgot to ask". A program that DID
subscribe was left un-killable during its own exit, which is exactly when a
user reaches for ^C a second time.

**Why this is the right split.** While the loop runs, a signal is the
program's to interpret (D17) — that's what lets an editor ask "save before
quitting?" on ^C. Once the loop has ended, there is no program left to ask:
the decision to exit has been made, and the only meaning a further ^C can
have is "stop waiting". Handing it back to the OS is both simpler and what
every other program does.

**Test.** `tests/kernel/signal_shutdown_test.cpp` forks a program whose task
never stops, waits for `release()` to announce that shutdown has begun (so
the test isn't racing a window), sends a second SIGINT, and requires the
child to die *by the signal* (`WIFSIGNALED`, `WTERMSIG == SIGINT`) rather
than exit tidily. It fails deterministically against the old code.

**Still true afterwards.** An inherited `SIG_IGN` (nohup, a background job
in a non-interactive shell) stays ignored — `release()` restores the
*previous* disposition, not the default, so jaal never makes a program more
killable than it was when it started.
