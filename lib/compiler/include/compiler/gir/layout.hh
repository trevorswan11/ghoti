#pragma once

#include <stdx/types.hh>

namespace ghoti::gir {

// A slice lowers to { ptr: T*, len: usize } everywhere it is constructed or indexed.
constexpr u64 SLICE_PTR_FIELD_INDEX{0};
constexpr u64 SLICE_LEN_FIELD_INDEX{1};

// A tagged union lowers to { tag: i32, payload: [N x i8] } regardless of active variant.
constexpr u64 TAGGED_UNION_PAYLOAD_INDEX{1};

} // namespace ghoti::gir
