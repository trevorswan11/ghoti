#pragma once

#include <string>
#include <type_traits>

#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/ast/handle.hh"
#include "compiler/syntax/error.hh"

namespace ghoti {

namespace syntax { class parser; } // namespace syntax

namespace ast {

#define DECLARE_PRIMITIVE_EXPRESSION(NodeType, ValueType)       \
    struct NodeType {                                           \
        using value_type = ValueType;                           \
        value_type                value;                        \
        [[nodiscard]] static auto parse(syntax::parser& parser) \
            -> stdx::result<expr_handle, syntax::diagnostic>;   \
    };

DECLARE_PRIMITIVE_EXPRESSION(string_expr, std::string)
DECLARE_PRIMITIVE_EXPRESSION(i32_expr, i32)
DECLARE_PRIMITIVE_EXPRESSION(i64_expr, i64)
DECLARE_PRIMITIVE_EXPRESSION(isize_expr, isize)
DECLARE_PRIMITIVE_EXPRESSION(u32_expr, u32)
DECLARE_PRIMITIVE_EXPRESSION(u64_expr, u64)
DECLARE_PRIMITIVE_EXPRESSION(usize_expr, usize)
DECLARE_PRIMITIVE_EXPRESSION(u8_expr, u8)
DECLARE_PRIMITIVE_EXPRESSION(f32_expr, f32)
DECLARE_PRIMITIVE_EXPRESSION(f64_expr, f64)

#undef DECLARE_PRIMITIVE_EXPRESSION

template <typename T> struct is_valued_primitive : std::false_type {};
template <> struct is_valued_primitive<ast::string_expr> : std::true_type {};
template <> struct is_valued_primitive<ast::i32_expr> : std::true_type {};
template <> struct is_valued_primitive<ast::i64_expr> : std::true_type {};
template <> struct is_valued_primitive<ast::isize_expr> : std::true_type {};
template <> struct is_valued_primitive<ast::u32_expr> : std::true_type {};
template <> struct is_valued_primitive<ast::u64_expr> : std::true_type {};
template <> struct is_valued_primitive<ast::usize_expr> : std::true_type {};
template <> struct is_valued_primitive<ast::u8_expr> : std::true_type {};
template <> struct is_valued_primitive<ast::f32_expr> : std::true_type {};
template <> struct is_valued_primitive<ast::f64_expr> : std::true_type {};

// A primitive node with its value embedded in the data
template <typename T>
concept ValuedPrimitive = is_valued_primitive<T>::value;

struct bool_expr {
    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

struct void_expr {
    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

struct undefined_expr {
    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

} // namespace ast

} // namespace ghoti
