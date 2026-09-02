#include "compiler/syntax/builtins.hh"

#include <string_view>
#include <tuple>

#include <stdx/fixed/enum_map.hh>
#include <stdx/fixed/hash_table.hh>
#include <stdx/hash.hh>
#include <stdx/option.hh>

#include "compiler/syntax/token_type.hh"
#include "support/string_utils.hh"

namespace ghoti::syntax {

namespace {

constexpr auto ALL_BUILTINS_BY_SV{std::apply(
    [](auto&&... builtin) { return string_utils::make_constexpr_map<token_type_t>(builtin...); },
    ALL_BUILTINS)};

constexpr auto ALL_BUILTINS_BY_TT{[] -> auto {
    stdx::fixed::enum_map<token_type_t, stdx::option<std::string_view>> builtins;
    for (const auto& [name, tok] : ALL_BUILTINS_BY_SV) { builtins[tok] = name; }
    return builtins;
}()};

} // namespace

auto get_builtin_opt(token_type_t tt) noexcept -> stdx::option<std::string_view> {
    return ALL_BUILTINS_BY_TT[tt];
}

auto get_builtin_opt(std::string_view sv) noexcept -> stdx::option<token_type_t> {
    return ALL_BUILTINS_BY_SV.get_opt(sv).materialize();
}

} // namespace ghoti::syntax
