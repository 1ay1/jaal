# Writing a host

A **host** is the thing that gives a program eyes, hands and a screen. jaal
runs the loop; the host owns whatever the OS calls a window, a terminal or a
socket, turns what arrives into events, and draws when the model changes.

maya is a host. So is a GUI toolkit binding, an ACP server, or a test
harness. This is the guide to writing one.

It assumes you've read the tour in the [README](../README.md). The protocol
itself is documented at the top of
[`kernel/run.hpp`](../include/jaal/kernel/run.hpp); this page is the worked
version, in the order you'll actually need it.

Two worked hosts, both built as part of the examples so they can't drift
from the library:

- **[`examples/host.cpp`](../examples/host.cpp)** — every piece below in one
  small file: one input handle, a router, one effect of its own.
- **[`examples/server.cpp`](../examples/server.cpp)** — the other shape: a
  TCP server with many handles coming and going, line framing, and writes
  that block (§3b).

## The shape of it

Everything is optional except `event_type`. A host is a plain struct: no base
class, no virtuals, no registration. jaal detects each member with a
`requires`, so you add only what you need and the rest costs nothing.

```cpp
struct my_host {
    using event_type = std::variant<Key, Resize>;   // what routers see

    void attach(jaal::host_context<my_host>& cx);    // register OS handles
    void on_ready(jaal::host_context<my_host>& cx, const jaal::readiness& r);
    template <class K> void present(K& kernel);      // draw
    void release();                                  // restore the terminal
};

int main() { my_host h; return jaal::run<App>(h); }
```

`run<P>(host)` then drives this loop:

1. start the kernel (runs `init` and its `Cmd`)
2. `attach`: you register your handles with the reactor
3. wait for a deadline, a wake, a signal, or one of your handles
4. `on_ready`: read the handle, `emit` events; they route through `subscribe`
5. `present`: draw, if the model changed
6. repeat until quit, then `release` and `finish`

