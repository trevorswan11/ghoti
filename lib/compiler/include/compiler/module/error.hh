#pragma once

#include <utility>

#include <stdx/result.hh>
#include <stdx/types.hh>

#include "support/diagnostic.hh"

namespace ghoti::mod {

enum class error : u8 {
    PATH_DOES_NOT_EXIST,
    PATH_IS_NOT_FILE,
    FAILED_TO_OPEN_FILE,
    NORMALIZATION_FAILED,
    MODULE_PATH_NOT_RELATIVE,
    MODULE_DOES_NOT_EXIST,
    MODULE_ALREADY_EXISTS,
};

using diagnostic = diagnostic<error>;

template <typename... Args>
[[nodiscard]] constexpr auto make_mod_err(Args&&... args) -> stdx::err<diagnostic> {
    return stdx::make_err<diagnostic>(std::forward<Args>(args)...);
}

} // namespace ghoti::mod
