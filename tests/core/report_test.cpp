// tests/core/report_test.cpp — a child tells its parent something (D41).
//
// What must hold:
//   * a child's Cmd::report(...) reaches the parent as the From message, in
//     the same step, with the child's state as of the report
//   * the child's own effects still work next to a report (batch)
//   * with children<>, the parent learns WHICH child reported
//   * a child can report only what its Out lists; a child that can report
//     can't be embedded without a From (compile_fail covers both)
//   * reports survive being nested two levels deep
#include <jaal/core/child.hpp>
#include <jaal/core/children.hpp>
#include <jaal/host.hpp>
#include <jaal/jaal.hpp>

#include <cstdio>
#include <string>
#include <variant>
#include <vector>

#define CHECK(c)                                                                    \
    do {                                                                            \
        if (!(c)) {                                                                 \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #c); \
            return 1;                                                               \
        }                                                                           \
    } while (0)

namespace {

// A child that reports. Saving is an internal event; Saved is what it tells
// whoever embeds it.
struct Editor {
    struct Model {
        int saves = 0;
        std::string text;
        bool operator==(const Model&) const = default;
    };
    struct Type { char c; };
    struct Save {};
    using Msg = std::variant<Type, Save>;

    struct Saved { int count; std::string text; };
    struct Emptied {};
    using Out = std::variant<Saved, Emptied>;

    using Cmd = jaal::Cmd<Msg, jaal::fx::report<Out>>;

    static Cmd update(Model& m, Type t) {
        m.text += t.c;
        return {};
    }
    static Cmd update(Model& m, Save) {
        ++m.saves;
        if (m.text.empty()) return Cmd::report(Emptied{});
        // A report next to another effect: both happen.
        return Cmd::batch(Cmd::report(Saved{m.saves, m.text}), Cmd::send(Type{'!'}));
    }
};

// A parent with one editor.
struct App {
    struct ToEditor { Editor::Msg msg; };
    struct FromEditor { Editor::Out out; };
    struct Go {};
    using Msg = std::variant<ToEditor, FromEditor, Go>;
    using Ed = jaal::child<Editor, App, ToEditor, FromEditor>;
    using Cmd = jaal::Cmd<Msg>;

    struct Model {
        Editor::Model editor;
        std::vector<std::string> heard;
        int editor_saves_when_heard = -1;
        bool operator==(const Model&) const = default;
    };

    static Cmd update(Model& m, ToEditor t) { return Ed::update(m.editor, std::move(t)); }
    static Cmd update(Model& m, FromEditor f) {
        std::visit(
            [&]<class R>(const R& r) {
                if constexpr (std::same_as<R, Editor::Saved>) {
                    m.heard.push_back("saved " + std::to_string(r.count) + " " + r.text);
                    m.editor_saves_when_heard = m.editor.saves;
                } else {
                    m.heard.push_back("emptied");
                }
            },
            f.out);
        return {};
    }
    static Cmd update(Model&, Go) { return {}; }
};

int child_reports_reach_the_parent() {
    jaal::given<App> t;
    t.when(App::ToEditor{Editor::Type{'h'}})
        .when(App::ToEditor{Editor::Type{'i'}})
        .when(App::ToEditor{Editor::Save{}});
    // The report is a send of FromEditor, returned by the save's step.
    t.settle();
    CHECK(t.ok());
    const auto& m = t.model();
    CHECK(m.heard.size() == 1);
    CHECK(m.heard[0] == "saved 1 hi");
    // The parent saw the child as of the report: the save had happened.
    CHECK(m.editor_saves_when_heard == 1);
    // And the child's own effect next to the report ran too.
    CHECK(m.editor.text == "hi!");
    return 0;
}

int every_kind_the_child_lists_arrives() {
    jaal::given<App> t;
    t.when(App::ToEditor{Editor::Save{}});   // nothing typed: Emptied
    t.settle();
    CHECK(t.model().heard == std::vector<std::string>{"emptied"});
    return 0;
}

// A parent with a keyed list of editors: it must know WHICH one reported.
struct Tabs {
    struct ToTab { int id; Editor::Msg msg; };
    struct FromTab { int id; Editor::Out out; };
    struct Open {};
    using Msg = std::variant<ToTab, FromTab, Open>;
    using Kids = jaal::children<Editor, Tabs, ToTab, FromTab>;
    using Cmd = jaal::Cmd<Msg>;

