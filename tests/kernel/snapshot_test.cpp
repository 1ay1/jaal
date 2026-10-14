// jaal::snapshot<T>: lock-free reads, CAS updates, values stay alive across
// stores, and concurrent update()s never lose an increment.
#include <jaal/kernel/snapshot.hpp>
#include <jaal/kernel/scope.hpp>

#include <atomic>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {
int failures = 0;
void ok(bool c, const char* what) {
    if (!c) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

struct Prefs {
    int width = 80;
    std::string theme = "dark";
    bool operator==(const Prefs&) const = default;
};

// Never a lock, so never a rank or a capability.
static_assert(!jaal::Sendable<jaal::snapshot<Prefs>>);
static_assert(jaal::Sync<const jaal::snapshot<Prefs>>);
}  // namespace

int main() {
    // 1. default value, store, load
    {
        jaal::snapshot<Prefs> s;
        ok(s.load()->width == 80, "1: default");
        s.store(Prefs{100, "light"});
        ok(s.get() == Prefs{100, "light"}, "1: store/get");
    }
    // 2. a held value survives later stores
    {
        jaal::snapshot<std::vector<int>> s{std::vector<int>{1, 2, 3}};
        auto held = s.load();
        s.store({9});
        ok(held->size() == 3 && s.load()->size() == 1, "2: old snapshot outlives store");
    }
    // 3. update() under contention loses nothing
    {
        jaal::snapshot<std::map<std::string, int>> s;
        jaal::thread_root([&](const jaal::unlocked& cap) {
            jaal::scope(cap, [&](jaal::nursery& n) {
                n.spawn_n(8, [&](std::size_t) {
                    for (int i = 0; i < 2000; ++i)
                        s.update([](const std::map<std::string, int>& m, std::string k) {
                            auto out = m;
                            ++out[k];
                            return out;
                        }, std::string("k"));
                }).join();
            });
        });
        ok(s.load()->at("k") == 8 * 2000, "3: concurrent update is exact");
    }
    if (failures) return 1;
    std::printf("snapshot: ok\n");
    return 0;
}
