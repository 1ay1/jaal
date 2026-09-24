#pragma once
// jaal::meta::type_name — a type's name as a compile-time string.
//
// Used only in diagnostics ("field 'GotLine::text' is std::string_view"),
// never for identity or dispatch: the spelling is compiler-specific
// (e.g. "std::__cxx11::basic_string<char>" on libstdc++) and that's fine
// for a human reading an error, not fine for anything a program depends on.

#include <string_view>

namespace jaal::meta {

namespace detail {
template <class T>
consteval std::string_view raw_signature() noexcept {
#if defined(__clang__) || defined(__GNUC__)
    return __PRETTY_FUNCTION__;
#elif defined(_MSC_VER)
    return __FUNCSIG__;
#else
    return {};
#endif
}
}  // namespace detail

template <class T>
consteval std::string_view type_name() noexcept {
    constexpr std::string_view s = detail::raw_signature<T>();
#if defined(__clang__)
    // "... raw_signature() [T = X]"
    constexpr auto b = s.find("T = ") + 4;
    constexpr auto e = s.rfind(']');
#elif defined(__GNUC__)
    // "... raw_signature() [with T = X; std::string_view = ...]"
    constexpr auto b = s.find("T = ") + 4;
    constexpr auto e = s.find_first_of(";]", b);
#elif defined(_MSC_VER)
    // "... raw_signature<X>(void) noexcept"
    constexpr auto b = s.find("raw_signature<") + 14;
    constexpr auto e = s.rfind(">(");
#else
    constexpr std::size_t b = 0, e = 0;
#endif
    if constexpr (b >= s.size() || e <= b) return "<type>";
    else return s.substr(b, e - b);
}

}  // namespace jaal::meta
