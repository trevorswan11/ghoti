#include "compiler/gir/builder.hh"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/segment.hh"
#include "compiler/sema/type.hh"

namespace ghoti::gir {

auto builder::emit_instruction(instruction inst) -> instruction& {
    ASSERT(segment_, "Cannot emit instruction without an active segment");
    return segment_->append(std::move(inst));
}

auto builder::emit_alloca(sema::type& type, std::string_view) -> local_id {
    ASSERT(function_, "Cannot emit alloca without an active function");
    const auto slot{function_->next_local_id(local_kind::ALLOCA)};
    emit_instruction({
        .kind     = instruction_kind::ALLOCA,
        .type     = type,
        .result   = slot,
        .operands = {},
    });
    return slot;
}

auto builder::emit_load(local_id src, sema::type& type) -> local_id {
    ASSERT(function_, "Cannot emit load without an active function");
    const auto dest{function_->next_local_id(local_kind::TEMPORARY)};
    emit_instruction({
        .kind     = instruction_kind::LOAD,
        .type     = type,
        .result   = dest,
        .operands = {value{src, type}},
    });
    return dest;
}

auto builder::emit_store(local_id dest, value val) -> instruction& {
    const auto val_type{val.type};
    return emit_instruction({
        .kind     = instruction_kind::STORE,
        .type     = val_type,
        .result   = dest,
        .operands = {std::move(val)},
    });
}

auto builder::emit_binary(instruction_kind kind, value lhs, value rhs, sema::type& type)
    -> local_id {
    ASSERT(function_, "Cannot emit binary instruction without an active function");
    const auto dest{function_->next_local_id(local_kind::TEMPORARY)};
    emit_instruction({
        .kind     = kind,
        .type     = type,
        .result   = dest,
        .operands = {std::move(lhs), std::move(rhs)},
    });
    return dest;
}

auto builder::emit_unary(instruction_kind kind, value operand, sema::type& type) -> local_id {
    ASSERT(function_, "Cannot emit unary instruction without an active function");
    const auto dest{function_->next_local_id(local_kind::TEMPORARY)};
    emit_instruction({
        .kind     = kind,
        .type     = type,
        .result   = dest,
        .operands = {std::move(operand)},
    });
    return dest;
}

auto builder::emit_cast(instruction_kind kind, value operand, sema::type& target_type) -> local_id {
    ASSERT(function_, "Cannot emit cast instruction without an active function");
    const auto dest{function_->next_local_id(local_kind::TEMPORARY)};
    emit_instruction({
        .kind     = kind,
        .type     = target_type,
        .result   = dest,
        .operands = {std::move(operand)},
    });
    return dest;
}

auto builder::emit_call(std::string_view callee, std::vector<value> args, sema::type& return_type)
    -> stdx::option<local_id> {
    ASSERT(function_, "Cannot emit call instruction without an active function");
    if (return_type.get_kind() != sema::type_kind::VOID) {
        const auto dest{function_->next_local_id(local_kind::TEMPORARY)};
        emit_instruction({
            .kind        = instruction_kind::CALL,
            .type        = return_type,
            .result      = dest,
            .operands    = std::move(args),
            .callee_name = std::string{callee},
        });
        return dest;
    }

    emit_instruction({
        .kind        = instruction_kind::CALL,
        .type        = return_type,
        .result      = stdx::none,
        .operands    = std::move(args),
        .callee_name = std::string{callee},
    });
    return stdx::none;
}

auto builder::emit_return(stdx::option<value> val) -> instruction& {
    if (val) {
        const auto val_type{val->type};
        return emit_instruction({
            .kind     = instruction_kind::RET,
            .type     = val_type,
            .result   = stdx::none,
            .operands = {std::move(*val)},
        });
    }
    return emit_instruction({
        .kind     = instruction_kind::RET,
        .type     = stdx::none,
        .result   = stdx::none,
        .operands = {},
    });
}

auto builder::emit_goto(segment_id target_segment) -> instruction& {
    return emit_instruction({
        .kind           = instruction_kind::GOTO,
        .target_segment = target_segment,
    });
}

auto builder::emit_cond_goto(value cond, segment_id true_segment, segment_id false_segment)
    -> instruction& {
    return emit_instruction({
        .kind          = instruction_kind::COND_GOTO,
        .operands      = {std::move(cond)},
        .true_segment  = true_segment,
        .false_segment = false_segment,
    });
}

auto builder::emit_unreachable() -> instruction& {
    return emit_instruction({
        .kind = instruction_kind::UNREACHABLE,
    });
}

} // namespace ghoti::gir
