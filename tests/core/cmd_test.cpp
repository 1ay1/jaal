// tests/core/cmd_test.cpp — effects, rows, Cmd, Sink, tasks.
// Static checks plus a runtime part (map, batch, task execution).

#include <jaal/core/cmd.hpp>
#include <jaal/core/core_fx.hpp>
#include <jaal/core/effect.hpp>
#include <jaal/core/fx.hpp>
#include <jaal/core/row.hpp>
#include <jaal/core/sink.hpp>

#include <chrono>
#include <memory>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

using namespace std::chrono_literals;
using jaal::Cmd;
using jaal::make_row;
using jaal::row;
using jaal::row_union;
using jaal::subrow_of;
namespace fx = jaal::fx;

// ── effects ──────────────────────────────────────────────────────────────
struct Beep { int n = 0; };
using beep = jaal::pure_fx<Beep, "beep">;
struct Title { std::string s; };
using title = jaal::pure_fx<Title, "title">;

static_assert(jaal::Effect<fx::quit>);
static_assert(jaal::Effect<fx::after>);
static_assert(jaal::Effect<fx::task>);
static_assert(jaal::Effect<fx::now>);
static_assert(jaal::Effect<beep>);
static_assert(jaal::carries_msg_v<fx::after>);
static_assert(jaal::carries_msg_v<fx::task>);
static_assert(!jaal::carries_msg_v<beep>);

// ── rows are sets ────────────────────────────────────────────────────────
static_assert(std::is_same_v<make_row<fx::quit, fx::after>, make_row<fx::after, fx::quit>>);
static_assert(std::is_same_v<make_row<fx::quit, fx::quit, fx::after>, make_row<fx::after, fx::quit>>);
static_assert(std::is_same_v<make_row<>, row<>>);
static_assert(std::is_same_v<row_union<make_row<fx::quit>, make_row<beep, fx::quit>>,
                             make_row<beep, fx::quit>>);
static_assert(jaal::core_fx::size == 4);

static_assert(subrow_of<make_row<fx::after>, jaal::core_fx>);
static_assert(subrow_of<row<>, jaal::core_fx>);
static_assert(!subrow_of<make_row<beep>, jaal::core_fx>);
static_assert(subrow_of<jaal::core_fx, row_union<jaal::core_fx, make_row<beep>>>);

// raw row<> must already be canonical (sorted, unique names)
template <class... Ds> concept valid_row = requires { typename row<Ds...>; };
static_assert(valid_row<fx::after, fx::quit>);
static_assert(!valid_row<fx::quit, fx::after>);   // not sorted
static_assert(!valid_row<fx::quit, fx::quit>);    // duplicate

// ── Cmd ─────────────────────────────────────────────────────────────────
struct Msg { int v; };
using App = row_union<jaal::core_fx, make_row<beep, title>>;
using C   = Cmd<Msg, App>;

static_assert(std::is_constructible_v<C, Beep>);
static_assert(std::is_constructible_v<C, fx::after::type<Msg>>);
static_assert(!std::is_constructible_v<Cmd<Msg, jaal::core_fx>, Beep>);   // not in row

// factories exist exactly when the effect is in the row
template <class X> concept has_after = requires { X::after(1ms, Msg{0}); };
template <class X> concept has_quit  = requires { X::quit(); };
static_assert(has_after<C> && has_quit<C>);
static_assert(!has_after<Cmd<Msg, make_row<fx::quit>>>);
static_assert(has_quit<Cmd<Msg, make_row<fx::quit>>>);

// widening yes, narrowing no
using Small = Cmd<Msg, make_row<fx::after>>;
static_assert(std::is_convertible_v<Small, C>);
static_assert(!std::is_constructible_v<Small, C>);

// ── tasks: the lifetime rules ────────────────────────────────────────────
// C::task(bad...) is DECLARED (so it can explain itself in a static_assert),
// which means a requires-expression can't see the rejection. So these
// checks test the rule directly; tests/compile_fail/cmd.cpp checks that a
// real call is rejected with the right message.
using jaal::Sink;
template <class F, class... A>
concept can_task = jaal::TaskBody<F, Msg, A...> && (jaal::Sendable<A> && ...);

auto good_body = [](Sink<Msg> out, std::stop_token, std::string s) {
    out.send(Msg{static_cast<int>(s.size())});
};
static_assert(can_task<decltype(good_body), std::string>);

