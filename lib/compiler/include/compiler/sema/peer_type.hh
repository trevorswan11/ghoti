#pragma once

#include <string>

#include <gsl/pointers>
#include <gsl/span>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/sema/type.hh"

namespace ghoti::sema {

struct context;

struct peer_operand {
    gsl::not_null<type*> type;
    // An array value only becomes a slice when it has an address to point at
    bool addressable{true};
};

// The two operands that have no common type, and what to write instead when that is known
struct peer_error {
    usize       first{0};
    usize       second{0};
    std::string hint;
};

// The one type every operand converts to implicitly, the type a value gets where several meet:
// the two sides of an operator, the arms of an `if` or `match`, the values a block breaks with.
//   - a poison operand gives poison, and `noreturn`/`undefined` operands take no part
//   - numbers give the operand type every other widens into; none is invented (`i32` with `u32`
//     has no peer), and untyped constants adopt the concrete peer
//   - pointers, references, and slices give the least mutable, keeping `volatile`
//   - arrays of different lengths, and pointers to them, give a slice of their element
[[nodiscard]] auto peer_type(context& ctx, gsl::span<const peer_operand> operands)
    -> stdx::result<gsl::not_null<type*>, peer_error>;

// "no peer type for 'i32' and 'bool'", followed by the hint when there is one
[[nodiscard]] auto peer_error_message(const context&                ctx,
                                      gsl::span<const peer_operand> operands,
                                      const peer_error&             error) -> std::string;

} // namespace ghoti::sema
