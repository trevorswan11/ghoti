#pragma once

#include <stdx/types.hh>

#include "compiler/codegen/target.hh"
#include "compiler/sema/type.hh"

namespace ghoti::gir {

[[nodiscard]] auto align_of(const sema::type& type, const codegen::type_layout& layout) -> usize;
[[nodiscard]] auto size_of(const sema::type& type, const codegen::type_layout& layout) -> usize;
[[nodiscard]] auto struct_field_align(const sema::types::struct_t& st,
                                      usize                        idx,
                                      const codegen::type_layout&  layout) -> usize;

// A slice lowers to { ptr: T*, len: usize } everywhere it is constructed or indexed.
constexpr u64 SLICE_PTR_FIELD_INDEX{0};
constexpr u64 SLICE_LEN_FIELD_INDEX{1};

// Array's lengths are compile time constants, so they only have to store a pointer
constexpr u64 ARRAY_PTR_FIELD_INDEX{0};

// A tagged union lowers to { tag: i32, payload } regardless of active variant, with the payload
// slot aligned to its most aligned member
constexpr u64 TAGGED_UNION_DISCRIMINANT_INDEX{0};
constexpr u64 TAGGED_UNION_PAYLOAD_INDEX{1};

} // namespace ghoti::gir
