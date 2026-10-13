#pragma once

#include <string_view>
#include <vector>

#include <stdx/option.hh>

#include "compiler/sema/side_tables.hh"

namespace ghoti::sema {

// One parameter as a callable declaration wrote it: its name and, when it has one, its default's
// source text
struct callable_param {
    std::string_view name;
    std::string_view default_value;
};

// The parameters a callable declaration wrote down, following aliases (`f: Callback`,
// `const g = mod.f;`) across modules. `none` when the declaration isn't a named callable.
[[nodiscard]] auto callable_params(const declaration_ref& declaration)
    -> stdx::option<std::vector<callable_param>>;
[[nodiscard]] auto callable_param_names(const declaration_ref& declaration)
    -> stdx::option<std::vector<std::string_view>>;

} // namespace ghoti::sema
