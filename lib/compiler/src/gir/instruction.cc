#include "compiler/gir/instruction.hh"

#include <string_view>

#include <magic_enum/magic_enum.hpp>
#include <stdx/enum.hh>
#include <stdx/fixed/enum_map.hh>

#include "support/string_utils.hh"

namespace ghoti::gir {

namespace {

constexpr auto INSTRUCTION_KIND_NAMES{[] {
    stdx::fixed::enum_map<instruction_kind, string_utils::lowercase_str<32>> map{};
    for (const auto kind : stdx::enum_range<instruction_kind>()) {
        map[kind] = string_utils::lowercase_str<32>{magic_enum::enum_name(kind)};
    }
    return map;
}()};

} // namespace

auto instruction_kind_name(instruction_kind kind) noexcept -> std::string_view {
    return INSTRUCTION_KIND_NAMES[kind].view();
}

} // namespace ghoti::gir
