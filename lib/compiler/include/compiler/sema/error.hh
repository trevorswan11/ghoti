#pragma once

#include <utility>

#include <stdx/result.hh>
#include <stdx/types.hh>

#include "support/diagnostic.hh"

namespace ghoti::sema {

enum class error : u8 {
    IDENTIFIER_REDECLARATION,
    ILLEGAL_TOP_LEVEL_STATEMENT,
    ILLEGAL_CONTROL_FLOW,
    ILLEGAL_IMPORT_LOCATION,
    ILLEGAL_NON_CONST_STATEMENT,
    FUNCTION_DECLARATION_MISSING_BODY,
    REDUNDANT_CONSTEXPR,
    INVALID_TABLE_IDX,
    SHADOWING_DECLARATION,
    ILLEGAL_TEST_LOCATION,
    MODULE_LOAD_ERROR,
    NON_CALLABLE_EXPRESSION,
    ARITY_MISMATCH,
    UNDECLARED_IDENTIFIER,
    TYPE_MISMATCH,
    OUTER_SCOPE_NOT_FOUND,
    CYCLIC_DEPENDENCY,
    ILLEGAL_SELF_PARAMETER,
    ILLEGAL_LABEL_USAGE,
    DUPLICATE_TEST_NAME,
    DUPLICATE_FIELD,
    MISSING_FIELD,
    UNKNOWN_FIELD,
    DUPLICATE_ENUMERATION,
    UNKNOWN_ENUMERATION,
    ILLEGAL_MATCH_PATTERN,
    AUTO_WITHOUT_INITIALIZER,
    ILLEGAL_AUTO_USAGE,
    CONSTEXPR_EVALUATION_FAILED,
    CONSTEXPR_RECURSION_LIMIT_EXCEEDED,
    GIR_FORBIDDEN_TYPE,
    ILLEGAL_PRIVATE_ACCESS,
    ASSIGNMENT_TO_CONST,
    ILLEGAL_CONST_CAST,
    OPERATOR_TYPE_MISMATCH,
    RETURN_TYPE_MISMATCH,
    AUTO_RETURN_TYPE_CONFLICT,
    ILLEGAL_OPAQUE_TYPE,
    ILLEGAL_REFERENCE_TO_REFERENCE,
    CLOSURE_SIGNATURE_MISMATCH,
    ILLEGAL_CLOSURE_ESCAPE,
};

using diagnostic  = diagnostic<error>;
using diagnostics = diagnostic_list<diagnostic>;

template <typename... Args>
[[nodiscard]] constexpr auto make_sema_err(Args&&... args) -> stdx::err<diagnostic> {
    return stdx::make_err<diagnostic>(std::forward<Args>(args)...);
}

} // namespace ghoti::sema
