#pragma once

#include <string_view>
#include <vector>

#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/variant.hh>

#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/syntax/error.hh"
#include "compiler/syntax/token_type.hh"

namespace ghoti {

namespace syntax { class parser; } // namespace syntax

namespace ast {

struct array_expr {
    stdx::option<expr_handle> size;
    bool                      null_terminated;
    explicit_type_id          item_explicit_type;
    std::vector<expr_handle>  items;

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

struct call_expr {
    using argument = stdx::variant<expr_handle, explicit_type_id>;

    expr_handle           function;
    std::vector<argument> arguments;

    [[nodiscard]] static auto parse(syntax::parser& parser, expr_handle function)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

struct do_while_loop_expr {
    block_handle block;
    expr_handle  condition;

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

struct enum_expr {
    struct enumeration {
        identifier_handle         name;
        stdx::option<expr_handle> value;
    };

    stdx::option<identifier_handle> underlying;
    std::vector<enumeration>        enumerations;
    bool                            non_exhaustive;
    member_list                     members;

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

struct for_loop_expr {
    struct capture {
        type_modifier            modifier;
        discardable_ident_handle payload;
    };

    std::vector<expr_handle>  iterables;
    std::vector<capture>      captures;
    block_handle              block;
    stdx::option<stmt_handle> non_break;

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

struct self_parameter {
    type_modifier     modifier;
    identifier_handle name;
};

} // namespace ast

} // namespace ghoti

template <> struct stdx::nullable<ghoti::ast::self_parameter> {
    [[nodiscard]] static constexpr auto invalid() noexcept -> ghoti::ast::self_parameter {
        return {.modifier = {}, .name = ghoti::ast::identifier_handle::make_invalid()};
    }

    [[nodiscard]] static constexpr auto is_valid(const ghoti::ast::self_parameter& self) noexcept
        -> bool {
        return self.name.is_valid();
    }
};

namespace ghoti::ast {

[[nodiscard]] auto try_parse_variadic_fn(syntax::parser& parser)
    -> stdx::result<bool, syntax::diagnostic>;

struct function_expr {
    struct parameter {
        identifier_handle name;
        explicit_type_id  explicit_type;
    };

    stdx::option<self_parameter> self;
    std::vector<parameter>       parameters;
    bool                         variadic;
    explicit_type_id             explicit_return_type;
    block_handle                 body;

    // Parse the function as a value. Meant for the parser LUT
    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

struct grouped_expr {
    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

struct identifier_expr {
    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;

    std::string_view name;
};

struct if_expr {
    bool                      constexpr_condition;
    expr_handle               condition;
    stmt_handle               consequence;
    stdx::option<stmt_handle> alternate;

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

struct index_expr {
    expr_handle array;
    expr_handle index;

    [[nodiscard]] static auto parse(syntax::parser& parser, expr_handle array)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

struct infinite_loop_expr {
    block_handle block;

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

#define DECLARE_INFIX_EXPRESSION(Type)                                           \
    struct Type {                                                                \
        expr_handle               lhs;                                           \
        expr_handle               rhs;                                           \
        [[nodiscard]] static auto parse(syntax::parser& parser, expr_handle lhs) \
            -> stdx::result<expr_handle, syntax::diagnostic>;                    \
    };

// The operator is stored in the nodes id
DECLARE_INFIX_EXPRESSION(assignment_expr)

// The operator is stored in the nodes id
DECLARE_INFIX_EXPRESSION(binary_expr)

struct dot_expr {
    expr_handle       object;
    identifier_handle member;

    [[nodiscard]] static auto parse(syntax::parser& parser, expr_handle outer)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

// The operator is stored in the nodes id
DECLARE_INFIX_EXPRESSION(range_expr)

#undef DECLARE_INFIX_EXPRESSION

struct initializer_expr {
    struct initializer {
        implicit_access_handle member;
        expr_handle            value;
    };

    stdx::option<expr_handle> object_type;
    std::vector<initializer>  initializers;

    // Parse assuming an object is present. Meant for the parser LUT
    [[nodiscard]] static auto parse(syntax::parser& parser, expr_handle object)
        -> stdx::result<expr_handle, syntax::diagnostic> {
        return parse(parser, stdx::option<expr_handle>{object});
    }

    // Parse the expression with a potentially empty object
    [[nodiscard]] static auto parse(syntax::parser& parser, stdx::option<expr_handle> object)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

struct label_expr {
    identifier_handle   name;
    labeled_node_handle body;

    [[nodiscard]] static auto parse(syntax::parser& parser, expr_handle name)
        -> stdx::result<expr_handle, syntax::diagnostic>;

  private:
    [[nodiscard]] static auto deconstruct_body(syntax::parser& parser, stmt_handle raw_stmt)
        -> stdx::result<labeled_node_handle, syntax::diagnostic>;
};

struct match_expr {
    struct arm {
        match_pattern_handle                   pattern;
        stdx::option<discardable_ident_handle> capture;
        stmt_handle                            dispatch;
    };

    expr_handle      matcher;
    std::vector<arm> arms;
    stdx::opt_size   catch_all_idx;

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

#define DECLARE_PREFIX_EXPRESSION(Type)                         \
    struct Type {                                               \
        expr_handle               rhs;                          \
        [[nodiscard]] static auto parse(syntax::parser& parser) \
            -> stdx::result<expr_handle, syntax::diagnostic>;   \
    };

DECLARE_PREFIX_EXPRESSION(unary_expr)
DECLARE_PREFIX_EXPRESSION(reference_expr)
DECLARE_PREFIX_EXPRESSION(dereference_expr)
DECLARE_PREFIX_EXPRESSION(address_of_expr)

struct implicit_access_expr {
    identifier_handle member;

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

#undef DECLARE_PREFIX_EXPRESSION

struct module_access_expr {
    outer_access_handle outer;
    identifier_handle   inner;

    [[nodiscard]] static auto parse(syntax::parser& parser, expr_handle outer)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

struct struct_expr {
    // Field publicity is baked into the identifier's token type
    struct field {
        identifier_handle         name;
        explicit_type_id          explicit_type;
        stdx::option<expr_handle> default_value;
        stdx::option<expr_handle> explicit_alignment;

        [[nodiscard]] constexpr auto is_public() const noexcept -> bool {
            return name->get_token_type() == syntax::token_type_t::PUBLIC;
        }
    };

    std::vector<field> fields;
    member_list        members;
    bool               is_extern{false};
    bool               is_packed{false};

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic> {
        return parse(parser, false, false);
    }
    [[nodiscard]] static auto parse(syntax::parser& parser, bool is_extern, bool is_packed)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

struct union_expr {
    struct field {
        identifier_handle         name;
        explicit_type_id          explicit_type;
        stdx::option<expr_handle> explicit_alignment;
    };

    std::vector<field> fields;
    member_list        members;
    bool               is_extern{false};

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic> {
        return parse(parser, false);
    }
    [[nodiscard]] static auto parse(syntax::parser& parser, bool is_extern)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

[[nodiscard]] auto parse_modified_struct_or_union(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic>;

struct while_loop_expr {
    expr_handle               condition;
    stdx::option<expr_handle> continuation;
    block_handle              block;
    stdx::option<stmt_handle> non_break;

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

} // namespace ghoti::ast
