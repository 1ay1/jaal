#pragma once
// jaal::meta — "does this type DECLARE a member with this name?", for any
// shape of member.
//
// ── why this exists ──────────────────────────────────────────────────────
//
// jaal's optional hooks are detected with a requires-test on their SHAPE:
//
//     if constexpr (requires(H& h, K& k) { h.present(k); }) host.present(k);
//
// That is the right way to CALL an optional hook, but it cannot tell two
// very different situations apart:
//
//     (a) the host has no present() at all        — opted out, fine.
//     (b) the host has one whose signature drifted — a BUG, silently read
//                                                    as (a).
//
// (b) has shipped twice: agentty's init() kept an older
// `pair<Model,Cmd> init()` shape and the kernel value-initialised a blank
// Model over everything init() had just loaded; and maya's terminal_host
// declared attach() against a base host_context, so a derived host never
// had attach called and the program took no keyboard input. Both compiled
// clean and failed at runtime, far from the cause.
//
// The fix is to pair every shape-test with a NAME-test and static_assert
// that `declares && !shape` is impossible. This header is the name-test.
//
// ── why not `requires { &T::name; }` ─────────────────────────────────────
//
// The obvious probe is forming a pointer-to-member. It is wrong in a way
// that is worse than useless, because it fails CLOSED on exactly the hooks
// most likely to drift:
//
//     struct H {
//         void attach(ctx&);                      // &H::attach   ✓ ok
//         template <class K> void present(K&);    // &H::present  ✗ FALSE
//         void handle(A); void handle(B);         // &H::handle   ✗ FALSE
//     };
//
// A template member has no single address to take, and an overload set is
// ambiguous, so both report "this member does not exist". jaal's own
// terminal host declares `present`, `present_frame` and `handle` as
// templates — so the drift check was structurally blind to them, and a
// host whose present() drifted compiled clean and simply never drew.
//
// ── how this works instead ───────────────────────────────────────────────
//
// A poison pill. Mix T with a base that declares the same member name and
// nothing else:
//
//     struct pill { void present(); };
//     template <class T> struct mixin : T, pill {};
//
// Now look the name up in `mixin<T>`. If T did NOT declare it, lookup
// finds only `pill::present` and succeeds. If T declared it in ANY shape —
// plain, template, overloaded, drifted, or inherited from a base of its
// own — lookup finds declarations in two unrelated subobjects and is
// AMBIGUOUS, so the probe fails. Failure means "T declares it".
//
// Ambiguity is the signal, so the member's shape never enters into it.
// That is the whole trick: we are asking about the NAME, and only the
// name.
//
//     T declares it        →  ambiguous  →  probe false  →  concept TRUE
//     T does not           →  finds pill →  probe true   →  concept FALSE
//
// Inheritance resolves the way we need too: dominance only hides a name
// when one subobject derives from the other, and `pill` is unrelated to T
// by construction, so a hook T inherited from ITS base still collides with
// the pill and is correctly reported as declared.
//
// ── limits, stated honestly ──────────────────────────────────────────────
//
// The mixin needs T to be a usable base class. For a final or non-class T
// we fall back to the pointer-to-member probe, which is imperfect but
// no worse than what the whole check used to be. No jaal host is final;
// the fallback exists so the concept is total rather than ill-formed.
//
// Private or ambiguous-within-T members read as declared. That is the
// conservative direction: it can ask for a static_assert on a hook jaal
// would not have called anyway, and it never hides a drifted one.

#include <type_traits>

namespace jaal::meta {

/// Can `T` be used as a base class? The poison-pill probe needs this;
/// anything else falls back to the pointer-to-member probe.
template <class T>
concept probeable_base =
    std::is_class_v<T> && !std::is_union_v<T> && !std::is_final_v<T>;

}  // namespace jaal::meta

/// Define `NAME`, a concept true when a type declares a member called
/// `MEMBER` in ANY shape — plain, template, overloaded, or inherited.
///
///     JAAL_DECLARES_MEMBER(host_declares_present, present);
///     static_assert(host_declares_present<my_host>);
///
/// Expands to a concept plus a small detail namespace holding the pill.
/// Use it at namespace scope; `NAME` must be unique in that namespace.
#define JAAL_DECLARES_MEMBER(NAME, MEMBER)                                    \
    namespace jaal_declares_##NAME##_detail {                                 \
        /* Declares the name and nothing else. Never defined — the probe   */ \
        /* only ever performs name lookup, so no body is needed.           */ \
        struct pill { void MEMBER(); };                                       \
                                                                              \
        /* Fallback for final / non-class T: the old pointer-to-member     */ \
        /* probe. Imperfect for templates and overload sets, but total.    */ \
        template <class T, bool = ::jaal::meta::probeable_base<T>>            \
        struct probe : std::bool_constant<requires { &T::MEMBER; }> {};       \
                                                                              \
        template <class T> struct mixin : T, pill {};                         \
                                                                              \
        /* The real probe: ambiguous lookup means T declared it too.       */ \
        template <class T>                                                    \
        struct probe<T, true>                                                 \
            : std::bool_constant<!requires { &mixin<T>::MEMBER; }> {};        \
    }                                                                         \
    template <class T>                                                        \
    concept NAME = jaal_declares_##NAME##_detail::probe<T>::value
