#include "compiler/ast/attributes.hh"

#include <string_view>
#include <utility>

#include <stdx/fixed/enum_map.hh>
#include <stdx/option.hh>

#include "support/string_utils.hh"

namespace ghoti::ast {

namespace {

constexpr auto CALLCONV_NAMES_TO_VALUES{string_utils::make_constexpr_map<calling_convention>(
    std::pair{"c", calling_convention::C},
    std::pair{"sysv", calling_convention::SYSV},
    std::pair{"win64", calling_convention::WIN64},
    std::pair{"stdcall", calling_convention::STDCALL},
    std::pair{"fastcall", calling_convention::FASTCALL},
    std::pair{"aapcs", calling_convention::AAPCS})};

constexpr auto CALLCONV_VALS_TO_NAMES{[] {
    stdx::fixed::enum_map<calling_convention, std::string_view> map{};
    for (const auto& [sv, conv] : CALLCONV_NAMES_TO_VALUES) { map[conv] = sv; }
    return map;
}()};

} // namespace

auto calling_convention_from_name(std::string_view name) noexcept
    -> stdx::option<calling_convention> {
    return CALLCONV_NAMES_TO_VALUES.get_opt(name).materialize();
}

auto calling_convention_name(calling_convention conv) noexcept -> std::string_view {
    return CALLCONV_VALS_TO_NAMES[conv];
}

} // namespace ghoti::ast
