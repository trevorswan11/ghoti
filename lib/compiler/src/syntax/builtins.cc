#include "compiler/syntax/builtins.hh"

#include <array>
#include <string_view>
#include <tuple>
#include <utility>

#include <stdx/option.hh>

#include "compiler/syntax/token_type.hh"
#include "support/string_utils.hh"

namespace ghoti::syntax {

namespace {

constexpr auto BUILTINS_BY_NAME{std::apply(
    [](const auto&... builtin) {
        return string_utils::make_constexpr_map<builtin_t>(
            std::pair{builtin.name, builtin.kind}...);
    },
    ALL_BUILTINS)};

constexpr auto BUILTIN_NAMES{[] {
    std::array<std::string_view, ALL_BUILTINS.size()> names{};
    for (const auto& builtin : ALL_BUILTINS) {
        names[std::to_underlying(builtin.kind)] = builtin.name;
    }
    return names;
}()};

} // namespace

auto builtin_name(builtin_t builtin) noexcept -> std::string_view {
    return BUILTIN_NAMES[std::to_underlying(builtin)];
}

auto builtin_from_name(std::string_view name) noexcept -> stdx::option<builtin_t> {
    return BUILTINS_BY_NAME.get_opt(name).materialize();
}

auto builtin_token_opt(std::string_view name) noexcept -> stdx::option<token_type_t> {
    if (builtin_from_name(name)) { return token_type_t::BUILTIN; }
    for (const auto& form : builtins::SPECIAL_FORMS) {
        if (form.name == name) { return form.type; }
    }
    return stdx::none;
}

} // namespace ghoti::syntax
