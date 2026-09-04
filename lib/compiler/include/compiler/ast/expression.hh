#pragma once

#include <algorithm>
#include <string_view>
#include <vector>

#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/variant.hh>

#include "compiler/ast/attributes.hh"
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
    bool                      mut_elements;
    explicit_type_id          item_explicit_type;
    std::vector<expr_handle>  items;
    bool                      is_type_expr{false}; // `[]T` / `[N]T` with no `{ ... }` initializer

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

struct asm_expr {
    // How each operand's constraint string is bound to a ghoti expression.
    //   inputs : `"{rdi}" = fd`     -> `value` holds the expression producing the input
    //   outputs: `"={rax}" = ret`   -> `value` holds the (mutable lvalue) to store the result into
    //   outputs: `"={rax}" = _`     -> `value` is none; the operand feeds `asm`'s own result
    struct operand {
        string_handle             constraint;
        stdx::option<expr_handle> value;

        [[nodiscard]] auto is_result_slot() const noexcept -> bool { return !value.has_value(); }
    };

    // Bare identifiers accepted inside the `options: ( ... )` clause.
    enum class option : u8 {
        VOLATILE,
        NORETURN,
        INTEL,
        ATT,
        ALIGN_STACK,
    };

    string_handle                  tmpl;
    stdx::option<explicit_type_id> result_type;
    std::vector<operand>           outputs;
    std::vector<operand>           inputs;
    std::vector<string_handle>     clobbers;
    std::vector<option>            options;

    [[nodiscard]] auto has_option(option opt) const noexcept -> bool {
        return std::ranges::contains(options, opt);
    }

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

struct do_while_loop_expr {
    block_handle block;
    expr_handle  condition;

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

// A `@cfg(...) ...` group inside an aggregate  body
template <typename Item> struct cfg_item_group {
    struct arm {
        stdx::option<expr_handle> predicate; // unset on the trailing bare `else`
        std::vector<Item>         items;
    };

    usize            position{0}; // index in the aggregate's item list this group expands at
    std::vector<arm> arms;
};

struct enum_expr {
    struct enumeration {
        identifier_handle         name;
        stdx::option<expr_handle> value;
    };

    using cfg_group        = cfg_item_group<enumeration>;
    using member_cfg_group = cfg_item_group<member_handle>;

    stdx::option<identifier_handle> underlying;
    std::vector<enumeration>        enumerations;
    std::vector<cfg_group>          cfg_groups;
    bool                            non_exhaustive;
    member_list                     members;
    std::vector<member_cfg_group>   member_cfg_groups;

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
        bool              is_constexpr{false};
    };

    // The parameter's `auto` type must infer to a type that implements every interface in
    // `interfaces`.
    struct impl_bound {
        u32                           param_index;
        std::vector<explicit_type_id> interfaces; // `w: impl I` / `w: impl (A + B)`
    };

    stdx::option<self_parameter> self;
    std::vector<parameter>       parameters;
    explicit_type_id             explicit_return_type;
    block_handle                 body;
    bool                         variadic;
    bool                         is_move{false};
    bool                         is_naked{false};
    calling_convention           conv{calling_convention::C};
    bool                         is_type_expr{false};
    std::vector<impl_bound>      impl_bounds{};