void capture_checks() {
    std::string local = "x";
    int n = 0;
    auto by_ref   = [&](Sink<Msg>, std::stop_token, std::string) { (void)local; };
    auto by_value = [local](Sink<Msg>, std::stop_token, std::string) {};
    auto by_this  = [&n](Sink<Msg>, std::stop_token, std::string) { ++n; };
    static_assert(!can_task<decltype(by_ref), std::string>);
    static_assert(!can_task<decltype(by_value), std::string>);
    static_assert(!can_task<decltype(by_this), std::string>);
}

// arguments must be Sendable
using view_body = decltype([](Sink<Msg>, std::stop_token, std::string_view) {});
static_assert(!can_task<view_body, std::string_view>);
using ptr_body = decltype([](Sink<Msg>, std::stop_token, int*) {});
static_assert(!can_task<ptr_body, int*>);

// ── Sink ─────────────────────────────────────────────────────────────────
static_assert(jaal::Sendable<Sink<Msg>>);
static_assert(std::default_initializable<Sink<Msg>>);
// Can't forge one from a mailbox without the kernel's key:
static_assert(!std::is_constructible_v<Sink<Msg>,
                                       std::weak_ptr<jaal::detail::mailbox_iface<Msg>>>);

// ── runtime ──────────────────────────────────────────────────────────────
struct collect final : jaal::detail::mailbox_iface<Msg> {
    std::vector<Msg> got;
    bool post(Msg m) override { got.push_back(m); return true; }
};

struct Child { int n; };
struct Parent { Child c; };

int main() {
    // batch flattens and drops nones
    auto b = C::batch(C::none(), C::batch(C(Beep{1}), C(Beep{2})), C::quit());
    auto* bb = std::get_if<C::Batch>(&b.inner);
    if (!bb || bb->cmds.size() != 3) return 1;
    if (!C::batch(C::none(), C::none()).is_none()) return 2;
    if (!std::holds_alternative<Beep>(C::batch(C(Beep{7})).inner)) return 3;
    if (!b.contains<beep>() || !b.contains<fx::quit>() || b.contains<fx::after>()) return 4;

    // map + widen: a child's after(...) lands in the parent's row
    Cmd<Child, make_row<fx::after>> child = Cmd<Child, make_row<fx::after>>::after(5ms, Child{9});
    Cmd<Parent, App> parent = std::move(child).map([](Child c) { return Parent{c}; });
    auto* a = std::get_if<fx::after::type<Parent>>(&parent.inner);
    if (!a || a->msg.c.n != 9 || a->delay != 5ms) return 5;

    // a task runs with its owned arguments and reports through the sink
    auto box = std::make_shared<collect>();
    auto sink = jaal::sink_access::make<Msg>(
        std::weak_ptr<jaal::detail::mailbox_iface<Msg>>(box));
    auto t = C::task(good_body, std::string("hello"));
    auto* tp = std::get_if<fx::task::type<Msg>>(&t.inner);
    if (!tp) return 6;
    std::move(tp->thunk).run(sink, {});
    if (box->got.size() != 1 || box->got[0].v != 5) return 7;

    // task map: child task's Msg is mapped on the way out
    using CC = Cmd<Child, jaal::core_fx>;
    auto ct = CC::task([](Sink<Child> out, std::stop_token, int k) { out.send(Child{k}); }, 42);
    struct pcollect final : jaal::detail::mailbox_iface<Parent> {
        std::vector<Parent> got;
        bool post(Parent p) override { got.push_back(p); return true; }
    };
    auto pbox = std::make_shared<pcollect>();
    auto psink = jaal::sink_access::make<Parent>(
        std::weak_ptr<jaal::detail::mailbox_iface<Parent>>(pbox));
    auto pt = std::move(ct).map([](Child c) { return Parent{c}; });
    auto* ptp = std::get_if<fx::task::type<Parent>>(&pt.inner);
    if (!ptp) return 8;
    std::move(ptp->thunk).run(psink, {});
    if (pbox->got.size() != 1 || pbox->got[0].c.n != 42) return 9;

    // a closed sink: sends fail quietly
    Sink<Msg> closed;
    if (closed.send(Msg{1}) || closed.open()) return 10;

    // sink outliving its mailbox: no crash, send returns false
    auto s2 = [&] {
        auto tmp = std::make_shared<collect>();
        return jaal::sink_access::make<Msg>(std::weak_ptr<jaal::detail::mailbox_iface<Msg>>(tmp));
    }();
    if (s2.send(Msg{1}) || s2.open()) return 11;

    return 0;
}
