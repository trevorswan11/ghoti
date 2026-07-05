#pragma once

#include <string_view>
#include <vector>

#include <gsl/pointers>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/segment.hh"
#include "compiler/sema/type.hh"

namespace ghoti::gir {

class builder {
  public:
    constexpr builder() noexcept = default;
    constexpr explicit builder(function& fn, segment& seg) noexcept
        : function_{fn}, segment_{seg} {}
    ~builder() = default;
    MAKE_PINNED(builder);

    MAKE_GETTER(function, stdx::option<function&>);
    MAKE_GETTER(segment, stdx::option<segment&>);

    constexpr auto set_insert_point(function& fn, segment& seg) noexcept -> void {
        function_.emplace(fn);
        segment_.emplace(seg);
    }

    constexpr auto set_segment(segment& seg) noexcept -> void { segment_.emplace(seg); }

    auto emit_instruction(instruction inst) -> instruction&;
    auto emit_alloca(sema::type& type, std::string_view name = {}) -> local_id;
    auto emit_load(local_id src, sema::type& type) -> local_id;
    auto emit_store(local_id dest, value val) -> instruction&;
    auto emit_binary(instruction_kind kind, value lhs, value rhs, sema::type& type) -> local_id;
    auto emit_unary(instruction_kind kind, value operand, sema::type& type) -> local_id;
    auto emit_cast(instruction_kind kind, value operand, sema::type& target_type) -> local_id;
    auto emit_call(std::string_view callee, std::vector<value> args, sema::type& return_type)
        -> stdx::option<local_id>;
    auto emit_return(stdx::option<value> val = stdx::none) -> instruction&;

  private:
    stdx::option<function&> function_;
    stdx::option<segment&>  segment_;
};

} // namespace ghoti::gir
