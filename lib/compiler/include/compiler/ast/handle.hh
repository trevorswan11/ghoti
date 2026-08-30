#pragma once

#include <vector>

#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"

namespace ghoti::ast {

namespace detail {

// Checks if the provided kind is compatible with any allowed kind
template <node_kind... AllowedKinds>
[[nodiscard]] constexpr auto any_kind_compatible(node_kind kind) noexcept -> bool {
    return ((kind == AllowedKinds) || ...);
}

} // namespace detail

// A pseudo-type-safe wrapper around nodes, defaulting to an invalid state
template <node_kind... AllowedKinds> class handle {
  public:
    constexpr explicit handle(node_id id) noexcept : id_{id} {
        ASSERT(id.is_valid(), "Attempt to create Handle from invalid NodeID");
        ASSERT(any_compatible(id.get_kind()), "Assigned invalid NodeKind to Handle");
    }

    template <node_kind... OtherKinds>
        requires(detail::any_kind_compatible<AllowedKinds...>(OtherKinds) || ...)
    constexpr handle(handle<OtherKinds...> other) noexcept : id_{*other} {
        ASSERT(other.is_valid(), "Attempt to create Handle from invalid Handle");
    }

    [[nodiscard]] constexpr static auto any_compatible(node_kind kind) noexcept -> bool {
        return detail::any_kind_compatible<AllowedKinds...>(kind);
    }

    [[nodiscard]] constexpr auto operator*() const noexcept -> node_id { return id_; }
    [[nodiscard]] constexpr auto operator->(this auto&& self) noexcept { return &self.id_; }

    [[nodiscard]] constexpr auto        is_valid() const noexcept -> bool { return id_.is_valid(); }
    [[nodiscard]] static constexpr auto make_invalid() noexcept -> handle {
        return handle{detail::INVALID_ID};
    }

    [[nodiscard]] constexpr auto get_index() const noexcept -> usize { return id_.get_index(); }
    [[nodiscard]] constexpr      operator node_id() const noexcept { return id_; } // NOLINT

    template <NodeData N> [[nodiscard]] constexpr auto is() const noexcept -> bool {
        return id_.is<N>();
    }

    template <NodeData... Ns> [[nodiscard]] constexpr auto any() const noexcept -> bool {
        return id_.any<Ns...>();
    }

  private:
    constexpr explicit handle(u64 raw) noexcept : id_{raw} {}

  private:
    node_id id_{node_id::make_invalid()};
};

using expr_handle = handle<node_kind::ARRAY_EXPRESSION,
                           node_kind::ASM_EXPRESSION,
                           node_kind::ASSIGNMENT_EXPRESSION,
                           node_kind::BINARY_EXPRESSION,
                           node_kind::CALL_EXPRESSION,
                           node_kind::DO_WHILE_LOOP_EXPRESSION,
                           node_kind::DOT_EXPRESSION,
                           node_kind::ENUM_EXPRESSION,
                           node_kind::FOR_LOOP_EXPRESSION,
                           node_kind::FUNCTION_EXPRESSION,
                           node_kind::IDENTIFIER_EXPRESSION,
                           node_kind::IF_EXPRESSION,
                           node_kind::INDEX_EXPRESSION,
                           node_kind::INFINITE_LOOP_EXPRESSION,
                           node_kind::CFG_VALUE_EXPRESSION,
                           node_kind::INITIALIZER_EXPRESSION,
                           node_kind::LABEL_EXPRESSION,
                           node_kind::MATCH_EXPRESSION,
                           node_kind::UNARY_EXPRESSION,
                           node_kind::UNWRAP_EXPRESSION,
                           node_kind::REFERENCE_EXPRESSION,
                           node_kind::DEREFERENCE_EXPRESSION,
                           node_kind::ADDRESS_OF_EXPRESSION,
                           node_kind::IMPLICIT_ACCESS_EXPRESSION,
                           node_kind::STRING_EXPRESSION,
                           node_kind::I32_EXPRESSION,
                           node_kind::I64_EXPRESSION,
                           node_kind::ISIZE_EXPRESSION,
                           node_kind::U32_EXPRESSION,
                           node_kind::U64_EXPRESSION,
                           node_kind::USIZE_EXPRESSION,
                           node_kind::U8_EXPRESSION,
                           node_kind::F32_EXPRESSION,
                           node_kind::F64_EXPRESSION,
                           node_kind::BOOL_EXPRESSION,
                           node_kind::VOID_EXPRESSION,
                           node_kind::UNDEFINED_EXPRESSION,
                           node_kind::NULLPTR_EXPRESSION,
                           node_kind::UNREACHABLE_EXPRESSION,
                           node_kind::RANGE_EXPRESSION,
                           node_kind::MODULE_ACCESS_EXPRESSION,
                           node_kind::STRUCT_EXPRESSION,
                           node_kind::UNION_EXPRESSION,
                           node_kind::WHILE_LOOP_EXPRESSION>;

using identifier_handle        = handle<node_kind::IDENTIFIER_EXPRESSION>;
using discardable_ident_handle = handle<node_kind::IDENTIFIER_EXPRESSION, node_kind::DISCARDED>;
using implicit_access_handle   = handle<node_kind::IMPLICIT_ACCESS_EXPRESSION>;
using string_handle            = handle<node_kind::STRING_EXPRESSION>;
using outer_access_handle      = handle<node_kind::IDENTIFIER_EXPRESSION,
                                        node_kind::MODULE_ACCESS_EXPRESSION,
                                        node_kind::DOT_EXPRESSION>;

using match_pattern_handle = handle<node_kind::ARRAY_EXPRESSION,
                                    node_kind::CALL_EXPRESSION,
                                    node_kind::DOT_EXPRESSION,
                                    node_kind::IDENTIFIER_EXPRESSION,
                                    node_kind::INDEX_EXPRESSION,
                                    node_kind::UNARY_EXPRESSION,
                                    node_kind::REFERENCE_EXPRESSION,
                                    node_kind::DEREFERENCE_EXPRESSION,
                                    node_kind::ADDRESS_OF_EXPRESSION,
                                    node_kind::IMPLICIT_ACCESS_EXPRESSION,
                                    node_kind::STRING_EXPRESSION,
                                    node_kind::I32_EXPRESSION,
                                    node_kind::I64_EXPRESSION,
                                    node_kind::ISIZE_EXPRESSION,
                                    node_kind::U32_EXPRESSION,
                                    node_kind::U64_EXPRESSION,
                                    node_kind::USIZE_EXPRESSION,
                                    node_kind::U8_EXPRESSION,
                                    node_kind::F32_EXPRESSION,
                                    node_kind::F64_EXPRESSION,
                                    node_kind::BOOL_EXPRESSION,
                                    node_kind::MODULE_ACCESS_EXPRESSION,
                                    node_kind::DISCARDED>;

using stmt_handle = handle<node_kind::BLOCK_STATEMENT,
                           node_kind::CFG_STATEMENT,
                           node_kind::DECL_STATEMENT,
                           node_kind::DEFER_STATEMENT,
                           node_kind::DISCARD_STATEMENT,
                           node_kind::EXPRESSION_STATEMENT,
                           node_kind::IMPORT_STATEMENT,
                           node_kind::RETURN_STATEMENT,
                           node_kind::BREAK_STATEMENT,
                           node_kind::CONTINUE_STATEMENT,
                           node_kind::TEST_STATEMENT,
                           node_kind::USING_STATEMENT>;

struct decl_stmt;
using decl_handle   = handle<node_kind::DECL_STATEMENT>;
using block_handle  = handle<node_kind::BLOCK_STATEMENT>;
using import_handle = ast::handle<ast::node_kind::IMPORT_STATEMENT>;
using import_payload_handle =
    handle<node_kind::STRING_EXPRESSION, node_kind::IDENTIFIER_EXPRESSION>;

using member_handle =
    handle<node_kind::DECL_STATEMENT, node_kind::IMPORT_STATEMENT, node_kind::USING_STATEMENT>;
using member_list = std::vector<member_handle>;

using labeled_node_handle = handle<node_kind::DO_WHILE_LOOP_EXPRESSION,
                                   node_kind::FOR_LOOP_EXPRESSION,
                                   node_kind::IF_EXPRESSION,
                                   node_kind::INFINITE_LOOP_EXPRESSION,
                                   node_kind::MATCH_EXPRESSION,
                                   node_kind::WHILE_LOOP_EXPRESSION,
                                   node_kind::BLOCK_STATEMENT>;

} // namespace ghoti::ast

template <ghoti::ast::node_kind... Kinds> struct stdx::nullable<ghoti::ast::handle<Kinds...>> {
    [[nodiscard]] static constexpr auto invalid() noexcept -> ghoti::ast::handle<Kinds...> {
        return ghoti::ast::handle<Kinds...>::make_invalid();
    }

    [[nodiscard]] static constexpr auto is_valid(ghoti::ast::handle<Kinds...> handle) noexcept
        -> bool {
        return handle.is_valid();
    }
};
