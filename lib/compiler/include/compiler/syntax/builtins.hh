#pragma once

#include <string_view>

#include <stdx/enum.hh>
#include <stdx/option.hh>

#include "compiler/syntax/token_type.hh"

namespace ghoti::syntax {

using builtin_t = typed_identifier;

namespace builtins {

constexpr builtin_t ALIGN_CAST{"@alignCast", token_type_t::BUILTIN_ALIGN_CAST};
constexpr builtin_t PTR_CAST{"@ptrCast", token_type_t::BUILTIN_PTR_CAST};
constexpr builtin_t BIT_CAST{"@bitCast", token_type_t::BUILTIN_BIT_CAST};
constexpr builtin_t CONST_CAST{"@constCast", token_type_t::BUILTIN_CONST_CAST};
constexpr builtin_t VOLATILE_CAST{"@volatileCast", token_type_t::BUILTIN_VOLATILE_CAST};
constexpr builtin_t AS{"@as", token_type_t::BUILTIN_AS};

constexpr builtin_t INT_FROM_PTR{"@intFromPtr", token_type_t::BUILTIN_INT_FROM_PTR};
constexpr builtin_t PTR_FROM_INT{"@ptrFromInt", token_type_t::BUILTIN_PTR_FROM_INT};
constexpr builtin_t PTR_FROM_ARRAY{"@ptrFromArray", token_type_t::BUILTIN_PTR_FROM_ARRAY};
constexpr builtin_t SLICE_FROM_PTR{"@sliceFromPtr", token_type_t::BUILTIN_SLICE_FROM_PTR};
constexpr builtin_t FIELD_PARENT_PTR{"@fieldParentPtr", token_type_t::BUILTIN_FIELD_PARENT_PTR};

constexpr builtin_t ALIGN_OF{"@alignOf", token_type_t::BUILTIN_ALIGN_OF};
constexpr builtin_t SIZE_OF{"@sizeOf", token_type_t::BUILTIN_SIZE_OF};
constexpr builtin_t TYPE_OF{"@typeOf", token_type_t::BUILTIN_TYPE_OF};
constexpr builtin_t THIS{"@this", token_type_t::BUILTIN_THIS};
constexpr builtin_t TAG_NAME{"@tagName", token_type_t::BUILTIN_TAG_NAME};
constexpr builtin_t TYPE_NAME{"@typeName", token_type_t::BUILTIN_TYPE_NAME};

constexpr builtin_t MEMCPY{"@memcpy", token_type_t::BUILTIN_MEMCPY};
constexpr builtin_t MEMSET{"@memset", token_type_t::BUILTIN_MEMSET};
constexpr builtin_t MEMMOVE{"@memmove", token_type_t::BUILTIN_MEMMOVE};

constexpr builtin_t MUL_ADD{"@mulAdd", token_type_t::BUILTIN_MUL_ADD};
constexpr builtin_t CLZ{"@clz", token_type_t::BUILTIN_CLZ};
constexpr builtin_t CTZ{"@ctz", token_type_t::BUILTIN_CTZ};
constexpr builtin_t POP_COUNT{"@popCount", token_type_t::BUILTIN_POP_COUNT};
constexpr builtin_t ABS{"@abs", token_type_t::BUILTIN_ABS};

constexpr builtin_t MIN{"@min", token_type_t::BUILTIN_MIN};
constexpr builtin_t MAX{"@max", token_type_t::BUILTIN_MAX};
constexpr builtin_t DIV_TRUNC{"@divTrunc", token_type_t::BUILTIN_DIV_TRUNC};
constexpr builtin_t DIV_FLOOR{"@divFloor", token_type_t::BUILTIN_DIV_FLOOR};
constexpr builtin_t REM{"@rem", token_type_t::BUILTIN_REM};
constexpr builtin_t MOD{"@mod", token_type_t::BUILTIN_MOD};
constexpr builtin_t ADD_WITH_OVERFLOW{"@addWithOverflow", token_type_t::BUILTIN_ADD_WITH_OVERFLOW};
constexpr builtin_t SUB_WITH_OVERFLOW{"@subWithOverflow", token_type_t::BUILTIN_SUB_WITH_OVERFLOW};
constexpr builtin_t MUL_WITH_OVERFLOW{"@mulWithOverflow", token_type_t::BUILTIN_MUL_WITH_OVERFLOW};
constexpr builtin_t SHL_WITH_OVERFLOW{"@shlWithOverflow", token_type_t::BUILTIN_SHL_WITH_OVERFLOW};

constexpr builtin_t C_VA_START{"@cVaStart", token_type_t::BUILTIN_C_VA_START};
constexpr builtin_t C_VA_ARG{"@cVaArg", token_type_t::BUILTIN_C_VA_ARG};
constexpr builtin_t C_VA_COPY{"@cVaCopy", token_type_t::BUILTIN_C_VA_COPY};
constexpr builtin_t C_VA_END{"@cVaEnd", token_type_t::BUILTIN_C_VA_END};
constexpr builtin_t ALIGNAS{"@alignas", token_type_t::BUILTIN_ALIGNAS};

constexpr builtin_t TARGET_OS{"@targetOs", token_type_t::BUILTIN_TARGET_OS};
constexpr builtin_t TARGET_ARCH{"@targetArch", token_type_t::BUILTIN_TARGET_ARCH};
constexpr builtin_t TARGET_TRIPLE{"@targetTriple", token_type_t::BUILTIN_TARGET_TRIPLE};
constexpr builtin_t TARGET_ABI{"@targetAbi", token_type_t::BUILTIN_TARGET_ABI};
constexpr builtin_t TARGET_PTR_BITS{"@targetPtrBits", token_type_t::BUILTIN_TARGET_PTR_BITS};
constexpr builtin_t TARGET_ENDIAN{"@targetEndian", token_type_t::BUILTIN_TARGET_ENDIAN};
constexpr builtin_t TARGET_FAMILY{"@targetFamily", token_type_t::BUILTIN_TARGET_FAMILY};

constexpr builtin_t SET_EVAL_RECURSION_LIMIT{"@setEvalRecursionLimit",
                                             token_type_t::BUILTIN_SET_EVAL_RECURSION_LIMIT};
constexpr builtin_t SET_MAIN_SYMBOL{"@setMainSymbol", token_type_t::BUILTIN_SET_MAIN_SYMBOL};

constexpr builtin_t PANIC{"@panic", token_type_t::BUILTIN_PANIC};
constexpr builtin_t TRAP{"@trap", token_type_t::BUILTIN_TRAP};

// The innermost enclosing function as a callable value, for self-recursion
constexpr builtin_t FN_CTX{"@fnCtx", token_type_t::BUILTIN_FN_CTX};

constexpr builtin_t SRC{"@src", token_type_t::BUILTIN_SRC};
constexpr builtin_t EXPECT{"@expect", token_type_t::BUILTIN_EXPECT};
constexpr builtin_t REQUIRE{"@require", token_type_t::BUILTIN_REQUIRE};
constexpr builtin_t SKIP{"@skip", token_type_t::BUILTIN_SKIP};

constexpr builtin_t COMPILE_ERROR{"@compileError", token_type_t::BUILTIN_COMPILE_ERROR};
constexpr builtin_t CFG{"@cfg", token_type_t::BUILTIN_CFG};
constexpr builtin_t CFG_VALUE{"@cfgValue", token_type_t::BUILTIN_CFG_VALUE};

constexpr auto ALL_TOKEN_TYPES{
    stdx::enum_range<token_type_t::BUILTIN_ALIGN_CAST, token_type_t::BUILTIN_COMPILE_ERROR>()};

constexpr auto SPECIAL_FORM_TOKEN_TYPES{
    stdx::enum_range<token_type_t::BUILTIN_CFG, token_type_t::BUILTIN_CFG_VALUE>()};

} // namespace builtins

[[nodiscard]] auto get_builtin_opt(token_type_t tt) noexcept -> stdx::option<std::string_view>;
[[nodiscard]] auto get_builtin_opt(std::string_view sv) noexcept -> stdx::option<token_type_t>;

} // namespace ghoti::syntax
