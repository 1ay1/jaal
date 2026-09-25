#pragma once
// jaal::fx::report — a child tells whoever embeds it that something happened.
//
// child<> and children<> route messages DOWN: the parent wraps the child's
// Msg and folds it into the child. Nothing comes back up. So a parent that
// wants to know "the editor saved" or "the login finished" had to look inside
// the child's model after every message and notice the change itself, which
// is the most repeated workaround in component code and the easiest to get
// wrong (it fires twice, or never, depending on when you look).
//
// A report is a message the child sends to its PARENT, as an effect:
//
//   struct Editor {
//       struct Saved { int count; };                  // what it reports
//       using Out = std::variant<Saved>;               // everything it can report
//       using Cmd = jaal::Cmd<Msg, jaal::fx::report<Out>>;
//       static Cmd update(Model& m, Save) {
//           ++m.saves;
//           return Cmd::report(Saved{m.saves});
//       }
//   };
//
// and child<> / children<> turn it into one of the parent's messages, typed:
//
//   using E = jaal::child<Editor, App, ToEditor, FromEditor>;
//   struct FromEditor { Editor::Out out; };                 // one of App's Msgs
//   static Cmd update(Model& m, FromEditor f) { ... }       // the parent hears it
//
// With a keyed list the parent also learns WHICH child reported:
//
//   using Tabs = jaal::children<Tab, App, ToTab, FromTab>;
//   struct FromTab { int id; Tab::Out out; };
//
// What the types guarantee:
//
//   * A child can only report what its `Out` lists. A report of anything
//     else doesn't compile.
//   * A child that reports can't be embedded without saying where its
//     reports go: child<> without the From argument, for a child whose row
//     has `report`, is a compile error naming the fix. A report can't be
//     lost by forgetting to wire it.
//   * The parent handles a report with an ordinary update overload, so a
//     report kind it doesn't handle is the usual "no update for message"
//     error.
//   * A report is folded in the same step, before the next message, like
//     Cmd::send. The parent sees the child's state as of the report.
//
// A program run as the ROOT (no parent) can still return reports; the
// kernel has nowhere to deliver them and drops them. A program that means to
// be embedded is tested with jaal::given, which records them.
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace jaal::fx {

template <class Out>
struct report {
    static constexpr std::string_view name = "report";
    using out_type = Out;

    /// The payload doesn't mention Msg: a report is addressed UP, and only
    /// child<> / children<> know the parent's Msg. So map() carries it
    /// through untouched, and the wrapper re-targets it into a send.
    template <class Msg>
    struct type {
        Out out;
    };
    template <class F, class M>
    static type<std::invoke_result_t<F, M>> fmap(F&&, type<M> e) {
        return {std::move(e.out)};
    }
    template <class Id, class F, class M>
    static type<std::invoke_result_t<F, const Id&, M>> fmap_with(const Id&, F&&, type<M> e) {
        return {std::move(e.out)};
    }

    template <class Self, class Msg>
    struct ctors {
        /// Report one of the things this child can report. Anything
        /// convertible to Out: a single case, or the variant itself.
        template <class E>
        [[nodiscard]] static Self report(E&& e) {
            static_assert(std::is_constructible_v<Out, E>,
                          "jaal: this report isn't one of the child's Out cases; "
                          "add it to `using Out = std::variant<...>`");
            return Self(type<Msg>{Out(std::forward<E>(e))});
        }
    };
};

}  // namespace jaal::fx

namespace jaal {

/// Is D a report effect (for some Out)?
template <class D>
inline constexpr bool is_report_v = false;
template <class Out>
inline constexpr bool is_report_v<fx::report<Out>> = true;

}  // namespace jaal
