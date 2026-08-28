#pragma once

#include <string_view>

#include <stdx/option.hh>
#include <stdx/types.hh>

namespace ghoti::ast {

// C is the default and  means "whatever the target ABI dictates"
enum class calling_convention : u8 {
    C,
    SYSV,
    WIN64,
    STDCALL,
    FASTCALL,
    AAPCS,
};

[[nodiscard]] auto calling_convention_from_name(std::string_view name) noexcept
    -> stdx::option<calling_convention>;
[[nodiscard]] auto calling_convention_name(calling_convention conv) noexcept -> std::string_view;

} // namespace ghoti::ast
