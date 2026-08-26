#pragma once

#include <string_view>
#include <utility>
#include <vector>

#include <magic_enum/magic_enum.hpp>
#include <stdx/enum.hh>
#include <stdx/iterator.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/syntax/error.hh"
#include "compiler/syntax/token_type.hh"

namespace ghoti {

namespace syntax { class parser; } // namespace syntax

namespace ast {

struct block_stmt {
    MAKE_ITERATOR(statements_t, std::vector<stmt_handle>, statements);

    statements_t statements;

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<stmt_handle, syntax::diagnostic>;
};

struct break_stmt {
    stdx::option<identifier_handle> label;
    stdx::option<expr_handle>       expression;

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<stmt_handle, syntax::diagnostic>;
};

struct continue_stmt {
    stdx::option<identifier_handle> label;

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<stmt_handle, syntax::diagnostic>;
};

enum class decl_modifiers : u8 {
    VARIABLE  = 1 << 0,
    CONSTANT  = 1 << 1,
    CONSTEXPR = 1 << 2,
    PUBLIC    = 1 << 3,
    EXTERN    = 1 << 4,
    EXPORT    = 1 << 5,
};

MAKE_ENUM_OPERATORS(decl_modifiers)

struct decl_stmt {
    identifier_handle              name;
    stdx::option<explicit_type_id> explicit_type;
    stdx::option<expr_handle>      value;
    decl_modifiers                 modifiers;
    stdx::option<string_handle>    extern_target;

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<stmt_handle, syntax::diagnostic>;

    [[nodiscard]] auto has_modifier(decl_modifiers flag) const noexcept -> bool {
        return modifiers_has(modifiers, flag);
    }

  private:
    static auto modifiers_has(decl_modifiers modifiers, decl_modifiers flag) noexcept -> bool {
        return static_cast<bool>(modifiers & flag);
    }
};

struct defer_stmt {
    stmt_handle deferred;

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<stmt_handle, syntax::diagnostic>;
};

struct discard_stmt {
    expr_handle discarded;

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<stmt_handle, syntax::diagnostic>;
};

struct expr_stmt {
    expr_handle expression;

    [[nodiscard]] static auto parse(syntax::parser& parser, syntax::semicolon_behavior behavior)
        -> stdx::result<stmt_handle, syntax::diagnostic>;
};

class AST;

struct import_stmt {
    import_payload_handle           payload;
    stdx::option<identifier_handle> alias;

    [[nodiscard]] static constexpr auto is_public(ast::node_id id) noexcept -> bool {
        return id.get_token_type() == syntax::token_type_t::PUBLIC;
    }

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<stmt_handle, syntax::diagnostic>;

    [[nodiscard]] auto get_name(const AST& tree) const noexcept
        -> std::pair<ast::identifier_handle, std::string_view>;
};

struct return_stmt {
    stdx::option<expr_handle> expression;

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<stmt_handle, syntax::diagnostic>;
};

struct test_stmt {
    stdx::option<string_handle> description;
    block_handle                block;

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<stmt_handle, syntax::diagnostic>;
};

struct using_stmt {
    identifier_handle alias;
    explicit_type_id  explicit_type;

    [[nodiscard]] static constexpr auto is_public(ast::node_id id) noexcept -> bool {
        return id.get_token_type() == syntax::token_type_t::PUBLIC;
    }

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<stmt_handle, syntax::diagnostic>;
};

} // namespace ast

} // namespace ghoti

template <> struct magic_enum::customize::enum_range<ghoti::ast::decl_modifiers> {
    static constexpr bool is_flags{true};
};
