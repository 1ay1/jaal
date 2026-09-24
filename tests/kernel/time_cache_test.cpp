// tests/kernel/time_cache_test.cpp — "now" is read once per step, and a
// host event is its own instant.
//
// The kernel reads the clock at most once per step and shares that reading
// with everything the step arms. It's a real saving (steady_clock::now() is
// ~16 ns on macOS, more than folding a message) and it gives every timer in
// one step the same "now". But a cache can go stale, and this is the way it
// does: step() caches T0, the program sits idle, the clock moves, and a
// keystroke arrives through route() — which is NOT step(). If route() reused
// the cached T0, `after(10ms)` would get a deadline in the past and fire at
// once. So route() starts a new instant too, and this test pins it: it
// fails, with the timer firing early, if route() forgets to reset.
//
// Getting the cache populated is the subtle part: step() only reads the
// clock when a timer is armed, so the program keeps one alive.
#include <jaal/jaal.hpp>
#include <cstdio>
using namespace std::chrono_literals;
struct Key { char c; };
using on_key = jaal::router<Key, "on_key">;
struct App {
    struct Model { int fired = 0; };
    struct Pressed {}; struct Fire {}; struct Keepalive {};
    using Msg = std::variant<Pressed, Fire, Keepalive>;
    using Cmd = jaal::Cmd<Msg>;
    using Sub = jaal::Sub<Msg, on_key>;
    static Cmd init(Model&) { return Cmd::after(1h, Keepalive{}); }   // a timer: step reads the clock
    static Cmd update(Model&, Pressed) { return Cmd::after(10ms, Fire{}); }
    static Cmd update(Model& m, Fire) { ++m.fired; return {}; }
    static Cmd update(Model&, Keepalive) { return {}; }
    static Sub subscribe(const Model&) {
        return Sub::on(on_key{}, [](const Key&) -> std::optional<Msg> { return Pressed{}; });
    }
};
int main() {
  jaal::headless<App, Key> h;
  auto& k = h.kernel();
  k.step(h.record());                       // timers non-empty: caches now = T0
  k.clock().advance(5s);                    // clock moves, NO step
  (void)k.route(Key{'x'}, h.record());      // arms after(10ms) from cached time
  k.step(h.record());
  const bool early = h.model().fired != 0;
  std::printf("fired early (stale now)? %s\n", early ? "YES - BUG" : "no");
  return early ? 1 : 0;
}
