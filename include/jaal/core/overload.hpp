#pragma once
// jaal::overload — build a visitor from lambdas, for std::visit on a Msg.
//
//   std::visit(jaal::overload{
//       [&](Inc) { ++m.n; },
//       [&](Dec) { --m.n; },
//   }, msg);
//
// With a closed Msg variant, forgetting a case is a compile error.

namespace jaal {

template <class... Fs>
struct overload : Fs... {
    using Fs::operator()...;
};

}  // namespace jaal
