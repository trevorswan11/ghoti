#pragma once

#include <concepts>

#include <stdx/types.hh>
#include <stdx/variant.hh>

namespace ghoti::ast {

enum class node_kind : u8 {
    ARRAY_EXPRESSION,
    ASM_EXPRESSION,
    CALL_EXPRESSION,
    DO_WHILE_LOOP_EXPRESSION,
    ENUM_EXPRESSION,
    FOR_LOOP_EXPRESSION,
    FUNCTION_EXPRESSION,
    IDENTIFIER_EXPRESSION,
    IF_EXPRESSION,
    INDEX_EXPRESSION,
    INFINITE_LOOP_EXPRESSION,
    CFG_VALUE_EXPRESSION,
    ASSIGNMENT_EXPRESSION,
    BINARY_EXPRESSION,
    DOT_EXPRESSION,
    RANGE_EXPRESSION,
    INITIALIZER_EXPRESSION,
    LABEL_EXPRESSION,
    MATCH_EXPRESSION,
    UNARY_EXPRESSION,
    UNWRAP_EXPRESSION,
    REFERENCE_EXPRESSION,
    ADDRESS_OF_EXPRESSION,
    DEREFERENCE_EXPRESSION,
    IMPLICIT_ACCESS_EXPRESSION,
    STRING_EXPRESSION,
    I32_EXPRESSION,
    I64_EXPRESSION,
    ISIZE_EXPRESSION,
    U32_EXPRESSION,
    U64_EXPRESSION,
    USIZE_EXPRESSION,
    U8_EXPRESSION,
    F32_EXPRESSION,
    F64_EXPRESSION,
    BOOL_EXPRESSION,
    VOID_EXPRESSION,
    UNDEFINED_EXPRESSION,
    NULLPTR_EXPRESSION,
    UNREACHABLE_EXPRESSION,
    MODULE_ACCESS_EXPRESSION,
    STRUCT_EXPRESSION,
    UNION_EXPRESSION,
    INTERFACE_EXPRESSION,
    WHILE_LOOP_EXPRESSION,

    BLOCK_STATEMENT,
    BREAK_STATEMENT,
    CFG_STATEMENT,
    CONTINUE_STATEMENT,
    DECL_STATEMENT,
    DEFER_STATEMENT,
    DISCARD_STATEMENT,
    EXPRESSION_STATEMENT,
    IMPL_STATEMENT,
    IMPORT_STATEMENT,
    RETURN_STATEMENT,
    TEST_STATEMENT,
    USING_STATEMENT,

