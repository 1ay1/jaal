#pragma once
// jaal::meta::fields — the field types of a plain struct, at compile time.
//
// The one piece of "reflection" jaal needs. C++26 structured binding packs
// (P1061) let a template bind every field of an aggregate:
//
//     auto& [...xs] = t;
//
// and __builtin_structured_binding_size(T) (GCC 16, clang 22) says softly
// whether T can be bound at all. That matters: binding a union, a type with
// private fields, or a type with fields in both itself and a base is a hard
// error inside a function body, which no concept can catch. The builtin
// turns those into a clean `false`.
//
// Field types are reported exactly as a CONST binding sees them:
//   - a normal field `int x`         → const int
//   - a `mutable` field              → int          (non-const: used by Frozen)
//   - a reference field `int& r`     → int&         (references keep their kind)
//   - an array field `int a[3]`      → const int[3]
//
// When structured binding packs aren't available (MSVC today), `decomposable`
// is false for everything and callers fall back to shallow rules.

#include <cstddef>
#include <type_traits>
#include <utility>

#include "list.hpp"
#include "algo.hpp"

namespace jaal::meta {

#if defined(__cpp_structured_bindings) && __cpp_structured_bindings >= 202411L \
    && defined(__has_builtin)
#  if __has_builtin(__builtin_structured_binding_size)
#    define JAAL_HAS_FIELDS 1
#  endif
#endif
#ifndef JAAL_HAS_FIELDS
#  define JAAL_HAS_FIELDS 0
#endif

// Without this, every struct looks opaque, so jaal's Sendable and Frozen
// checks reject EVERY program with "a class jaal can't see inside" pointing
// at the user's first message type. That diagnoses the wrong thing: the
// program is fine, the language level isn't. Say so directly. MSVC is the
// exception jaal still supports on shallow rules; everything else needs
// C++26.
#if !JAAL_HAS_FIELDS && !defined(_MSC_VER)
#  error "jaal needs C++26 structured binding packs (P1061) to check messages are Sendable: build with -std=c++26 (or -std=c++2c) on GCC 16+ or clang 22+."
#endif

inline constexpr bool has_fields_support = JAAL_HAS_FIELDS;

// ── aggregate_struct ─────────────────────────────────────────────────────
// A class aggregate whose fields can be bound. Tuple-like types
// (std::pair, std::array) also have a binding size but are NOT treated as
// structs here: they are handled by their own rules, which is more precise.
template <class T>
concept aggregate_struct =
    has_fields_support
    && std::is_class_v<T>
    && std::is_aggregate_v<T>
#if JAAL_HAS_FIELDS
    && requires { __builtin_structured_binding_size(T); }
#endif
    ;

// ── fields_t ─────────────────────────────────────────────────────────────────────
// The pack binding below is SYNTAX that older compilers can't even parse,
// so the whole definition is compiled out without support. aggregate_struct
// is then false for every type, and fields_t is never instantiated.
#if JAAL_HAS_FIELDS
namespace detail {
template <class T>
auto bind_fields(const T& t) {
    auto& [... xs] = t;
    return list<decltype(xs)...>{};
}
}  // namespace detail

template <aggregate_struct T>
using fields_t = decltype(detail::bind_fields(std::declval<const T&>()));
#else
template <aggregate_struct T>
using fields_t = list<>;   // unreachable: aggregate_struct is always false
#endif

// The binding types as a const binding sees them, with the top-level const
// removed. A `mutable` field still differs: it was never const to begin with.
// Use fields_t when you need to tell them apart (Frozen does).
template <aggregate_struct T>
using field_types_t = transform_t<std::remove_const_t, fields_t<T>>;

}  // namespace jaal::meta
