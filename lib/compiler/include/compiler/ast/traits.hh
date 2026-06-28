#pragma once

#include <type_traits>

#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"

namespace ghoti::ast {

template <typename T> struct is_node_id : std::false_type {};
template <> struct is_node_id<ast::node_id> : std::true_type {};

template <typename T>
concept NodeId = is_node_id<T>::value;

template <typename T> struct is_node_handle : std::false_type {};
template <ast::node_kind... Kinds> struct is_node_handle<ast::handle<Kinds...>> : std::true_type {};

template <typename T>
concept NodeHandle = is_node_handle<T>::value;

// Represents either a handle or an id that can be used as an index
template <typename T>
concept IndexableNodeID = NodeId<T> || NodeHandle<T>;

template <typename T> struct is_explicit_type_id : std::false_type {};
template <> struct is_explicit_type_id<ast::explicit_type_id> : std::true_type {};

// Represents a type id that can be used as an index
template <typename T>
concept IndexableExplicitTypeID = is_explicit_type_id<T>::value;

// A generically indexable ID
template <typename T>
concept IndexableID = IndexableExplicitTypeID<T> || IndexableNodeID<T>;

} // namespace ghoti::ast
