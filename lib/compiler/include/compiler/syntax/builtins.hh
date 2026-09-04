#pragma once

#include <array>
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
constexpr builtin_t DYN_CAST{"@dynCast", token_type_t::BUILTIN_DYN_CAST};

constexpr builtin_t INT_FROM_PTR{"@intFromPtr", token_type_t::BUILTIN_INT_FROM_PTR};
constexpr builtin_t PTR_FROM_INT{"@ptrFromInt", token_type_t::BUILTIN_PTR_FROM_INT};
constexpr builtin_t PTR_FROM_ARRAY{"@ptrFromArray", token_type_t::BUILTIN_PTR_FROM_ARRAY};
constexpr builtin_t SLICE_FROM_PTR{"@sliceFromPtr", token_type_t::BUILTIN_SLICE_FROM_PTR};
constexpr builtin_t FIELD_PARENT_PTR{"@fieldParentPtr", token_type_t::BUILTIN_FIELD_PARENT_PTR};

constexpr builtin_t ALIGN_OF{"@alignOf", token_type_t::BUILTIN_ALIGN_OF};
constexpr builtin_t SIZE_OF{"@sizeOf", token_type_t::BUILTIN_SIZE_OF};
constexpr builtin_t BIT_SIZE_OF{"@bitSizeOf", token_type_t::BUILTIN_BIT_SIZE_OF};
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
constexpr builtin_t IMPLEMENTS{"@implements", token_type_t::BUILTIN_IMPLEMENTS};
constexpr builtin_t EXPECT{"@expect", token_type_t::BUILTIN_EXPECT};
constexpr builtin_t REQUIRE{"@require", token_type_t::BUILTIN_REQUIRE};
constexpr builtin_t SKIP{"@skip", token_type_t::BUILTIN_SKIP};

constexpr builtin_t ASSERT{"@assert", token_type_t::BUILTIN_ASSERT};
constexpr builtin_t VERIFY{"@verify", token_type_t::BUILTIN_VERIFY};

constexpr builtin_t COMPILE_ERROR{"@compileError", token_type_t::BUILTIN_COMPILE_ERROR};
constexpr builtin_t CFG{"@cfg", token_type_t::BUILTIN_CFG};
constexpr builtin_t CFG_VALUE{"@cfgValue", token_type_t::BUILTIN_CFG_VALUE};

// A declaration-level attribute, not a callable builtin: `@discardable const f := fn ...`.
constexpr builtin_t DISCARDABLE{"@discardable", token_type_t::BUILTIN_DISCARDABLE};

constexpr auto ALL_TOKEN_TYPES{
    stdx::enum_range<token_type_t::BUILTIN_ALIGN_CAST, token_type_t::BUILTIN_COMPILE_ERROR>()};

constexpr auto SPECIAL_FORM_TOKEN_TYPES{
    stdx::enum_range<token_type_t::BUILTIN_CFG, token_type_t::BUILTIN_CFG_VALUE>()};

} // namespace builtins

// Single source of truth for every `@builtin` spelling
constexpr std::array ALL_BUILTINS{
    builtins::ALIGN_CAST,
    builtins::PTR_CAST,
    builtins::BIT_CAST,
    builtins::CONST_CAST,
    builtins::VOLATILE_CAST,
    builtins::AS,
    builtins::INT_FROM_PTR,
    builtins::PTR_FROM_INT,
    builtins::PTR_FROM_ARRAY,
    builtins::SLICE_FROM_PTR,
    builtins::FIELD_PARENT_PTR,
    builtins::ALIGN_OF,
    builtins::SIZE_OF,
    builtins::BIT_SIZE_OF,
    builtins::TYPE_OF,
    builtins::THIS,
    builtins::TAG_NAME,
    builtins::TYPE_NAME,
    builtins::MEMCPY,
    builtins::MEMSET,
    builtins::MEMMOVE,
    builtins::MUL_ADD,
    builtins::CLZ,
    builtins::CTZ,
    builtins::POP_COUNT,
    builtins::ABS,
    builtins::MIN,
    builtins::MAX,
    builtins::DIV_TRUNC,
    builtins::DIV_FLOOR,
    builtins::REM,
    builtins::MOD,
    builtins::ADD_WITH_OVERFLOW,
    builtins::SUB_WITH_OVERFLOW,
    builtins::MUL_WITH_OVERFLOW,
    builtins::SHL_WITH_OVERFLOW,
    builtins::C_VA_START,
    builtins::C_VA_ARG,
    builtins::C_VA_COPY,
    builtins::C_VA_END,
    builtins::ALIGNAS,
    builtins::TARGET_OS,
    builtins::TARGET_ARCH,
    builtins::TARGET_TRIPLE,
    builtins::TARGET_ABI,
    builtins::TARGET_PTR_BITS,
    builtins::TARGET_ENDIAN,
    builtins::TARGET_FAMILY,
    builtins::SET_EVAL_RECURSION_LIMIT,
    builtins::SET_MAIN_SYMBOL,
    builtins::PANIC,
    builtins::TRAP,
    builtins::FN_CTX,
    builtins::SRC,
    builtins::IMPLEMENTS,
    builtins::EXPECT,
    builtins::REQUIRE,
    builtins::SKIP,
    builtins::ASSERT,
    builtins::VERIFY,
    builtins::DYN_CAST,
    builtins::CFG,
    builtins::CFG_VALUE,
    builtins::DISCARDABLE,
    builtins::COMPILE_ERROR,
};

[[nodiscard]] auto get_builtin_opt(token_type_t tt) noexcept -> stdx::option<std::string_view>;
[[nodiscard]] auto get_builtin_opt(std::string_view sv) noexcept -> stdx::option<token_type_t>;

} // namespace ghoti::syntax
