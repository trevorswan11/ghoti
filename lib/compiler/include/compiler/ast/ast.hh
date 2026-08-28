#pragma once

#include <vector>

#include <stdx/assert.hh>
#include <stdx/iterator.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"
#include "compiler/ast/traits.hh"
#include "compiler/ast/visitor.hh"
#include "compiler/syntax/token.hh"
#include "support/diagnostic.hh"

namespace ghoti::ast {

template <IndexableID ID, typename Data> struct data_pool_base {
    std::vector<Data>            pool;
    std::vector<source_location> locations;
    std::vector<source_location> end_locations;

    constexpr auto clear() noexcept -> void {
        pool.clear();
        locations.clear();
        end_locations.clear();
    }

    // `end_token` is the last token consumed while parsing this types
    constexpr auto emplace_back(const syntax::token_t& start_token,
                                const syntax::token_t& end_token,
                                Data&&                 data) -> u64 {
        return emplace_back(
            source_info<syntax::token_t>::get(start_token), end_token, std::forward<Data>(data));
    }

    // For types whose span starts earlier than the token that tags their id
    constexpr auto emplace_back(const source_location& start_loc,
                                const syntax::token_t& end_token,
                                Data&&                 data) -> u64 {
        const u64 index{pool.size()};
        pool.emplace_back(std::forward<Data>(data));
        locations.emplace_back(start_loc);
        end_locations.emplace_back(end_token.line, end_token.column + end_token.slice.size());
        return index;
    }
};

template <typename ID, typename Data> struct data_pool : public data_pool_base<ID, Data> {};
template <typename Data> struct data_pool<node_id, Data> : public data_pool_base<node_id, Data> {
    std::vector<node_id> roots;
    std::vector<u8>      paren_depths;

    // `end_token` is the last token consumed while parsing this node
    constexpr auto emplace_back(const syntax::token_t& start_token,
                                const syntax::token_t& end_token,
                                Data&&                 data) -> u64 {
        return emplace_back(
            source_info<syntax::token_t>::get(start_token), end_token, std::forward<Data>(data));
    }

    // For nodes whose span starts earlier than the token that tags their node_id
    constexpr auto emplace_back(const source_location& start_loc,
                                const syntax::token_t& end_token,
                                Data&&                 data) -> u64 {
        paren_depths.emplace_back(u8{0});
        return data_pool_base<node_id, Data>::emplace_back(
            start_loc, end_token, std::forward<Data>(data));
    }

    constexpr auto clear() noexcept -> void {
        roots.clear();
        data_pool_base<node_id, Data>::clear();
    }
};

// A collection of multiple AST tree structures
class AST {
  public:
    struct data_pool_sizes {
        usize nodes_size;
        usize types_size;
    };

  public:
    MAKE_UNALIASED_ITERATOR(std::vector<node_id>, nodes_.roots)

  public:
    AST()  = default;
    ~AST() = default;
    MAKE_MOVE_ONLY(AST)

    constexpr auto add_root(node_id id) -> void { nodes_.roots.emplace_back(id); }

    [[nodiscard]] constexpr auto get_pool_sizes() const noexcept -> data_pool_sizes {
        return {.nodes_size = nodes_.pool.size(), .types_size = explicit_types_.pool.size()};
    }

    template <NodeData Data>
    [[nodiscard]] constexpr auto add_node(const syntax::token_t& start_token,
                                          const syntax::token_t& end_token,
                                          Data&&                 data) -> node_id {
        constexpr auto kind{node_kind_of<Data>::value()};
        const auto     index{nodes_.emplace_back(start_token, end_token, std::forward<Data>(data))};
        return node_id{kind, start_token.type, index};
    }

    // For infix nodes whose operator starts after the node's true span start
    template <NodeData Data>
    [[nodiscard]] constexpr auto add_node(const source_location& span_start,
                                          const syntax::token_t& tag_token,
                                          const syntax::token_t& end_token,
                                          Data&&                 data) -> node_id {
        constexpr auto kind{node_kind_of<Data>::value()};
        const auto     index{nodes_.emplace_back(span_start, end_token, std::forward<Data>(data))};
        return node_id{kind, tag_token.type, index};
    }