    DISCARDED, // Represented by stdx::monostate
};

#define FOREACH_AST_EXPR(X) \
    X(array_expr)           \
    X(asm_expr)             \
    X(call_expr)            \
    X(do_while_loop_expr)   \
    X(enum_expr)            \
    X(for_loop_expr)        \
    X(function_expr)        \
    X(identifier_expr)      \
    X(if_expr)              \
    X(index_expr)           \
    X(infinite_loop_expr)   \
    X(cfg_value_expr)       \
    X(assignment_expr)      \
    X(binary_expr)          \
    X(dot_expr)             \
    X(range_expr)           \
    X(initializer_expr)     \
    X(label_expr)           \
    X(match_expr)           \
    X(unary_expr)           \
    X(unwrap_expr)          \
    X(reference_expr)       \
    X(dereference_expr)     \
    X(address_of_expr)      \
    X(implicit_access_expr) \
    X(string_expr)          \
    X(i32_expr)             \
    X(i64_expr)             \
    X(isize_expr)           \
    X(u32_expr)             \
    X(u64_expr)             \
    X(usize_expr)           \
    X(u8_expr)              \
    X(f32_expr)             \
    X(f64_expr)             \
    X(bool_expr)            \
    X(void_expr)            \
    X(undefined_expr)       \
    X(nullptr_expr)         \
    X(unreachable_expr)     \
    X(module_access_expr)   \
    X(struct_expr)          \
    X(union_expr)           \
    X(interface_expr)       \
    X(while_loop_expr)

#define FOREACH_AST_STMT(X) \
    X(block_stmt)           \
    X(break_stmt)           \
    X(cfg_stmt)             \
    X(continue_stmt)        \
    X(decl_stmt)            \
    X(defer_stmt)           \
    X(discard_stmt)         \
    X(expr_stmt)            \
    X(impl_stmt)            \
    X(import_stmt)          \
    X(return_stmt)          \
    X(test_stmt)            \
    X(using_stmt)

// DOES NOT INCLUDE DISCARDED
#define FOREACH_AST_NODE(X) \
    FOREACH_AST_EXPR(X)     \
    FOREACH_AST_STMT(X)

using discarded = stdx::monostate;

#define FWD_DECLARE_NODE_X(NodeType) struct NodeType;
FOREACH_AST_NODE(FWD_DECLARE_NODE_X)
#undef FWD_DECLARE_NODE_X

enum class explicit_type_kind : u8 {
    IDENT,
    SCOPE,
    DOT,
    CALL,
    FUNCTION,
    RECURSIVE,
    STRUCT,
    ENUM,
    UNION,
    INTERFACE,
    ARRAY,
};

#define FOREACH_AST_TYPE(X)   \
    X(identifier_expr)        \
    X(module_access_expr)     \
    X(dot_expr)               \
    X(call_expr)              \
    X(explicit_function_type) \
    X(explicit_type_id)       \
    X(struct_expr)            \
    X(enum_expr)              \
    X(union_expr)             \
    X(interface_expr)         \
    X(explicit_array_type)

class explicit_type_id;
struct explicit_array_type;
struct explicit_function_type;

template <typename T> struct node_kind_of;

template <typename T>
concept NodeData = requires {
    { node_kind_of<T>::value() } -> std::same_as<ast::node_kind>;
};

#define NODE_KIND_OF_TRAIT(Type, Kind)                                           \
    template <> struct node_kind_of<ast::Type> {                                 \
        [[nodiscard]] static constexpr auto value() noexcept -> ast::node_kind { \
            return ast::node_kind::Kind;                                         \
        }                                                                        \
    };

NODE_KIND_OF_TRAIT(array_expr, ARRAY_EXPRESSION)
NODE_KIND_OF_TRAIT(asm_expr, ASM_EXPRESSION)
NODE_KIND_OF_TRAIT(call_expr, CALL_EXPRESSION)
NODE_KIND_OF_TRAIT(do_while_loop_expr, DO_WHILE_LOOP_EXPRESSION)
NODE_KIND_OF_TRAIT(enum_expr, ENUM_EXPRESSION)
NODE_KIND_OF_TRAIT(for_loop_expr, FOR_LOOP_EXPRESSION)
NODE_KIND_OF_TRAIT(function_expr, FUNCTION_EXPRESSION)
NODE_KIND_OF_TRAIT(identifier_expr, IDENTIFIER_EXPRESSION)
NODE_KIND_OF_TRAIT(if_expr, IF_EXPRESSION)
NODE_KIND_OF_TRAIT(index_expr, INDEX_EXPRESSION)
NODE_KIND_OF_TRAIT(infinite_loop_expr, INFINITE_LOOP_EXPRESSION)
NODE_KIND_OF_TRAIT(cfg_value_expr, CFG_VALUE_EXPRESSION)
NODE_KIND_OF_TRAIT(assignment_expr, ASSIGNMENT_EXPRESSION)
NODE_KIND_OF_TRAIT(binary_expr, BINARY_EXPRESSION)
NODE_KIND_OF_TRAIT(dot_expr, DOT_EXPRESSION)
NODE_KIND_OF_TRAIT(range_expr, RANGE_EXPRESSION)
NODE_KIND_OF_TRAIT(initializer_expr, INITIALIZER_EXPRESSION)
NODE_KIND_OF_TRAIT(label_expr, LABEL_EXPRESSION)
NODE_KIND_OF_TRAIT(match_expr, MATCH_EXPRESSION)
NODE_KIND_OF_TRAIT(unary_expr, UNARY_EXPRESSION)
NODE_KIND_OF_TRAIT(unwrap_expr, UNWRAP_EXPRESSION)
NODE_KIND_OF_TRAIT(reference_expr, REFERENCE_EXPRESSION)
NODE_KIND_OF_TRAIT(dereference_expr, DEREFERENCE_EXPRESSION)
NODE_KIND_OF_TRAIT(address_of_expr, ADDRESS_OF_EXPRESSION)
NODE_KIND_OF_TRAIT(implicit_access_expr, IMPLICIT_ACCESS_EXPRESSION)
NODE_KIND_OF_TRAIT(string_expr, STRING_EXPRESSION)
NODE_KIND_OF_TRAIT(i32_expr, I32_EXPRESSION)
NODE_KIND_OF_TRAIT(i64_expr, I64_EXPRESSION)
NODE_KIND_OF_TRAIT(isize_expr, ISIZE_EXPRESSION)
NODE_KIND_OF_TRAIT(u32_expr, U32_EXPRESSION)
NODE_KIND_OF_TRAIT(u64_expr, U64_EXPRESSION)
NODE_KIND_OF_TRAIT(usize_expr, USIZE_EXPRESSION)
NODE_KIND_OF_TRAIT(u8_expr, U8_EXPRESSION)
NODE_KIND_OF_TRAIT(f32_expr, F32_EXPRESSION)
NODE_KIND_OF_TRAIT(f64_expr, F64_EXPRESSION)
NODE_KIND_OF_TRAIT(bool_expr, BOOL_EXPRESSION)
NODE_KIND_OF_TRAIT(void_expr, VOID_EXPRESSION)
NODE_KIND_OF_TRAIT(undefined_expr, UNDEFINED_EXPRESSION)
NODE_KIND_OF_TRAIT(nullptr_expr, NULLPTR_EXPRESSION)
NODE_KIND_OF_TRAIT(unreachable_expr, UNREACHABLE_EXPRESSION)
NODE_KIND_OF_TRAIT(module_access_expr, MODULE_ACCESS_EXPRESSION)
NODE_KIND_OF_TRAIT(struct_expr, STRUCT_EXPRESSION)
NODE_KIND_OF_TRAIT(union_expr, UNION_EXPRESSION)
NODE_KIND_OF_TRAIT(interface_expr, INTERFACE_EXPRESSION)
NODE_KIND_OF_TRAIT(while_loop_expr, WHILE_LOOP_EXPRESSION)

NODE_KIND_OF_TRAIT(block_stmt, BLOCK_STATEMENT)
NODE_KIND_OF_TRAIT(break_stmt, BREAK_STATEMENT)
NODE_KIND_OF_TRAIT(cfg_stmt, CFG_STATEMENT)
NODE_KIND_OF_TRAIT(continue_stmt, CONTINUE_STATEMENT)
NODE_KIND_OF_TRAIT(decl_stmt, DECL_STATEMENT)
NODE_KIND_OF_TRAIT(defer_stmt, DEFER_STATEMENT)
NODE_KIND_OF_TRAIT(discard_stmt, DISCARD_STATEMENT)
NODE_KIND_OF_TRAIT(expr_stmt, EXPRESSION_STATEMENT)
NODE_KIND_OF_TRAIT(impl_stmt, IMPL_STATEMENT)
NODE_KIND_OF_TRAIT(import_stmt, IMPORT_STATEMENT)
NODE_KIND_OF_TRAIT(return_stmt, RETURN_STATEMENT)
NODE_KIND_OF_TRAIT(test_stmt, TEST_STATEMENT)
NODE_KIND_OF_TRAIT(using_stmt, USING_STATEMENT)
NODE_KIND_OF_TRAIT(discarded, DISCARDED)

#undef KIND_OF_TRAIT

template <typename T> struct explicit_type_kind_of;

template <typename T>
concept ExplicitTypeData = requires {
    { explicit_type_kind_of<T>::value() } -> std::same_as<ast::explicit_type_kind>;
};

#define KIND_OF_TRAIT(Type, Kind)                                                         \
    template <> struct explicit_type_kind_of<ast::Type> {                                 \
        [[nodiscard]] static constexpr auto value() noexcept -> ast::explicit_type_kind { \
            return ast::explicit_type_kind::Kind;                                         \
        }                                                                                 \
    };

KIND_OF_TRAIT(identifier_expr, IDENT)
KIND_OF_TRAIT(module_access_expr, SCOPE)
KIND_OF_TRAIT(dot_expr, DOT)
KIND_OF_TRAIT(call_expr, CALL)
KIND_OF_TRAIT(explicit_function_type, FUNCTION)
KIND_OF_TRAIT(explicit_type_id, RECURSIVE)
KIND_OF_TRAIT(struct_expr, STRUCT)
KIND_OF_TRAIT(enum_expr, ENUM)
KIND_OF_TRAIT(union_expr, UNION)
KIND_OF_TRAIT(interface_expr, INTERFACE)
KIND_OF_TRAIT(explicit_array_type, ARRAY)

#undef KIND_OF_TRAIT

} // namespace ghoti::ast
