// tests/meta/declares_test.cpp — static tests for JAAL_DECLARES_MEMBER.
//
// Every check is a static_assert: if this file compiles, the tests passed.
// main() exists only so ctest has something to run.
//
// What's being pinned: the probe answers "does this type declare a member
// with this NAME", for any SHAPE of member. The shape-blindness is the
// whole point — jaal pairs this with a shape test to tell "no such hook"
// (fine) apart from "hook with the wrong signature" (a bug). A probe that
// misses some shapes would silently classify those as "no such hook",
// which is the exact failure D42 exists to prevent.

#include <jaal/meta/declares.hpp>

#include <optional>

// ── the shapes a member can take ─────────────────────────────────────────
struct Plain          { void f(int); };
struct Templated      { template <class T> void f(T&); };
struct Overloaded     { void f(int); void f(double); };
struct TmplOverloaded { void f(int); template <class T> void f(T*, T*); };
struct Static         { static void f(int); };
struct StaticTmpl     { template <class T> static void f(T&); };
struct Variadic       { void f(int, ...); };
struct ConstQualified { void f(int) const; };
struct RefQualified   { void f(int) &&; };
struct DataMember     { int f; };
struct MemberType     { struct f {}; };
struct Absent         { void other(int); };
struct Empty          { };

// Inherited, which is the general form of the maya/agentty scar: the drift
// isn't even in the class you're looking at.
struct Base             { template <class T> void f(T&); };
struct Derived : Base   { };
struct DerivedShadows : Base { void f(int); };   // declared in BOTH

JAAL_DECLARES_MEMBER(declares_f, f);

// ── declared, in every shape ────────────────────────────────────────────
static_assert(declares_f<Plain>);
static_assert(declares_f<Templated>);       // &T::f is ill-formed here
static_assert(declares_f<Overloaded>);      // &T::f is ambiguous here
static_assert(declares_f<TmplOverloaded>);
static_assert(declares_f<Static>);
static_assert(declares_f<StaticTmpl>);
static_assert(declares_f<Variadic>);
static_assert(declares_f<ConstQualified>);
static_assert(declares_f<RefQualified>);

// Not a function, but it IS the name — and a hook replaced by a data member
// or a nested type is drift worth reporting, not silence.
static_assert(declares_f<DataMember>);
static_assert(declares_f<MemberType>);

// ── inheritance ─────────────────────────────────────────────────────────
static_assert(declares_f<Derived>);         // found in the base
static_assert(declares_f<DerivedShadows>);  // shadowing still collides

// ── not declared ────────────────────────────────────────────────────────
static_assert(!declares_f<Absent>);
static_assert(!declares_f<Empty>);

// ── totality: never ill-formed, whatever it's handed ────────────────────
// A final class can't be a base, so the probe falls back to the
// pointer-to-member form. Imperfect (it can't see templates), but total —
// the concept must answer rather than fail to compile.
struct FinalWith final  { void f(int); };
struct FinalWithout final { };
static_assert(declares_f<FinalWith>);
static_assert(!declares_f<FinalWithout>);

// Non-class types must answer false rather than be ill-formed.
static_assert(!declares_f<int>);
static_assert(!declares_f<void*>);
static_assert(!declares_f<int[4]>);
static_assert(!declares_f<std::optional<int>>);   // real type, no member f

union U { int a; double b; };
static_assert(!declares_f<U>);

// ── independence: two probes in one scope don't collide ─────────────────
struct HasG { template <class T> void g(T&); };
JAAL_DECLARES_MEMBER(declares_g, g);
static_assert(declares_g<HasG>);
static_assert(!declares_g<Plain>);      // has f, not g
static_assert(!declares_f<HasG>);       // has g, not f

int main() { return 0; }