The kernel never owns the thread ([D4](decisions.md#d4-the-kernel-never-owns-the-thread)),
so if you already have a loop — Qt, GLFW, someone else's `select` — you can
drive `kernel::step()` yourself instead of calling `run`. See
[Driving it yourself](#driving-it-yourself).

## 1. Events

`event_type` is what your host produces and what the program's routers
consume. Use a `std::variant` when you have several kinds:

```cpp
struct Key    { char32_t ch; bool ctrl; };
struct Resize { int cols, rows; };
using event_type = std::variant<Key, Resize>;
```

A program doesn't subscribe to these directly — it subscribes to a **router**
you define, which turns an event into an optional message. That's the piece
that makes a host's input typed:

```cpp
// In your host's header, next to the events.
struct on_key {
    static constexpr std::string_view name = "on_key";
    using event_type = Key;                        // which event it reads

    template <class Msg> struct type {
        std::function<std::optional<Msg>(Key)> f;
    };
    template <class F, class M>
    static auto fmap(F&& f, type<M> e) -> type<std::invoke_result_t<F, M>> {
        using B = std::invoke_result_t<F, M>;
        return {[g = std::move(e.f), f = std::forward<F>(f)](Key k) -> std::optional<B> {
            if (auto m = g(k)) return f(std::move(*m));
            return std::nullopt;
        }};
    }
    /// Called on the loop thread with each event; nullopt = not for me.
    template <class M>
    static std::optional<M> route(const type<M>& p, const Key& k) { return p.f(k); }

    template <class Self, class Msg> struct ctors {
        template <class F>
        [[nodiscard]] static Self on_key(F f) { return Self(type<Msg>{std::move(f)}); }
    };
};
```

Now a program writes `Sub::on_key(...)` and it only compiles on a host that
produces `Key` events. The program lists the source in its `Sub`:

```cpp
struct App {
    // ... Model, Msg, Cmd, update ...
    using Sub = jaal::Sub<Msg, on_key>;            // core sources + on_key

    static Sub subscribe(const Model&) {
        return Sub::on_key([](Key k) -> std::optional<Msg> {
            if (k.ch == 'q') return Msg{Quit{}};
            return std::nullopt;                   // ignored, not an error
        });
    }
};
```

`on_signal` is a source like any other, so a program that also wants
signals lists it too: `jaal::Sub<Msg, on_key, jaal::fx::on_signal>`.

Routers are rebuilt on every `subscribe()`, so they may capture the model
freely ([D13](decisions.md#d13-re-subscribe-between-events)). They run on the
loop thread, so they need not be `Sendable`.

If a router's `event_type` isn't one your host produces, the program won't
compile against your host, and the error names the source. That's the point.

## 2. Attaching handles

`attach` is where you hand the reactor whatever the OS lets you wait on: a
tty fd, an inotify fd, an X11 connection, a socket. **Keep the
registrations** — dropping one unwatches the handle.

```cpp
void attach(jaal::host_context<my_host>& cx) {
    if (auto r = cx.watch(tty_fd_, jaal::interest::read, kTty))
        tty_reg_ = std::move(*r);
    else
        cx.stop(70);                    // can't read input: nothing to do
}
```

The token (`kTty`) is yours; it comes back in `on_ready` so you know which
handle woke up. Host tokens can't collide with the driver's.

## 3. Reading and emitting

```cpp
void on_ready(jaal::host_context<my_host>& cx, const jaal::readiness& r) {
    if (r.token != kTty) return;
    if (r.hung_up()) { cx.stop(0); return; }        // the terminal went away

    char buf[512];
    const auto n = ::read(tty_fd_, buf, sizeof buf);
    if (n <= 0) return;
    for (auto ev : parse(buf, n)) cx.emit(ev);      // one event at a time
}
```

`emit` routes the event **now**, then re-subscribes before the next one, so a
program that changes its subscriptions in response to a keystroke has them in
effect for the very next keystroke. That was a real bug in maya, and it's why
the rule exists ([D13](decisions.md#d13-re-subscribe-between-events)).

Readiness is level-triggered ([D15](decisions.md#d15-level-triggered-readiness)):
if you don't drain the fd, you'll be told again. That's deliberate — a partial
read is not a lost event.

## 3b. Writes that don't finish

Reading is the easy direction. A write to a socket takes as much as the send
buffer has room for and then returns `EAGAIN`, so a host that writes has to
keep the leftovers and finish later. Two things belong to the host, not the
program:

- an **output buffer** per handle, holding what the socket wouldn't take
- a **write interest** that follows whether that buffer is empty

`registration::modify(interest)` is the second one. It changes what a handle
waits for, keeping its token:

```cpp
void flush(conn& c) {
    while (!c.out.empty()) {
        const auto n = ::write(c.fd, c.out.data(), c.out.size());
        if (n > 0) { c.out.erase(0, std::size_t(n)); continue; }
        if (n < 0 && errno == EINTR) continue;
        break;                                  // EAGAIN: the rest waits
    }
    const bool want_write = !c.out.empty();
    if (want_write == c.watching_write) return;  // interest already right
    const auto what = want_write ? jaal::interest::read_write : jaal::interest::read;
    if (c.reg.modify(what)) c.watching_write = want_write;
}
```

Then `on_ready` calls `flush` whenever `r.writable`, and the program's write
effect appends to `c.out` and calls `flush` too. The program never learns
that a write blocked; it returns a `reply` effect and the host decides when
the bytes go out.

**Take the write interest away again.** Leaving it on looks harmless — you
just get extra wakeups — but an idle socket is *always* writable, so the
reactor reports it every time round the loop and the program spins at 100%
CPU doing nothing. That's what the second half of
`tests/platform/modify_test.cpp` checks.

Why `modify` rather than dropping the registration and re-watching: the
re-watch costs two syscalls instead of one, and between them the handle is
unwatched, so readiness that arrives in the gap is lost. `modify` is in the
`Reactor` concept, so all four backends provide it and the conformance suite
holds them to the same behaviour (on Windows, where
`WaitForMultipleObjects` has no read/write interest, it succeeds and changes
nothing — host code stays portable).

**[`examples/server.cpp`](../examples/server.cpp)** is a full TCP line-echo
server built this way: one registration per connection, line framing in the
host, and this exact backpressure. Send it `flood` and it writes a megabyte
through a socket that blocks halfway.

## 4. Drawing

```cpp
template <class K> void present(K& kernel) {
    const auto& m = kernel.model();
    if constexpr (jaal::Viewable<App, screen>) draw(App::view(m));
}
```

`present` is called after any step that changed the model. jaal has no
opinion about what a frame is: damage tracking and vsync are yours.

To cap how often it's called, set `run_options::min_present_interval`
(16ms is 60 frames a second). A frame that comes too soon isn't dropped:
it's owed, and drawn when the gap is up, with whatever the model is by
then ([D40](decisions.md#d40-drawing-can-be-paced-and-a-paced-frame-is-owed-not-dropped)).

Two optional hooks on the *program* help a drawing host:

- `visual_hash(model)` — skip `view()` when the hash is unchanged
- `needs_warmup(model)` — render once off-screen to warm caches

Both are detected with `requires`, so a program that doesn't have them
simply doesn't pay for them.

## 5. Your own effects

A host that can do something the kernel can't should say so as an effect,
not as a method the program calls directly. Declare a descriptor, and
programs get a factory named after it:

```cpp
// Effects with no Msg inside are one line.
struct Bell {};
using ring_bell = jaal::pure_fx<Bell, "ring_bell">;

// ...and the host handles it. The kernel calls handle(payload) for every
// non-core effect in the program's row.
void handle(Bell) { write("\a"); }
```

If a program returns an effect your host can't handle, it fails to compile
with the effect named:

```
error: static assertion failed: jaal: host 'my_host' cannot run effect
       'ring_bell' that program 'App' can return; add handle() for it
```

That check is `HostFor<H, P>`, and it's what keeps "works on the terminal,
crashes on the web" from being possible.

**An effect can answer.** Some effects have to run on the loop thread and
produce a result, for example handing the tty to an interactive child
(`sudo`, `$EDITOR`) and reporting how it exited. Return the Msg from
`handle()`, or a `std::optional<Msg>` when there may be no answer:

```cpp
struct RunChild { std::string cmd; };
using run_child = jaal::pure_fx<RunChild, "run_child">;

std::optional<Msg> handle(RunChild r) {
    const int code = suspend_and_run(r.cmd);      // blocks: the user is in the child
    return ChildExited{code};
}
```

The answer is folded in the same step, in the order the effects were
returned, just like `Cmd::send`. The effect is still a value; there's no
callback for the program to capture. Returning anything that isn't the
program's Msg is a compile error that names the rule. (D39)

Long-running host work (watching a directory, a socket pump) is a **source**
instead, with `start_source`/`stop_source`, so the reconciler starts and
stops it as the program subscribes and unsubscribes.

## 6. Teardown

```cpp
void release() { restore_terminal(); }
```

Called before the kernel finishes, so the screen is sane before any shutdown
message is printed. Shutdown is bounded
([D20](decisions.md#d20-shutdown-is-bounded)): a worker still running after
the grace period is abandoned and reported, not waited on forever.

The order once the loop ends is fixed, and it's a destructor
(`kernel/teardown.hpp`), not a convention:

1. **signal handlers come off** ([D34](decisions.md#d34-signal-handlers-come-off-before-shutdown)),
   restoring whatever disposition each signal had before jaal started — so a
   second Ctrl+C during a slow shutdown kills the process instead of being
   queued for a loop that has stopped reading
2. `release()` — your turn: restore the terminal, close sockets, drop
   registrations
3. `finish()` — stop timers and sources, ask every task to stop, join
   workers within the grace, close the mailbox

So `release()` can assume the loop is over and no more events will arrive,
and it must not assume any task has finished yet. If it throws, the throw is
swallowed and step 3 still runs: a wedged worker must not outlive the
process's last chance to stop it.

## Driving it yourself

If something else owns the loop — Qt, a game engine, an existing `select` —
don't call `run`. Make the kernel and step it:

```cpp
auto k = jaal::kernel::kernel<App, event_type, jaal::platform::steady_clock>::start(
             host, {}, opts, [waker] { waker.wake(); });

// Shutdown order, owned by a destructor: signals off, host.release(),
// kernel.finish(). Declare it and forget it — it runs on every path out of
// the scope, including an exception from your own code.
jaal::kernel::teardown guard{k, host, std::move(sigs)};   // or kernel::no_signals{}

while (!k.quitting()) {
    const auto turn = k.step(host);            // fold, re-subscribe
    if (turn.model_changed) draw(k.model());
    if (auto d = k.next_deadline()) wait_until(*d);   // your wait
}
return guard.exit_code();
```

There's no way to do this in the wrong order: `kernel::finish()` takes a key
only `teardown` can make, so `std::move(k).finish()` doesn't compile
([D35](decisions.md#d35-shutdown-order-is-a-destructor-not-a-convention)).
If you forget `exit_code()`, the destructor still shuts everything down in
order; you just don't see the code.

`step()` folds at most `fold_budget` messages, so a message storm can't
starve your input or your frames. `next_deadline()` tells you how long you
may sleep; honour it or timers fire late.

The kernel is not movable ([D5](decisions.md#d5-the-kernel-is-not-movable)):
make it where it lives.

## Sending messages from outside

A host thread, a callback from a C library, an incoming RPC: get a `Sink` and
send.

```cpp
jaal::Sink<App::Msg> sink = k.sink();
// ...from any thread:
sink.send(App::Msg{Incoming{payload}});
```

A `Sink` is weak by type ([D10](decisions.md#d10-sink-is-weak-by-type)): it
does not keep the kernel alive, and `send` returns `false` once the loop is
gone. So a late callback after shutdown is a `false`, not a crash. The `Msg`
must be `Sendable`, which is checked when the `Sink` is instantiated.

## Testing a host

Don't start by testing it end to end. In order of cost:

| what | how |
|---|---|
| the program's logic | `given<App>` — `update` as data, no kernel, no clock |
| effects it asked for | `headless<App>` — a real kernel, a fake clock, effects recorded |
| ordering and races | `sim<App>` + `explore()` — one seed controls everything |
| your reactor use | the conformance suite in `tests/platform/conformance.cpp` |

A host's own parsing (bytes to events) is ordinary code and deserves
ordinary unit tests. What's worth testing through jaal is the part that
touches subscriptions: that closing a pane stops its stream, that a resize
mid-keystroke doesn't lose the keystroke.

## Checklist

- [ ] `event_type` names every kind of input you produce
- [ ] a router per event kind, each with `name`, `event_type`, `type<Msg>`,
      `fmap`, `route`, `ctors`
- [ ] `attach` keeps its registrations
- [ ] `on_ready` drains the handle and checks `hung_up()`
- [ ] `release` restores global state, even when the program quit by signal
- [ ] every host effect has a `handle()`, and long work is a source
- [ ] a program that asks for something you can't do fails to **compile**
