// tests/kernel/timeline_test.cpp — diff, timeline, bisect, and the sim → timeline path.

#include <jaal/jaal.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <variant>
#include <vector>

using namespace std::chrono_literals;
using jaal::Sink;

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,   \
                         __LINE__, #c);                                   \
            return 1;                                                     \
        }                                                                 \
    } while (0)

namespace {

// ── diff ─────────────────────────────────────────────────────────────────
struct Opaque { int x; };                                // no ==, a struct: walked
struct NoEq { NoEq() = default; NoEq(const NoEq&) = default; private: [[maybe_unused]] int x = 0; };  // opaque
struct Inner { bool on = false; double level = 0; bool operator==(const Inner&) const = default; };
struct Model {
    int         n = 0;
    std::string name;
    Inner       in;
    Opaque      op{};
    std::vector<int> v;
};

int diff_tests() {
    Model a{1, "x", {false, 0.5}, {3}, {1, 2}};
    Model b = a;
    CHECK(jaal::diff(a, b).empty());

    b.n = 2;
    b.in.on = true;
    b.op.x = 4;
    auto d = jaal::diff(a, b);
    CHECK(d.size() == 3);
    CHECK((d[0] == jaal::field_change{".0", "1", "2"}));
    CHECK((d[1] == jaal::field_change{".2.0", "false", "true"}));   // Inner has == but no formatter: walked
    CHECK((d[2] == jaal::field_change{".3.0", "3", "4"}));

    Model c = a;
    c.name = "hello";
    c.v.push_back(3);
    d = jaal::diff(a, c);
    CHECK(d.size() == 2);
    CHECK(d[0].path == ".1" && d[0].before == "\"x\"" && d[0].after == "\"hello\"");
    CHECK(d[1].path == ".4" && d[1].after == "[1, 2, 3]");

    // Top-level scalar.
    d = jaal::diff(5, 6);
    CHECK(d.size() == 1 && d[0].path.empty());
    CHECK(jaal::to_string(d) == "(value): 5 -> 6\n");

    // Long values are cut.
    jaal::diff_options o; o.max_text = 4;
    d = jaal::diff(std::string("abcdefgh"), std::string("x"), o);
    CHECK(d[0].before == "\"abc...");

    // Opaque: silent unless asked.
    CHECK(jaal::diff(NoEq{}, NoEq{}).empty());
    o = {}; o.report_opaque = true;
    CHECK(jaal::diff(NoEq{}, NoEq{}, o).size() == 1);
    return 0;
}

// ── timeline ─────────────────────────────────────────────────────────────
struct Bank {
    struct Model { int balance = 0; int ops = 0; bool operator==(const Model&) const = default; };
    struct Deposit { int n; }; struct Withdraw { int n; };
    using Msg = std::variant<Deposit, Withdraw>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model& m, Deposit d) {
        ++m.ops;
        m.balance += d.n;
        return {};
    }
    static Cmd update(Model& m, Withdraw w) {
        ++m.ops;
        m.balance -= w.n;                                   // bug: no overdraft check
        return {};
    }
};

int timeline_tests() {
    std::vector<Bank::Msg> log;
    for (int i = 0; i < 500; ++i) log.push_back(Bank::Deposit{1});
    log.push_back(Bank::Withdraw{600});                    // step 501: balance -100
    for (int i = 0; i < 300; ++i) log.push_back(Bank::Deposit{1});   // recovers at step 601

    jaal::timeline<Bank> t(log, 16);
    CHECK(t.size() == 802);
    CHECK(t.at(0).balance == 0);
    CHECK(t.at(500).balance == 500);
    CHECK(t.at(501).balance == -100);
    CHECK(t.at(801).balance == 200);
    // Back and forth: same answers (snapshots + cache agree with a replay).
    CHECK(t.at(17).balance == 17);
    CHECK(t.at(3).balance == 3);
    CHECK(t.at(4).balance == 4);
    for (std::size_t s : {0u, 1u, 15u, 16u, 17u, 500u, 501u, 777u, 801u, 2u}) {
        std::vector<Bank::Msg> prefix(log.begin(), log.begin() + static_cast<long>(s));
        CHECK(t.at(s) == jaal::replay<Bank>(prefix));
    }

    CHECK(std::holds_alternative<Bank::Withdraw>(t.message(501)));
    auto ch = t.changes(501);
    CHECK(ch.size() == 2);
    CHECK((ch[0] == jaal::field_change{".0", "500", "-100"}));
    CHECK(t.changes(0).empty());
    CHECK(t.changes(0, 801).size() == 2);

    auto good = [](const Bank::Model& m) { return m.balance >= 0; };
    CHECK(t.first_bad(good) == 501u);
    CHECK(!t.first_bad([](const Bank::Model&) { return true; }));

    // Monotone property: "ever had more than 250 ops".
    auto small = [](const Bank::Model& m) { return m.ops <= 250; };
    CHECK(t.bisect(small) == 251u);
    CHECK(t.bisect(small) == t.first_bad(small));

    bool threw = false;
    try { (void)t.at(802); } catch (const std::out_of_range&) { threw = true; }
    CHECK(threw);
    return 0;
}

// ── sim failure → timeline ───────────────────────────────────────────────
struct Race {
    struct Model { int query = 0; int shown = 0; };
    struct Type { int q; }; struct Results { int q; };
    using Msg = std::variant<Type, Results>;
    using Cmd = jaal::Cmd<Msg>;
    static Cmd update(Model& m, Type t) {
        m.query = t.q;
        return Cmd::task([](Sink<Msg> out, std::stop_token, int q) {
                             out.send(Results{q});
                         }, t.q);
    }
    static Cmd update(Model& m, Results r) {
        m.shown = r.q;
        return {};
    }
};

int sim_to_timeline() {
    auto r = jaal::explore<Race>(1, 200, {}, [](jaal::sim<Race>& s) {
        s.at(0ms, Race::Type{1});
        s.at(1ms, Race::Type{2});
        s.check("fresh", [](const Race::Model& m) { return m.shown == 0 || m.shown == m.query
                                                       || m.shown > m.query; });
    });
    CHECK(!r.ok());
    jaal::timeline<Race> t(r.failure->messages);
    auto bad = t.first_bad([](const Race::Model& m) {
        return m.shown == 0 || m.shown >= m.query;
    });
    CHECK(bad.has_value());
    CHECK(*bad == t.size() - 1);                     // the sim stopped right at it
    CHECK(std::holds_alternative<Race::Results>(t.message(*bad)));
    auto ch = t.changes(*bad);
    CHECK(ch.size() == 1 && ch[0].path == ".1");
    std::printf("  step %zu: %s", *bad, jaal::to_string(ch).c_str());
    return 0;
}

}  // namespace

int main() {
    if (int r = diff_tests())      return r;
    if (int r = timeline_tests())  return r;
    if (int r = sim_to_timeline()) return r;
    std::puts("timeline: ok");
    return 0;
}
