#pragma once

#include <stdx/types.hh>

namespace ghoti::gir {

// A slice lowers to { ptr: T*, len: usize } everywhere it is constructed or indexed.
constexpr u64 SLICE_PTR_FIELD_INDEX{0};
constexpr u64 SLICE_LEN_FIELD_INDEX{1};

// Array's lengths are compile time constants, so they only have to store a pointer
constexpr u64 ARRAY_PTR_FIELD_INDEX{0};

// A tagged union lowers to { tag: i32, payload: [N x i8] } regardless of active variant.
constexpr u64 TAGGED_UNION_DISCRIMINANT_INDEX{0};
constexpr u64 TAGGED_UNION_PAYLOAD_INDEX{1};

} // namespace ghoti::gir
