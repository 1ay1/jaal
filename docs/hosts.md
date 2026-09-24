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

**[`examples/host.cpp`](../examples/host.cpp) is every piece below, in one
file that compiles and runs.** It's built as part of the examples, so it
can't drift from the library.

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

## 4. Drawing

```cpp
template <class K> void present(K& kernel) {
    const auto& m = kernel.model();
    if constexpr (jaal::Viewable<App, screen>) draw(App::view(m));
}
```

`present` is called after any step that changed the model, and when the loop
is otherwise idle. jaal has no opinion about what a frame is: coalescing,
damage tracking and vsync are yours.

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

## Driving it yourself

If something else owns the loop — Qt, a game engine, an existing `select` —
don't call `run`. Make the kernel and step it:

```cpp
auto k = jaal::kernel::kernel<App, event_type, jaal::platform::steady_clock>::start(
             host, {}, opts, [waker] { waker.wake(); });

while (!k.quitting()) {
    const auto turn = k.step(host);            // fold, re-subscribe
    if (turn.model_changed) draw(k.model());
    if (auto d = k.next_deadline()) wait_until(*d);   // your wait
}
return std::move(k).finish();
```

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