    // Parse the function as a value. Meant for the parser LUT
    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic> {
        return parse(parser, false, false);
    }
    [[nodiscard]] static auto parse(syntax::parser& parser, bool is_move, bool is_naked)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

// Consumes a leading `move` modifier before delegating to function_expr::parse
[[nodiscard]] auto parse_move_function_expr(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic>;

// Consumes a leading `naked` modifier before delegating to function_expr::parse
[[nodiscard]] auto parse_naked_function_expr(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic>;

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

// `@cfgValue(<pred>)` -> constexpr bool, or the guard form
// `@cfgValue(<pred> => <val>, ..., _ => <val>)` -> constexpr T
struct cfg_value_expr {
    struct guard {
        expr_handle predicate;
        expr_handle value;
    };

    stdx::option<expr_handle> predicate;
    std::vector<guard>        guards;
    stdx::option<expr_handle> fallback;

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

#undef DECLARE_INFIX_EXPRESSION

struct range_expr {
    stdx::option<expr_handle> lhs;
    stdx::option<expr_handle> rhs;

    [[nodiscard]] static auto parse(syntax::parser& parser, expr_handle lhs)
        -> stdx::result<expr_handle, syntax::diagnostic>;
    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

struct initializer_expr {
    struct initializer {
        stdx::option<implicit_access_handle> member; // Absent for a positional entry like an array
        expr_handle                          value;
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
        // One arm may list several patterns (`a, b, 1..8 => ...`) or a single discard
        std::vector<match_pattern_handle>      patterns;
        stdx::option<discardable_ident_handle> capture;
        // Only meaningful when `capture` holds a real (non-discarded) identifier
        type_modifier modifier;
        stmt_handle   dispatch;

        // The canonical pattern used for side-table keying and diagnostic locations.
        [[nodiscard]] auto primary_pattern() const noexcept -> match_pattern_handle {
            return patterns.front();
        }
    };

    expr_handle      matcher;
    std::vector<arm> arms;
    stdx::opt_size   catch_all_idx;
    bool             is_constexpr{false};

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

// The operator token (`QUESTION` / `BANG`) is stored in the node's id.
struct unwrap_expr {
    expr_handle operand;

    [[nodiscard]] static auto parse(syntax::parser& parser, expr_handle lhs)
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

    using cfg_group        = cfg_item_group<field>;
    using member_cfg_group = cfg_item_group<member_handle>;

    std::vector<field>            fields;
    std::vector<cfg_group>        cfg_groups;
    member_list                   members;
    std::vector<member_cfg_group> member_cfg_groups;
    bool                          is_extern{false};
    bool                          is_packed{false};

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

    using cfg_group        = cfg_item_group<field>;
    using member_cfg_group = cfg_item_group<member_handle>;

    std::vector<field>            fields;
    std::vector<cfg_group>        cfg_groups;
    member_list                   members;
    std::vector<member_cfg_group> member_cfg_groups;
    bool                          is_extern{false};
    bool                          is_packed{false};

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic> {
        return parse(parser, false, false);
    }
    [[nodiscard]] static auto parse(syntax::parser& parser, bool is_extern, bool is_packed)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

[[nodiscard]] auto parse_modified_struct_or_union(syntax::parser& parser)
    -> stdx::result<expr_handle, syntax::diagnostic>;

[[nodiscard]] auto parse_member_block(syntax::parser& parser)
    -> stdx::result<member_list, syntax::diagnostic>;

struct interface_expr {
    // `Name: type;` (required) or `Name: type = T;` (defaulted)
    struct assoc_type {
        identifier_handle              name;
        explicit_type_id               annotation;
        stdx::option<explicit_type_id> default_type;
    };

    // `const N: T;` (required) or `const N: T = expr;` (defaulted).
    struct assoc_const {
        identifier_handle         name;
        explicit_type_id          explicit_type;
        stdx::option<expr_handle> default_value;
    };

    // Bodyless signature is a requirement; one with a body is a default method.
    struct method {
        identifier_handle name; // Publicity hides in here
        function_handle   signature;

        [[nodiscard]] constexpr auto is_public() const noexcept -> bool {
            return name->get_token_type() == syntax::token_type_t::PUBLIC;
        }
    };

    std::vector<assoc_type>  assoc_types;
    std::vector<assoc_const> assoc_consts;
    std::vector<method>      methods;

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

struct while_loop_expr {
    expr_handle               condition;
    stdx::option<expr_handle> continuation;
    block_handle              block;
    stdx::option<stmt_handle> non_break;

    [[nodiscard]] static auto parse(syntax::parser& parser)
        -> stdx::result<expr_handle, syntax::diagnostic>;
};

} // namespace ghoti::ast
