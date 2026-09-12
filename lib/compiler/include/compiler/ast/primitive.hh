#pragma once

#include <string_view>
#include <type_traits>

#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/ast/handle.hh"
#include "compiler/syntax/error.hh"
#include "compiler/syntax/token_type.hh"
#include "support/int128.hh"

namespace ghoti {

namespace syntax { class parser; } // namespace syntax

namespace ast {

struct string_expr {
    std::string_view          value;
    std::string_view          spelling;
    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

struct int_literal_expr {
    u128                 value{0};
    u16                  width{0}; // `width == 0` means width-less/coercible
    bool                 is_signed{false};
    bool                 is_size{false};
    syntax::numeric_base base{syntax::numeric_base::DECIMAL};
    std::string_view     spelling;

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

struct float_literal_expr {
    f64              value{0};
    u8               width{0}; // `width == 0` means coercible
    std::string_view spelling;

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

template <typename T> struct is_valued_primitive : std::false_type {};
template <> struct is_valued_primitive<ast::string_expr> : std::true_type {};

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

struct nullptr_expr {
    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

struct unreachable_expr {
    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

} // namespace ast

} // namespace ghoti
