#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <type_traits>
#include <utility>

#include <ankerl/unordered_dense.h>
#include <stdx/assert.hh>
#include <stdx/fixed/hash_table.hh>
#include <stdx/hash.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

namespace ghoti::syntax {

enum class token_type_t : u8 {
    END,

    IDENT,

    INT_2,
    INT_8,
    INT_10,
    INT_16,
    LINT_2,
    LINT_8,
    LINT_10,
    LINT_16,
    ZINT_2,
    ZINT_8,
    ZINT_10,
    ZINT_16,

    UINT_2,
    UINT_8,
    UINT_10,
    UINT_16,
    ULINT_2,
    ULINT_8,
    ULINT_10,
    ULINT_16,
    UZINT_2,
    UZINT_8,
    UZINT_10,
    UZINT_16,

    F32,
    F64,
    STRING,
    U8,

    ASSIGN,
    WALRUS,
    PLUS,
    MINUS,
    STAR,
    SLASH,
    PERCENT,
    BANG,
    QUESTION,

    BW_AND,
    BW_OR,
    SHL,
    SHR,
    NOT,
    CARET,

    PLUS_ASSIGN,
    MINUS_ASSIGN,
    STAR_ASSIGN,
    SLASH_ASSIGN,
    PERCENT_ASSIGN,
    BW_AND_ASSIGN,
    BW_OR_ASSIGN,
    SHL_ASSIGN,
    SHR_ASSIGN,
    NOT_ASSIGN,
    XOR_ASSIGN,

    LT,
    LT_EQ,
    GT,
    GT_EQ,
    EQ,
    NEQ,

    DOT,
    DOT_DOT,
    DOT_DOT_EQ,
    FAT_ARROW,

    COMMENT,
    MULTILINE_STRING,
    NULL_TERMINATED,

    COMMA,
    COLON,
    SEMICOLON,
    COLON_COLON,
    ELLIPSIS,

    LPAREN,
    RPAREN,
    LBRACE,
    RBRACE,
    LBRACKET,
    RBRACKET,

    SINGLE_QUOTE,
    UNDERSCORE,
    USING,
    AND_MUT,
    CARET_MUT,

    FUNCTION,
    VAR,
    CONSTANT,
    CONSTEXPR,
    STRUCT,
    ENUM,
    UNION,
    BOOLEAN_TRUE,
    BOOLEAN_FALSE,
    BOOLEAN_AND,
    BOOLEAN_OR,
    IF,
    ELSE,
    MATCH,
    RETURN,
    LOOP,
    FOR,
    WHILE,
    CONTINUE,
    BREAK,
    IMPORT,
    TYPE_TYPE,
    AUTO_TYPE,
    OPAQUE_TYPE,
    DO,
    AS,
    DEFER,
    TEST,
    UNDEFINED,
    ASM,
    IMPL,
    INTERFACE,
    DYN,

    I8_TYPE,
    I16_TYPE,
    I32_TYPE,
    I64_TYPE,
    ISIZE_TYPE,
    U16_TYPE,
    U32_TYPE,
    U64_TYPE,
    USIZE_TYPE,
    U8_TYPE,
    F32_TYPE,
    F64_TYPE,
    BOOL_TYPE,
    VOID_TYPE,

    PUBLIC,
    EXTERN,
    EXPORT,
    THREADLOCAL,
    WEAK,
    NAKED,
    CALLCONV,
    VOLATILE,
    MUT,
    MOVE,
    PACKED,
    NORETURN,
    NULLPTR,
    UNREACHABLE,