    struct Model {
        Kids::map tabs;
        std::vector<int> saved_by;
        bool operator==(const Model&) const = default;
    };

    static Cmd update(Model& m, ToTab t) { return Kids::update(m.tabs, std::move(t)); }
    static Cmd update(Model& m, FromTab f) {
        if (std::holds_alternative<Editor::Saved>(f.out)) m.saved_by.push_back(f.id);
        return {};
    }
    static Cmd update(Model& m, Open) { return Kids::add(m.tabs).second; }
};

int children_say_which_one() {
    jaal::given<Tabs> t;
    t.when(Tabs::Open{}).when(Tabs::Open{}).when(Tabs::Open{});
    const auto ids = [&] {
        std::vector<int> v;
        for (const auto& [id, _] : t.model().tabs) v.push_back(id);
        return v;
    }();
    CHECK(ids.size() == 3);
    t.when(Tabs::ToTab{ids[2], Editor::Type{'x'}}).when(Tabs::ToTab{ids[2], Editor::Save{}});
    t.settle();
    t.when(Tabs::ToTab{ids[0], Editor::Type{'y'}}).when(Tabs::ToTab{ids[0], Editor::Save{}});
    t.settle();
    CHECK(t.model().saved_by == (std::vector<int>{ids[2], ids[0]}));
    return 0;
}

// Two levels: Root -> App -> Editor. The editor reports to App; App reports
// to Root in turn, only when it chooses to.
struct AppReporting {
    struct ToEditor { Editor::Msg msg; };
    struct FromEditor { Editor::Out out; };
    using Msg = std::variant<ToEditor, FromEditor>;
    struct Dirty {};
    using Out = std::variant<Dirty>;
    using Ed = jaal::child<Editor, AppReporting, ToEditor, FromEditor>;
    using Cmd = jaal::Cmd<Msg, jaal::fx::report<Out>>;
    struct Model {
        Editor::Model editor;
        bool operator==(const Model&) const = default;
    };
    static Cmd update(Model& m, ToEditor t) { return Ed::update(m.editor, std::move(t)); }
    static Cmd update(Model&, FromEditor f) {
        if (std::holds_alternative<Editor::Saved>(f.out)) return Cmd::report(Dirty{});
        return {};
    }
};

struct Root {
    struct ToApp { AppReporting::Msg msg; };
    struct FromApp { AppReporting::Out out; };
    using Msg = std::variant<ToApp, FromApp>;
    using A = jaal::child<AppReporting, Root, ToApp, FromApp>;
    using Cmd = jaal::Cmd<Msg>;
    struct Model {
        AppReporting::Model app;
        int dirty = 0;
        bool operator==(const Model&) const = default;
    };
    static Cmd update(Model& m, ToApp t) { return A::update(m.app, std::move(t)); }
    static Cmd update(Model& m, FromApp) {
        ++m.dirty;
        return {};
    }
};

int nested_reports_bubble_one_level_at_a_time() {
    jaal::given<Root> t;
    t.when(Root::ToApp{AppReporting::ToEditor{Editor::Type{'a'}}})
        .when(Root::ToApp{AppReporting::ToEditor{Editor::Save{}}});
    t.settle();
    CHECK(t.ok());
    CHECK(t.model().dirty == 1);
    return 0;
}

}  // namespace

int main() {
    int (*const checks[])() = {child_reports_reach_the_parent, every_kind_the_child_lists_arrives,
                               children_say_which_one, nested_reports_bubble_one_level_at_a_time};
    for (auto f : checks)
        if (int r = f()) return r;
    return 0;
}
