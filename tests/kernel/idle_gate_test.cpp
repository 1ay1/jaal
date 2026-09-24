// tests/kernel/idle_gate_test.cpp — the idle fast path never skips work.
//
// step() starts with one question: is there anything to do anywhere? If
// not, it returns before reaching any phase. That took an idle step from
// 7.4 to 2.3 ns, and an idle step is most of what an interactive loop does.
//
// A fast path is only safe if its condition is EXACTLY "no phase below would
// do work". Get one clause wrong and a message sits unfolded, or a timer
// never fires, with nothing to show for it. So each case here puts work in
// exactly ONE of the places the gate checks, and asserts the step does it:
// a message from another thread, a due timer, a timer not yet due (must NOT
// fire), a local dispatch — and tracing, which promises a step event for
// every step and so switches the fast path off.
#include <jaal/jaal.hpp>
#include <cstdio>
#include <thread>
using namespace std::chrono_literals;
#define CHECK(c) do{ if(!(c)){ std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); return 1; } }while(0)
struct C {
    struct Model { int n=0; int ticks=0; bool live=false; };
    struct Inc{}; struct Tick{}; struct Go{};
    using Msg=std::variant<Inc,Tick,Go>; using Cmd=jaal::Cmd<Msg>; using Sub=jaal::Sub<Msg>;
    static Cmd update(Model& m, Inc){ ++m.n; return {}; }
    static Cmd update(Model& m, Tick){ ++m.ticks; return {}; }
    static Cmd update(Model& m, Go){ m.live=true; return {}; }
    static Sub subscribe(const Model& m){ return m.live ? Sub::every(10ms, Tick{}) : Sub::none(); }
};
int main(){
  // 1. mailbox: a message from another thread, no dispatch, no timer
  { jaal::headless<C> h; auto& k=h.kernel(); auto s=k.sink();
    std::thread t([s]{ s.send(C::Inc{}); }); t.join();
    k.step(h.record());
    CHECK(h.model().n==1); }
  // 2. a due timer, nothing else pending
  { jaal::headless<C> h; auto& k=h.kernel();
    h.send(C::Go{});                  // subscribes the 10ms timer
    k.clock().advance(15ms);          // due, but nothing queued
    k.step(h.record());
    CHECK(h.model().ticks==1); }
  // 3. a timer NOT yet due must not fire
  { jaal::headless<C> h; auto& k=h.kernel();
    h.send(C::Go{});
    k.clock().advance(3ms);
    k.step(h.record());
    CHECK(h.model().ticks==0); }
  // 4. a pending dispatch
  { jaal::headless<C> h; auto& k=h.kernel();
    k.dispatch(C::Inc{}); k.step(h.record());
    CHECK(h.model().n==1); }
  // 5. tracing still sees idle steps
  { int steps=0; jaal::kernel::options opt;
    opt.trace=[&](const jaal::trace_event& e){ if(e.kind==jaal::trace_kind::step) ++steps; };
    jaal::headless<C> h(opt); auto& k=h.kernel();
    const int before=steps;
    for (int i=0;i<5;++i) k.step(h.record());
    CHECK(steps-before==5); }
  std::puts("gate: all 5 cases correct"); return 0;
}
