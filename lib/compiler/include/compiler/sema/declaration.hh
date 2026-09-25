#pragma once

#include <string_view>
#include <vector>

#include <stdx/option.hh>

#include "compiler/sema/side_tables.hh"

namespace ghoti::sema {

// The parameter names a callable declaration wrote down, following aliases (`f: Callback`,
// `const g := mod.f;`) across modules. `none` when the declaration isn't a named callable.
[[nodiscard]] auto callable_param_names(const declaration_ref& declaration)
    -> stdx::option<std::vector<std::string_view>>;

} // namespace ghoti::sema
