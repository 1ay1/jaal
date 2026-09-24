#pragma once
// jaal::meta::fixed_string — a string usable as a template argument.
//
// Effect descriptors carry their name as an NTTP (pure_fx<T, "beep">) so
// diagnostics can say "cannot run effect 'beep'" instead of printing a
// mangled type. Structural type: all members public, comparable at compile
// time.

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace jaal::meta {

template <std::size_t N>
struct fixed_string {
    char data[N]{};

    consteval fixed_string(const char (&s)[N]) noexcept {
        std::copy_n(s, N, data);
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return {data, N - 1};   // drop the terminator
    }
    [[nodiscard]] constexpr operator std::string_view() const noexcept { return view(); }

    template <std::size_t M>
    [[nodiscard]] constexpr bool operator==(const fixed_string<M>& o) const noexcept {
        return view() == o.view();
    }
};

template <std::size_t N>
fixed_string(const char (&)[N]) -> fixed_string<N>;

}  // namespace jaal::meta
