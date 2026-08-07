#pragma once

#include <utility>
#include <vector>

#include <stdx/option.hh>
#include <stdx/result.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/syntax/error.hh"

namespace ghoti {

namespace syntax { class parser; } // namespace syntax

namespace ast {

struct explicit_array_type {
    stdx::option<expr_handle> dimension;
    bool                      null_terminated;
    bool                      mut_elements;
    explicit_type_id          inner_explicit_type;
};

struct explicit_function_type {
    std::vector<explicit_type_id> parameter_types;
    bool                          variadic;
    explicit_type_id              explicit_return_type;

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<explicit_function_type, syntax::diagnostic>;
};

struct explicit_type {
    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<explicit_type_id, syntax::diagnostic>;

    // Parses an optionally present type and checks/advances for value initialization
    [[nodiscard]] static auto parse_opt_init(syntax::parser& parser)
        -> stdx::result<std::pair<stdx::option<explicit_type_id>, bool>, syntax::diagnostic>;
};

} // namespace ast

} // namespace ghoti