    BUILTIN_ALIGN_CAST,
    BUILTIN_PTR_CAST,
    BUILTIN_BIT_CAST,
    BUILTIN_CONST_CAST,
    BUILTIN_VOLATILE_CAST,
    BUILTIN_AS,
    BUILTIN_INT_FROM_PTR,
    BUILTIN_PTR_FROM_INT,
    BUILTIN_PTR_FROM_ARRAY,
    BUILTIN_SLICE_FROM_PTR,
    BUILTIN_FIELD_PARENT_PTR,
    BUILTIN_ALIGN_OF,
    BUILTIN_SIZE_OF,
    BUILTIN_TYPE_OF,
    BUILTIN_THIS,
    BUILTIN_TAG_NAME,
    BUILTIN_TYPE_NAME,
    BUILTIN_MEMCPY,
    BUILTIN_MEMSET,
    BUILTIN_MEMMOVE,
    BUILTIN_MUL_ADD,
    BUILTIN_CLZ, // Count leading zeroes
    BUILTIN_CTZ, // Count trailing zeroes
    BUILTIN_POP_COUNT,
    BUILTIN_ABS,
    BUILTIN_MIN,
    BUILTIN_MAX,
    BUILTIN_DIV_TRUNC,
    BUILTIN_DIV_FLOOR,
    BUILTIN_REM,
    BUILTIN_MOD,
    BUILTIN_ADD_WITH_OVERFLOW,
    BUILTIN_SUB_WITH_OVERFLOW,
    BUILTIN_MUL_WITH_OVERFLOW,
    BUILTIN_SHL_WITH_OVERFLOW,
    BUILTIN_C_VA_START,
    BUILTIN_C_VA_ARG,
    BUILTIN_C_VA_COPY,
    BUILTIN_C_VA_END,
    BUILTIN_ALIGNAS,
    BUILTIN_TARGET_OS,
    BUILTIN_TARGET_ARCH,
    BUILTIN_TARGET_TRIPLE,
    BUILTIN_TARGET_ABI,
    BUILTIN_TARGET_PTR_BITS,
    BUILTIN_TARGET_ENDIAN,
    BUILTIN_TARGET_FAMILY,
    BUILTIN_SET_EVAL_RECURSION_LIMIT,
    BUILTIN_SET_MAIN_SYMBOL,
    BUILTIN_PANIC,
    BUILTIN_TRAP,
    BUILTIN_FN_CTX,
    BUILTIN_SRC,
    BUILTIN_EXPECT,
    BUILTIN_REQUIRE,
    BUILTIN_SKIP,
    BUILTIN_COMPILE_ERROR,

    BUILTIN_CFG,
    BUILTIN_CFG_VALUE,

    BUILTIN_DISCARDABLE,