    template <ExplicitTypeData Data>
    [[nodiscard]] constexpr auto add_type(const syntax::token_t& start_token,
                                          const syntax::token_t& end_token,
                                          type_modifier          mod,
                                          Data&&                 data) -> explicit_type_id {
        constexpr auto kind{explicit_type_kind_of<Data>::value()};
        const auto     index{
            explicit_types_.emplace_back(start_token, end_token, std::forward<Data>(data))};
        return explicit_type_id{kind, mod, start_token.type, index};
    }

    template <IndexableID ID>
    [[nodiscard]] constexpr auto location_of(ID id) const noexcept -> const source_location& {
        ASSERT(id.is_valid(), "Attempt to access invalid id");
        if constexpr (IndexableNodeID<ID>) {
            return nodes_.locations[id.get_index()];
        } else {
            return explicit_types_.locations[id.get_index()];
        }
    }

    // The position just past the node's last token; see `data_pool_base::emplace_back`
    template <IndexableID ID>
    [[nodiscard]] constexpr auto end_location_of(ID id) const noexcept -> const source_location& {
        ASSERT(id.is_valid(), "Attempt to access invalid id");
        if constexpr (IndexableNodeID<ID>) {
            return nodes_.end_locations[id.get_index()];
        } else {
            return explicit_types_.end_locations[id.get_index()];
        }
    }

    // How many redundant `( )` pairs enclosed this node in the source.
    [[nodiscard]] constexpr auto paren_depth_of(node_id id) const noexcept -> u8 {
        ASSERT(id.is_valid(), "Attempt to access invalid id");
        return nodes_.paren_depths[id.get_index()];
    }

    // Records one more `( )` pair around an already-added node.
    constexpr auto add_parenthesization(node_id id) noexcept -> void {
        ASSERT(id.is_valid(), "Attempt to access invalid id");
        nodes_.paren_depths[id.get_index()] += 1;
    }

    // Returns the node data at the provided id
    template <IndexableID ID>
    [[nodiscard]] constexpr auto operator[](this auto&& self, ID id) noexcept -> auto& {
        ASSERT(id.is_valid(), "Attempt to access invalid id");
        if constexpr (IndexableNodeID<ID>) {
            return self.nodes_.pool[id.get_index()];
        } else {
            return self.explicit_types_.pool[id.get_index()];
        }
    }

    // Returns the casted node at the requested index
    template <typename Data, IndexableID ID>
    [[nodiscard]] constexpr auto get_as(ID id) const noexcept -> const Data& {
        ASSERT(id.template is<Data>(), "Illegal node data retrieval");
        return operator[](id).template as<Data>();
    }

    // Returns the casted node data at the requested index if present
    template <typename Data, IndexableID ID>
    [[nodiscard]] constexpr auto get_as_opt(ID id) const noexcept -> stdx::option<const Data&> {
        return operator[](id).template as_opt<Data>();
    }

    // Mutable access to a node's payload. Caller asserts the stored alternative.
    template <typename Data, IndexableNodeID ID>
    [[nodiscard]] constexpr auto get_as_mut(ID id) noexcept -> Data& {
        ASSERT(id.template is<Data>(), "Illegal node data retrieval");
        return nodes_.pool[id.get_index()].template as<Data>();
    }

    [[nodiscard]] constexpr auto roots_mut() noexcept -> std::vector<node_id>& {
        return nodes_.roots;
    }

    constexpr auto clear() noexcept -> void {
        nodes_.clear();
        explicit_types_.clear();
    }

  private:
    data_pool<node_id, node_variant>          nodes_;
    data_pool<explicit_type_id, type_variant> explicit_types_;
};

} // namespace ghoti::ast
