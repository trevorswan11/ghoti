#include "compiler/ast/id.hh"

#include <string_view>
#include <utility>

#include <stdx/enum.hh>
#include <stdx/fixed/enum_map.hh>
#include <stdx/result.hh>

#include "compiler/ast/kind.hh"
#include "compiler/syntax/error.hh"
#include "compiler/syntax/parser.hh"
#include "compiler/syntax/token.hh"
#include "compiler/syntax/token_type.hh"

namespace ghoti::ast {

namespace {

using name_mapping = std::pair<node_kind, std::string_view>;

constexpr auto NODE_NAMES{stdx::fixed::enum_map<node_kind, std::string_view>::from(
    "expression",
    name_mapping{node_kind::ARRAY_EXPRESSION, "array"},
    name_mapping{node_kind::ASM_EXPRESSION, "asm"},
    name_mapping{node_kind::CALL_EXPRESSION, "call"},
    name_mapping{node_kind::DO_WHILE_LOOP_EXPRESSION, "do-while loop"},
    name_mapping{node_kind::ENUM_EXPRESSION, "enum"},
    name_mapping{node_kind::FOR_LOOP_EXPRESSION, "for loop"},
    name_mapping{node_kind::FUNCTION_EXPRESSION, "function"},
    name_mapping{node_kind::IDENTIFIER_EXPRESSION, "identifier"},
    name_mapping{node_kind::IF_EXPRESSION, "if"},
    name_mapping{node_kind::INDEX_EXPRESSION, "index"},
    name_mapping{node_kind::INFINITE_LOOP_EXPRESSION, "infinite loop"},
    name_mapping{node_kind::CFG_VALUE_EXPRESSION, "@cfgValue"},
    name_mapping{node_kind::ASSIGNMENT_EXPRESSION, "assignment"},
    name_mapping{node_kind::BINARY_EXPRESSION, "binary"},
    name_mapping{node_kind::DOT_EXPRESSION, "dot"},
    name_mapping{node_kind::RANGE_EXPRESSION, "range"},
    name_mapping{node_kind::INITIALIZER_EXPRESSION, "initializer"},
    name_mapping{node_kind::LABEL_EXPRESSION, "label"},
    name_mapping{node_kind::MATCH_EXPRESSION, "match"},
    name_mapping{node_kind::UNARY_EXPRESSION, "unary"},
    name_mapping{node_kind::UNWRAP_EXPRESSION, "unwrap"},
    name_mapping{node_kind::REFERENCE_EXPRESSION, "reference-of"},
    name_mapping{node_kind::ADDRESS_OF_EXPRESSION, "address-of"},
    name_mapping{node_kind::DEREFERENCE_EXPRESSION, "dereference"},
    name_mapping{node_kind::IMPLICIT_ACCESS_EXPRESSION, "implicit access"},
    name_mapping{node_kind::STRING_EXPRESSION, "string"},
    name_mapping{node_kind::INT_LITERAL_EXPRESSION, "int-literal"},
    name_mapping{node_kind::FLOAT_LITERAL_EXPRESSION, "float-literal"},
    name_mapping{node_kind::BOOL_EXPRESSION, "bool"},
    name_mapping{node_kind::VOID_EXPRESSION, "void"},
    name_mapping{node_kind::UNDEFINED_EXPRESSION, "undefined"},
    name_mapping{node_kind::NULLPTR_EXPRESSION, "nullptr"},
    name_mapping{node_kind::MODULE_ACCESS_EXPRESSION, "module access"},
    name_mapping{node_kind::STRUCT_EXPRESSION, "struct"},
    name_mapping{node_kind::UNION_EXPRESSION, "union"},
    name_mapping{node_kind::INTERFACE_EXPRESSION, "interface"},
    name_mapping{node_kind::WHILE_LOOP_EXPRESSION, "while loop"},
    name_mapping{node_kind::BLOCK_STATEMENT, "statement"},
    name_mapping{node_kind::BREAK_STATEMENT, "statement"},
    name_mapping{node_kind::CFG_STATEMENT, "statement"},
    name_mapping{node_kind::CONTINUE_STATEMENT, "statement"},
    name_mapping{node_kind::DECL_STATEMENT, "statement"},
    name_mapping{node_kind::DEFER_STATEMENT, "statement"},
    name_mapping{node_kind::DISCARD_STATEMENT, "statement"},
    name_mapping{node_kind::EXPRESSION_STATEMENT, "statement"},
    name_mapping{node_kind::IMPL_STATEMENT, "statement"},
    name_mapping{node_kind::IMPORT_STATEMENT, "statement"},
    name_mapping{node_kind::RETURN_STATEMENT, "statement"},
    name_mapping{node_kind::TEST_STATEMENT, "statement"},
    name_mapping{node_kind::USING_STATEMENT, "statement"})};

} // namespace

auto node_id::display_name() const noexcept -> std::string_view { return NODE_NAMES[get_kind()]; }

namespace {

using token_type       = syntax::token_type_t;
using modifier         = type_modifier::modifier;
using modifier_mapping = std::pair<token_type, modifier>;

constexpr auto MODIFIERS{stdx::fixed::enum_map<token_type, modifier>::from(
    modifier::VALUE,
    modifier_mapping{token_type::BW_AND, modifier::REF},
    modifier_mapping{token_type::AND_MUT, modifier::MUT_REF},
    modifier_mapping{token_type::CARET, modifier::PTR},
    modifier_mapping{token_type::CARET_MUT, modifier::MUT_PTR},
    modifier_mapping{token_type::VOLATILE, modifier::VOLATILE})};

} // namespace

type_modifier::type_modifier(const syntax::token_t& tok) noexcept
    : underlying_{MODIFIERS[tok.type]} {}

auto type_modifier::parse(syntax::parser& parser, const syntax::token_t& current)
    -> stdx::result<type_modifier, syntax::diagnostic> {
    if (current.type == syntax::token_type_t::MUT) {
        TRY(parser.expect_peek(syntax::token_type_t::VOLATILE));
        return type_modifier{modifier::MUT_VOLATILE};
    }
    return type_modifier{current};
}

} // namespace ghoti::ast
