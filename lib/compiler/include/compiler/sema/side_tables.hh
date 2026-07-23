#pragma once

#include <concepts>
#include <vector>

#include <stdx/assert.hh>
#include <stdx/option.hh>

#include "compiler/ast/ast.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/traits.hh"

#include <string>

namespace ghoti::sema {

namespace detail {

// An ID-indexable side table containing attached data
template <ast::IndexableID ID, typename T> struct side_table {
    std::vector<T> values;

    // Allows a handle wrapper of a node to be used for raw ID-based tables
    template <typename U>
        requires std::convertible_to<U, ID>
    [[nodiscard]] constexpr auto operator[](this auto&& self, U id) noexcept -> auto& {
        ASSERT(id.is_valid(), "Attempt to access invalid id");
        return self.values[static_cast<ID>(id).get_index()];
    }
};

} // namespace detail

class type;

struct side_tables {
    detail::side_table<ast::node_id, stdx::option<sema::type&>>          node_types;
    detail::side_table<ast::explicit_type_id, stdx::option<sema::type&>> explicit_types;

    // Indexed by the match arms' pattern
    detail::side_table<ast::node_id, stdx::option<sema::type&>> match_arm_types;

    // Call expression to monomorphized generic symbol name
    detail::side_table<ast::node_id, stdx::option<std::string>> generic_call_targets;

    // Allocates `size` slots in all backing vectors
    constexpr auto resize(const ast::AST::data_pool_sizes& sizes) -> void {
        node_types.values.resize(sizes.nodes_size);
        explicit_types.values.resize(sizes.types_size);
        match_arm_types.values.resize(sizes.nodes_size);
        generic_call_targets.values.resize(sizes.nodes_size);
    }
};

} // namespace ghoti::sema