    ILLEGAL,
};

enum class semicolon_behavior : u8 {
    REQUIRE,
    ALLOWED,
    DISALLOW,
};

enum class numeric_base : u8 {
    BINARY      = 2,
    OCTAL       = 8,
    DECIMAL     = 10,
    HEXADECIMAL = 16,
};

[[nodiscard]] auto base_idx(numeric_base base) noexcept -> int;
[[nodiscard]] auto digit_in_base(char c, numeric_base base) noexcept -> bool;

namespace token_type {

enum class integer_category : u8 {
    SIGNED_BASE,
    SIGNED_WIDE,
    SIGNED_SIZE,
    UNSIGNED_BASE,
    UNSIGNED_WIDE,
    UNSIGNED_SIZE,
};

[[nodiscard]] consteval auto to_int_category(token_type_t tt) noexcept -> integer_category {
    switch (tt) {
    case token_type_t::INT_2:
    case token_type_t::INT_8:
    case token_type_t::INT_10:
    case token_type_t::INT_16:   return integer_category::SIGNED_BASE;
    case token_type_t::LINT_2:
    case token_type_t::LINT_8:
    case token_type_t::LINT_10:
    case token_type_t::LINT_16:  return integer_category::SIGNED_WIDE;
    case token_type_t::ZINT_2:
    case token_type_t::ZINT_8:
    case token_type_t::ZINT_10:
    case token_type_t::ZINT_16:  return integer_category::SIGNED_SIZE;
    case token_type_t::UINT_2:
    case token_type_t::UINT_8:
    case token_type_t::UINT_10:
    case token_type_t::UINT_16:  return integer_category::UNSIGNED_BASE;
    case token_type_t::ULINT_2:
    case token_type_t::ULINT_8:
    case token_type_t::ULINT_10:
    case token_type_t::ULINT_16: return integer_category::UNSIGNED_WIDE;
    case token_type_t::UZINT_2:
    case token_type_t::UZINT_8:
    case token_type_t::UZINT_10:
    case token_type_t::UZINT_16: return integer_category::UNSIGNED_SIZE;
    default:                     UNREACHABLE("Int-ness is assumed in this function");
    }
}

[[nodiscard]] auto to_base(token_type_t tt) noexcept -> stdx::option<numeric_base>;
[[nodiscard]] auto misc_from_char(char c) noexcept -> stdx::option<token_type_t>;

[[nodiscard]] constexpr auto is_i32(token_type_t tt) noexcept -> bool {
    return token_type_t::INT_2 <= tt && tt <= token_type_t::INT_16;
}

[[nodiscard]] constexpr auto is_i64(token_type_t tt) noexcept -> bool {
    return token_type_t::LINT_2 <= tt && tt <= token_type_t::LINT_16;
}

[[nodiscard]] constexpr auto is_isize_int(token_type_t tt) noexcept -> bool {
    return token_type_t::ZINT_2 <= tt && tt <= token_type_t::ZINT_16;
}

[[nodiscard]] constexpr auto is_u32(token_type_t tt) noexcept -> bool {
    return token_type_t::UINT_2 <= tt && tt <= token_type_t::UINT_16;
}

[[nodiscard]] constexpr auto is_u64(token_type_t tt) noexcept -> bool {
    return token_type_t::ULINT_2 <= tt && tt <= token_type_t::ULINT_16;
}

[[nodiscard]] constexpr auto is_usize_int(token_type_t tt) noexcept -> bool {
    return token_type_t::UZINT_2 <= tt && tt <= token_type_t::UZINT_16;
}

[[nodiscard]] constexpr auto is_int(token_type_t tt) noexcept -> bool {
    return token_type_t::INT_2 <= tt && tt <= token_type_t::UZINT_16;
}

[[nodiscard]] constexpr auto is_number(token_type_t tt) noexcept -> bool {
    return is_int(tt) || tt == token_type_t::F32 || tt == token_type_t::F64;
}

[[nodiscard]] auto is_primitive(token_type_t type) noexcept -> bool;

// Check whether the token is an ident, primitive type, or builtin function.
[[nodiscard]] auto is_valid_ident(token_type_t type) noexcept -> bool;
[[nodiscard]] auto is_valid_identifier_name(std::string_view name) noexcept -> bool;

auto suffix_length(token_type_t tt) noexcept -> usize;

[[nodiscard]] constexpr auto get_compound_base_op(syntax::token_type_t tok) noexcept
    -> stdx::option<syntax::token_type_t> {
    switch (tok) {
    case syntax::token_type_t::PLUS_ASSIGN:    return syntax::token_type_t::PLUS;
    case syntax::token_type_t::MINUS_ASSIGN:   return syntax::token_type_t::MINUS;
    case syntax::token_type_t::STAR_ASSIGN:    return syntax::token_type_t::STAR;
    case syntax::token_type_t::SLASH_ASSIGN:   return syntax::token_type_t::SLASH;
    case syntax::token_type_t::PERCENT_ASSIGN: return syntax::token_type_t::PERCENT;
    case syntax::token_type_t::BW_AND_ASSIGN:  return syntax::token_type_t::BW_AND;
    case syntax::token_type_t::BW_OR_ASSIGN:   return syntax::token_type_t::BW_OR;
    case syntax::token_type_t::XOR_ASSIGN:     return syntax::token_type_t::CARET;
    case syntax::token_type_t::SHL_ASSIGN:     return syntax::token_type_t::SHL;
    case syntax::token_type_t::SHR_ASSIGN:     return syntax::token_type_t::SHR;
    default:                                   return stdx::none;
    }
}

} // namespace token_type

struct typed_identifier {
    std::string_view name;
    token_type_t     type;
};

// Helper for ADL tuple get in stdx::fixed::hash_map
template <std::size_t I>
[[nodiscard]] constexpr auto get(const syntax::typed_identifier& typed) noexcept -> auto& {
    if constexpr (I == 0) {
        return typed.name;
    } else if constexpr (I == 1) {
        return typed.type;
    }
}

} // namespace ghoti::syntax

template <> struct ankerl::unordered_dense::hash<ghoti::syntax::typed_identifier> {
    using is_avalanching   = void;
    using typed_identifier = ghoti::syntax::typed_identifier;

    [[nodiscard]] auto operator()(const typed_identifier& type) const noexcept {
        stdx::hasher hasher{type.type};
        hasher.combine(type.name);
        return hasher.finalize();
    }
};

template <>
struct std::tuple_size<ghoti::syntax::typed_identifier> : std::integral_constant<std::size_t, 2> {};

template <> struct std::tuple_element<0, ghoti::syntax::typed_identifier> {
    using type = std::string_view;
};

template <> struct std::tuple_element<1, ghoti::syntax::typed_identifier> {
    using type = ghoti::syntax::token_type_t;
};
