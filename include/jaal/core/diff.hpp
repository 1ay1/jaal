#pragma once
// jaal::diff — which fields of two values differ, and how.
//
//   struct Model { int n; std::string name; Inner in; };
//   for (auto& c : jaal::diff(before, after))
//       std::printf("%s: %s -> %s\n", c.path.c_str(), c.before.c_str(), c.after.c_str());
//   // .0: 1 -> 2
//   // .2.1: false -> true
//
// Fields are named by position (".2.1" is field 2's field 1). C++26 as
// shipped by GCC 16 / clang 22 can bind every field of a struct, but not
// name it, so positions are what there is.
//
// Rules, in order, for a pair of values of type T:
//   * T has ==  and is formattable: compared, printed with std::format
//   * T is a plain struct: each field is diffed, recursively
//   * T has ==: compared; printed as "?" (no formatter)
//   * none of those: can't be compared, reported as one "?" change only if
//     the caller asks with diff_options::report_opaque
// Nothing here runs on a hot path: it's for debugging and test output.

#include <cstddef>
#include <format>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../meta/fields.hpp"

namespace jaal {

struct field_change {
    std::string path;     // ".0", ".2.1", or "" for the whole value
    std::string before;
    std::string after;
    bool operator==(const field_change&) const = default;
};

struct diff_options {
    bool report_opaque = false;   // report fields that can't be compared
    std::size_t max_text = 80;    // longer printed values are cut, with "..."
};

namespace detail::diffx {

template <class T>
concept printable = std::formattable<T, char>;

template <class T>
std::string show(const T& v, const diff_options& o) {
    if constexpr (printable<T>) {
        std::string s;
        if constexpr (std::is_convertible_v<const T&, std::string_view>)
            s = std::format("\"{}\"", v);
        else
            s = std::format("{}", v);
        if (s.size() > o.max_text) { s.resize(o.max_text); s += "..."; }
        return s;
    } else {
        return "?";
    }
}

template <class T>
void walk(const T& a, const T& b, std::string& path, std::vector<field_change>& out,
          const diff_options& o) {
    if constexpr (std::equality_comparable<T> && printable<T>) {
        if (!(a == b)) out.push_back({path, show(a, o), show(b, o)});
    }
#if JAAL_HAS_FIELDS
    else if constexpr (meta::aggregate_struct<T>) {
        const auto& [... xs] = a;
        const auto& [... ys] = b;
        std::size_t i = 0;
        auto one = [&](const auto& x, const auto& y) {
            const auto len = path.size();
            path += '.';
            path += std::to_string(i++);
            walk(x, y, path, out, o);
            path.resize(len);
        };
        (one(xs, ys), ...);
    }
#endif
    else if constexpr (std::equality_comparable<T>) {
        if (!(a == b)) out.push_back({path, "?", "?"});
    } else {
        if (o.report_opaque) out.push_back({path, "?", "?"});
    }
}

}  // namespace detail::diffx

/// The fields of `a` and `b` that differ. Empty when they're equal (as far
/// as the rules above can tell).
template <class T>
[[nodiscard]] std::vector<field_change> diff(const T& a, const T& b, diff_options o = {}) {
    std::vector<field_change> out;
    std::string path;
    detail::diffx::walk(a, b, path, out, o);
    return out;
}

/// One line per change: ".0: 1 -> 2".
[[nodiscard]] inline std::string to_string(const std::vector<field_change>& d) {
    std::string s;
    for (const auto& c : d) {
        s += c.path.empty() ? std::string("(value)") : c.path;
        s += ": ";
        s += c.before;
        s += " -> ";
        s += c.after;
        s += '\n';
    }
    return s;
}

}  // namespace jaal
