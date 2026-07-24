#pragma once

#include <utility>

#include <stdx/result.hh>
#include <stdx/types.hh>

#include "support/diagnostic.hh"

namespace ghoti::codegen {

enum class error : u8 {
    VERIFICATION_FAILED,
    OPTIMIZATION_FAILED,
    MODULE_LOAD_ERROR,
    TARGET_LOOKUP_FAILED,
    TARGET_MACHINE_CREATION_FAILED,
    OBJECT_EMISSION_FAILED,
    LINKING_FAILED,
    PERMISSIONS_ERROR,
};

using diagnostic  = diagnostic<error>;
using diagnostics = diagnostic_list<diagnostic>;

template <typename... Args>
[[nodiscard]] constexpr auto make_codegen_err(Args&&... args) -> stdx::err<diagnostic> {
    return stdx::make_err<diagnostic>(std::forward<Args>(args)...);
}

} // namespace ghoti::codegen
