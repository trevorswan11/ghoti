#pragma once

#include <string>
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
#include "support/diagnostic.hh"

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
    MAKE_GETTER(location, stdx::option<source_location>);

    constexpr auto set_insert_point(function& fn, segment& seg) noexcept -> void {
        function_.emplace(fn);
        segment_.emplace(seg);
    }

    constexpr auto set_segment(segment& seg) noexcept -> void { segment_.emplace(seg); }
    constexpr auto set_location(stdx::option<source_location> loc) noexcept -> void {
        location_ = loc;
    }

    auto emit_instruction(instruction inst) -> instruction&;
    auto emit_alloca(sema::type& type, std::string_view name = {}, bool is_const = false)
        -> local_id;
    auto emit_load(local_id src, sema::type& type) -> local_id;
    auto emit_load(value src, sema::type& type) -> local_id;
    auto emit_store(local_id dest, value val) -> instruction&;
    auto emit_store(value dest, value val) -> instruction&;
    // Produces a temporary holding the address of a module-level / static-member global.
    auto emit_global_addr(std::string name, sema::type& type, bool is_const = false) -> local_id;
    auto emit_get_element_ptr(value base, std::vector<value> indices, sema::type& result_type)
        -> local_id;
    auto emit_address_of(value target, sema::type& result_type) -> local_id;
    auto emit_deref(value ptr, sema::type& result_type) -> local_id;
    auto emit_binary(instruction_kind kind, value lhs, value rhs, sema::type& type) -> local_id;
    auto emit_unary(instruction_kind kind, value operand, sema::type& type) -> local_id;
    auto emit_cast(instruction_kind kind, value operand, sema::type& target_type) -> local_id;
    auto emit_call(std::string_view callee, std::vector<value> args, sema::type& return_type)
        -> stdx::option<local_id>;
    auto emit_builtin_call(std::string_view   callee,
                           std::vector<value> args,
                           sema::type&        return_type) -> stdx::option<local_id>;
    auto emit_indirect_call(value callee, std::vector<value> args, sema::type& return_type)
        -> stdx::option<local_id>;
    auto emit_inline_asm(inline_asm info, std::vector<value> inputs, sema::type& result_type)
        -> stdx::option<local_id>;
    auto emit_return(stdx::option<value> val = stdx::none) -> instruction&;
    auto emit_goto(segment_id target_segment) -> instruction&;
    auto emit_cond_goto(value cond, segment_id true_segment, segment_id false_segment)
        -> instruction&;
    auto emit_unreachable() -> instruction&;

  private:
    stdx::option<function&>       function_;
    stdx::option<segment&>        segment_;
    stdx::option<source_location> location_;
};

} // namespace ghoti::gir
