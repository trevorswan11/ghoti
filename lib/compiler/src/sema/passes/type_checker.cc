#include "compiler/sema/passes/type_checker.hh"

#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>
#include <stdx/assert.hh>
#include <stdx/option.hh>
#include <stdx/profiler.hh>
#include <stdx/types.hh>

#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/module.hh"
#include "compiler/gir/segment.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "support/diagnostic.hh"

namespace ghoti::sema {

auto type_checker::check_types(gir::module& gir_mod, mod::module& ast_mod, context& ctx)
    -> mod::module_state {
    PROFILE_FUNCTION();
    type_checker checker{gir_mod, ctx};

    for (const auto& fn : gir_mod.get_functions()) { checker.check_function(*fn); }
    if (!ctx.diags.empty() || ast_mod.is_poisoned()) {
        return ast_mod.error_out(std::move(ctx.diags), mod::module_state::POISONED_TYPE_RESOLVED);
    }
    return ast_mod.state;
}

auto type_checker::emit_diagnostic(std::string_view              message,
                                   error                         err,
                                   stdx::option<source_location> loc) -> void {
    ctx_.diags.emplace_back(std::string{message}, err, loc);
}

auto type_checker::get_operand_type(const gir::value& val) -> stdx::option<type&> {
    if (val.type) { return val.type; }
    if (const auto lid{val.data.as_opt<gir::local_id>()}) {
        if (auto it{locals_.find(*lid)}; it != locals_.end()) { return *it->second.type; }
    }
    return stdx::none;
}

auto type_checker::find_function(std::string_view name) const -> stdx::option<gir::function&> {
    for (auto* f : gir_mod_.get_functions()) {
        if (f->get_name() == name) { return *f; }
    }
    return stdx::none;
}

auto type_checker::check_function(gir::function& fn) -> void {
    PROFILE_FUNCTION();
    locals_.clear();

    for (const auto& param : fn.get_params()) {
        locals_.emplace(param->id,
                        local_info{.type = &param->type, .is_alloca = false, .is_const = false});
    }

    for (const auto& seg : fn.get_segments()) { check_segment(fn, *seg); }
}

auto type_checker::check_segment(gir::function& fn, gir::segment& seg) -> void {
    PROFILE_FUNCTION();
    for (const auto* inst : seg.get_instructions()) { check_instruction(fn, *inst); }
}

auto type_checker::check_instruction(gir::function& fn, const gir::instruction& inst) -> void {
    PROFILE_FUNCTION();

    switch (inst.kind) {
    case gir::instruction_kind::ALLOCA: {
        if (inst.type && inst.type->get_kind() == type_kind::OPAQUE) {
            emit_diagnostic("Cannot allocate variable of opaque type",
                            error::ILLEGAL_OPAQUE_TYPE,
                            inst.location);
        }
        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = true,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::LOAD: {
        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::STORE: {
        check_store(inst);
        break;
    }
    case gir::instruction_kind::ADD:
    case gir::instruction_kind::SUB:
    case gir::instruction_kind::MUL:
    case gir::instruction_kind::DIV:
    case gir::instruction_kind::MOD: {
        if (inst.operands.size() >= 2) {
            const auto lhs_t{get_operand_type(inst.operands[0])};
            const auto rhs_t{get_operand_type(inst.operands[1])};
            if (lhs_t && rhs_t && !lhs_t->is_poison() && !rhs_t->is_poison()) {
                if (!is_numeric(lhs_t->get_kind()) || !is_numeric(rhs_t->get_kind()) ||
                    !is_same_unqualified(*lhs_t, *rhs_t)) {
                    // Check if one is integer and one is pointer
                    const auto lhs_ptr_rhs_int{lhs_t->get_kind() == type_kind::POINTER &&
                                               is_integer(rhs_t->get_kind())};
                    const auto rhs_ptr_lhs_int{rhs_t->get_kind() == type_kind::POINTER &&
                                               is_integer(lhs_t->get_kind())};
                    const bool ptr_arith{
                        (inst.kind == gir::instruction_kind::ADD ||
                         inst.kind == gir::instruction_kind::SUB) &&
                        (lhs_ptr_rhs_int ||
                         (inst.kind == gir::instruction_kind::ADD && rhs_ptr_lhs_int))};
                    if (!ptr_arith) {
                        emit_diagnostic(
                            fmt::format("Operator '{}' cannot be applied to types '{}' and '{}'",
                                        gir::instruction_kind_name(inst.kind),
                                        type_kind_display_name(lhs_t->get_kind()),
                                        type_kind_display_name(rhs_t->get_kind())),
                            error::OPERATOR_TYPE_MISMATCH,
                            inst.location);
                    }
                }
            }
        }

        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::AND:
    case gir::instruction_kind::OR:  {
        if (inst.operands.size() >= 2) {
            const auto lhs_t{get_operand_type(inst.operands[0])};
            const auto rhs_t{get_operand_type(inst.operands[1])};
            if (lhs_t && rhs_t && !lhs_t->is_poison() && !rhs_t->is_poison()) {
                const bool both_bool{lhs_t->get_kind() == type_kind::BOOL &&
                                     rhs_t->get_kind() == type_kind::BOOL};
                const bool both_same_int{is_integer(lhs_t->get_kind()) &&
                                         is_integer(rhs_t->get_kind()) &&
                                         is_same_unqualified(*lhs_t, *rhs_t)};
                if (!both_bool && !both_same_int) {
                    emit_diagnostic(
                        fmt::format("Operator '{}' cannot be applied to types '{}' and '{}'",
                                    gir::instruction_kind_name(inst.kind),
                                    type_kind_display_name(lhs_t->get_kind()),
                                    type_kind_display_name(rhs_t->get_kind())),
                        error::OPERATOR_TYPE_MISMATCH,
                        inst.location);
                }
            }
        }

        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::XOR:
    case gir::instruction_kind::SHL:
    case gir::instruction_kind::SHR: {
        if (inst.operands.size() >= 2) {
            const auto lhs_t{get_operand_type(inst.operands[0])};
            const auto rhs_t{get_operand_type(inst.operands[1])};
            if (lhs_t && rhs_t && !lhs_t->is_poison() && !rhs_t->is_poison()) {
                if (!is_integer(lhs_t->get_kind()) || !is_integer(rhs_t->get_kind()) ||
                    !is_same_unqualified(*lhs_t, *rhs_t)) {
                    emit_diagnostic(
                        fmt::format("Operator '{}' cannot be applied to types '{}' and '{}'",
                                    gir::instruction_kind_name(inst.kind),
                                    type_kind_display_name(lhs_t->get_kind()),
                                    type_kind_display_name(rhs_t->get_kind())),
                        error::OPERATOR_TYPE_MISMATCH,
                        inst.location);
                }
            }
        }

        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::EQ:
    case gir::instruction_kind::NE: {
        if (inst.operands.size() >= 2) {
            const auto lhs_t{get_operand_type(inst.operands[0])};
            const auto rhs_t{get_operand_type(inst.operands[1])};
            if (lhs_t && rhs_t && !lhs_t->is_poison() && !rhs_t->is_poison()) {
                if (!is_assignable(*lhs_t, *rhs_t) && !is_assignable(*rhs_t, *lhs_t)) {
                    emit_diagnostic(
                        fmt::format("Comparison operator cannot be applied to incompatible types "
                                    "'{}' and '{}'",
                                    type_kind_display_name(lhs_t->get_kind()),
                                    type_kind_display_name(rhs_t->get_kind())),
                        error::OPERATOR_TYPE_MISMATCH,
                        inst.location);
                }
            }
        }

        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::LT:
    case gir::instruction_kind::LE:
    case gir::instruction_kind::GT:
    case gir::instruction_kind::GE: {
        if (inst.operands.size() >= 2) {
            const auto lhs_t{get_operand_type(inst.operands[0])};
            const auto rhs_t{get_operand_type(inst.operands[1])};
            if (lhs_t && rhs_t && !lhs_t->is_poison() && !rhs_t->is_poison()) {
                if (!is_numeric(lhs_t->get_kind()) || !is_numeric(rhs_t->get_kind()) ||
                    (!is_assignable(*lhs_t, *rhs_t) && !is_assignable(*rhs_t, *lhs_t))) {
                    emit_diagnostic(
                        fmt::format("Relational operator cannot be applied to non-numeric or "
                                    "incompatible types '{}' and '{}'",
                                    type_kind_display_name(lhs_t->get_kind()),
                                    type_kind_display_name(rhs_t->get_kind())),
                        error::OPERATOR_TYPE_MISMATCH,
                        inst.location);
                }
            }
        }

        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::NEG: {
        if (!inst.operands.empty()) {
            const auto op_t{get_operand_type(inst.operands[0])};
            if (op_t && !op_t->is_poison()) {
                if (!is_signed_integer(op_t->get_kind()) && !is_float(op_t->get_kind())) {
                    emit_diagnostic("Unary negation '-' requires a signed integer or float operand",
                                    error::OPERATOR_TYPE_MISMATCH,
                                    inst.location);
                }
            }
        }

        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::NOT: {
        if (!inst.operands.empty()) {
            const auto op_t{get_operand_type(inst.operands[0])};
            if (op_t && !op_t->is_poison()) {
                if (op_t->get_kind() != type_kind::BOOL) {
                    emit_diagnostic("Logical negation '!' requires a boolean operand",
                                    error::OPERATOR_TYPE_MISMATCH,
                                    inst.location);
                }
            }
        }

        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::BITNOT: {
        if (!inst.operands.empty()) {
            const auto op_t{get_operand_type(inst.operands[0])};
            if (op_t && !op_t->is_poison()) {
                if (!is_integer(op_t->get_kind())) {
                    emit_diagnostic("Bitwise negation '~' requires an integer operand",
                                    error::OPERATOR_TYPE_MISMATCH,
                                    inst.location);
                }
            }
        }

        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::RET: {
        const auto& fn_type{fn.get_type()};
        const auto  fn_data{fn_type.get_data().as_opt<types::function>()};
        if (!fn_data) { break; }

        auto& expected_ret_t{fn_data->return_type};
        if (expected_ret_t.get_kind() == type_kind::VOID) {
            if (!inst.operands.empty()) {
                // Returning from the void is fine (jason turner moment)
                const auto ret_t{get_operand_type(inst.operands[0])};
                if (ret_t && ret_t->get_kind() != type_kind::VOID && !ret_t->is_poison()) {
                    emit_diagnostic("Cannot return a value from a function returning void",
                                    error::RETURN_TYPE_MISMATCH,
                                    inst.location);
                }
            }
        } else {
            if (inst.operands.empty() || inst.operands[0].data.is<gir::void_val>()) {
                emit_diagnostic(fmt::format("Empty return in function expecting return type '{}'",
                                            type_kind_display_name(expected_ret_t.get_kind())),
                                error::RETURN_TYPE_MISMATCH,
                                inst.location);
            } else {
                const auto ret_t{get_operand_type(inst.operands[0])};
                if (ret_t && !ret_t->is_poison() && !is_assignable(*ret_t, expected_ret_t)) {
                    emit_diagnostic(
                        fmt::format("Return value of type '{}' is not assignable to function "
                                    "return type '{}'",
                                    type_kind_display_name(ret_t->get_kind()),
                                    type_kind_display_name(expected_ret_t.get_kind())),
                        error::RETURN_TYPE_MISMATCH,
                        inst.location);
                }
            }
        }
        break;
    }
    case gir::instruction_kind::CALL:
    case gir::instruction_kind::BUILTIN_CALL: {
        if (inst.callee_name) {
            if (const auto callee{find_function(*inst.callee_name)}) {
                const auto& params{callee->get_params()};
                if (callee->get_is_variadic()) {
                    if (inst.operands.size() < params.size()) {
                        emit_diagnostic(
                            fmt::format(
                                "Function '{}' expects at least {} arguments but {} were provided",
                                *inst.callee_name,
                                params.size(),
                                inst.operands.size()),
                            error::ARITY_MISMATCH,
                            inst.location);
                    } else {
                        for (usize i{0}; i < params.size(); ++i) {
                            const auto arg_t{get_operand_type(inst.operands[i])};
                            if (arg_t && !arg_t->is_poison() &&
                                !is_assignable(*arg_t, params[i]->type)) {
                                emit_diagnostic(
                                    fmt::format("Argument {} of type '{}' is not assignable to "
                                                "parameter type '{}' in call to '{}'",
                                                i + 1,
                                                type_kind_display_name(arg_t->get_kind()),
                                                type_kind_display_name(params[i]->type.get_kind()),
                                                *inst.callee_name),
                                    error::TYPE_MISMATCH,
                                    inst.location);
                            }
                        }
                    }
                } else if (params.size() != inst.operands.size()) {
                    emit_diagnostic(
                        fmt::format("Function '{}' expects {} arguments but {} were provided",
                                    *inst.callee_name,
                                    params.size(),
                                    inst.operands.size()),
                        error::ARITY_MISMATCH,
                        inst.location);
                } else {
                    for (usize i{0}; i < params.size(); ++i) {
                        const auto arg_t{get_operand_type(inst.operands[i])};
                        if (arg_t && !arg_t->is_poison() &&
                            !is_assignable(*arg_t, params[i]->type)) {
                            emit_diagnostic(
                                fmt::format("Argument {} of type '{}' is not assignable to "
                                            "parameter type '{}' in call to '{}'",
                                            i + 1,
                                            type_kind_display_name(arg_t->get_kind()),
                                            type_kind_display_name(params[i]->type.get_kind()),
                                            *inst.callee_name),
                                error::TYPE_MISMATCH,
                                inst.location);
                        }
                    }
                }
            }
        } else if (!inst.operands.empty()) {
            const auto callee_t{get_operand_type(inst.operands[0])};
            if (callee_t && !callee_t->is_poison()) {
                stdx::option<const types::function&> fn_data;
                if (const auto ptr_data{callee_t->get_data().as_opt<types::pointer>()}) {
                    fn_data = ptr_data->underlying.get_data().as_opt<types::function>();
                } else {
                    fn_data = callee_t->get_data().as_opt<types::function>();
                }

                if (fn_data) {
                    const auto expected_args{fn_data->params.size()};
                    const auto provided_args{inst.operands.size() - 1};

                    if (fn_data->is_variadic) {
                        if (provided_args < expected_args) {
                            emit_diagnostic(
                                fmt::format("Indirect function call expects at least {} arguments "
                                            "but {} were provided",
                                            expected_args,
                                            provided_args),
                                error::ARITY_MISMATCH,
                                inst.location);
                        } else {
                            for (usize i{0}; i < expected_args; ++i) {
                                const auto arg_t{get_operand_type(inst.operands[i + 1])};
                                if (arg_t && !arg_t->is_poison() &&
                                    !is_assignable(*arg_t, *fn_data->params[i])) {
                                    emit_diagnostic(
                                        fmt::format(
                                            "Argument {} of type '{}' is not assignable to "
                                            "parameter type '{}' in indirect call",
                                            i + 1,
                                            type_kind_display_name(arg_t->get_kind()),
                                            type_kind_display_name(fn_data->params[i]->get_kind())),
                                        error::TYPE_MISMATCH,
                                        inst.location);
                                }
                            }
                        }
                    } else if (provided_args != expected_args) {
                        emit_diagnostic(
                            fmt::format("Indirect function call expects {} arguments but {} were "
                                        "provided",
                                        expected_args,
                                        provided_args),
                            error::ARITY_MISMATCH,
                            inst.location);
                    } else {
                        for (usize i{0}; i < expected_args; ++i) {
                            const auto arg_t{get_operand_type(inst.operands[i + 1])};
                            if (arg_t && !arg_t->is_poison() &&
                                !is_assignable(*arg_t, *fn_data->params[i])) {
                                emit_diagnostic(
                                    fmt::format(
                                        "Argument {} of type '{}' is not assignable to "
                                        "parameter type '{}' in indirect call",
                                        i + 1,
                                        type_kind_display_name(arg_t->get_kind()),
                                        type_kind_display_name(fn_data->params[i]->get_kind())),
                                    error::TYPE_MISMATCH,
                                    inst.location);
                            }
                        }
                    }
                }
            }
        }

        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::PTR_CAST: {
        if (!inst.operands.empty() && inst.type) {
            const auto src_t{get_operand_type(inst.operands[0])};
            const auto dest_t{inst.type};
            if (src_t && dest_t && !src_t->is_poison() && !dest_t->is_poison()) {
                if (src_t->get_kind() == type_kind::POINTER &&
                    dest_t->get_kind() == type_kind::POINTER) {
                    const auto src_ptr{src_t->get_data().as_opt<types::pointer>()};
                    const auto dest_ptr{dest_t->get_data().as_opt<types::pointer>()};
                    if (src_ptr && dest_ptr) {
                        if (src_t->is_constant() && !dest_t->is_constant()) {
                            emit_diagnostic(
                                "Cannot cast away const from pointer without @constCast",
                                error::ILLEGAL_CONST_CAST,
                                inst.location);
                        }
                    }
                }
            }
        }

        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::WIDEN_CAST: {
        if (!inst.operands.empty() && inst.type) {
            const auto src_t{get_operand_type(inst.operands[0])};
            const auto dest_t{inst.type};
            if (src_t && dest_t && !src_t->is_poison() && !dest_t->is_poison()) {
                const auto src_k{src_t->get_kind()};
                const auto dest_k{dest_t->get_kind()};
                const bool is_num_cast{sema::is_numeric(src_k) && sema::is_numeric(dest_k)};
                if (!is_implicit_widenable(src_k, dest_k) &&
                    !is_same_unqualified(*src_t, *dest_t) && !is_num_cast) {
                    emit_diagnostic(fmt::format("Cannot cast type '{}' to '{}'",
                                                type_kind_display_name(src_k),
                                                type_kind_display_name(dest_k)),
                                    error::TYPE_MISMATCH,
                                    inst.location);
                }
            }
        }

        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = false,
                                     });
        }
        break;
    }
    case gir::instruction_kind::COND_GOTO: {
        if (!inst.operands.empty()) {
            const auto cond_t{get_operand_type(inst.operands[0])};
            if (cond_t && !cond_t->is_poison() && cond_t->get_kind() != type_kind::BOOL) {
                emit_diagnostic("Conditional branch condition must be of type 'bool'",
                                error::TYPE_MISMATCH,
                                inst.location);
            }
        }
        break;
    }
    case gir::instruction_kind::GET_ELEMENT_PTR:
    case gir::instruction_kind::ADDRESS_OF:
    case gir::instruction_kind::DEREF:
    case gir::instruction_kind::INT_FROM_PTR:
    case gir::instruction_kind::PTR_FROM_INT:
    case gir::instruction_kind::BIT_CAST:
    case gir::instruction_kind::CONST:           {
        if (inst.result && inst.type) {
            locals_.insert_or_assign(*inst.result,
                                     local_info{
                                         .type      = inst.type.get(),
                                         .is_alloca = false,
                                         .is_const  = inst.kind == gir::instruction_kind::CONST,
                                     });
        }
        break;
    }
    case gir::instruction_kind::GOTO:
    case gir::instruction_kind::UNREACHABLE: break;
    }
}

auto type_checker::check_store(const gir::instruction& inst) -> void {
    if (inst.result && !inst.operands.empty()) {
        // Case A: Storing to a local_id alloca slot or param
        if (auto it{locals_.find(*inst.result)}; it != locals_.end()) {
            const auto val_t{get_operand_type(inst.operands[0])};
            if (it->second.is_alloca) {
                if (it->second.is_const) {
                    emit_diagnostic("Cannot assign to constant variable",
                                    error::ASSIGNMENT_TO_CONST,
                                    inst.location);
                    return;
                }

                if (val_t && !is_assignable(*val_t, *it->second.type)) {
                    emit_diagnostic(
                        fmt::format("Type mismatch in store: cannot assign '{}' to '{}'",
                                    type_kind_display_name(val_t->get_kind()),
                                    type_kind_display_name(it->second.type->get_kind())),
                        error::TYPE_MISMATCH,
                        inst.location);
                }
                return;
            }

            if (it->second.type->get_kind() == type_kind::POINTER) {
                const auto ptr_data{it->second.type->get_data().as_opt<types::pointer>()};
                if (val_t && is_assignable(*val_t, *it->second.type)) {
                    // Storing pointer value into pointer variable or field: p = q
                    if (it->second.is_const) {
                        emit_diagnostic("Cannot assign to constant variable",
                                        error::ASSIGNMENT_TO_CONST,
                                        inst.location);
                        return;
                    }
                    return;
                }
                if (ptr_data && val_t && is_assignable(*val_t, ptr_data->underlying)) {
                    // Storing through pointer: *p = val
                    if (it->second.type->is_constant()) {
                        emit_diagnostic("Cannot assign to constant memory through pointer",
                                        error::ASSIGNMENT_TO_CONST,
                                        inst.location);
                        return;
                    }
                    return;
                } else {
                    if (it->second.type->is_constant()) {
                        emit_diagnostic("Cannot assign to constant memory through pointer",
                                        error::ASSIGNMENT_TO_CONST,
                                        inst.location);
                    } else {
                        emit_diagnostic(
                            fmt::format(
                                "Type mismatch in store: cannot assign '{}' to '{}'",
                                val_t ? type_kind_display_name(val_t->get_kind()) : "unknown",
                                ptr_data ? type_kind_display_name(ptr_data->underlying.get_kind())
                                         : type_kind_display_name(it->second.type->get_kind())),
                            error::TYPE_MISMATCH,
                            inst.location);
                    }
                }
                return;
            }

            if (it->second.is_const) {
                emit_diagnostic("Cannot assign to constant variable",
                                error::ASSIGNMENT_TO_CONST,
                                inst.location);
                return;
            }

            if (val_t && !is_assignable(*val_t, *it->second.type)) {
                emit_diagnostic(fmt::format("Type mismatch in store: cannot assign '{}' to '{}'",
                                            type_kind_display_name(val_t->get_kind()),
                                            type_kind_display_name(it->second.type->get_kind())),
                                error::TYPE_MISMATCH,
                                inst.location);
            }
        }
    } else if (inst.operands.size() >= 2) {
        // Case B: Storing through a pointer destination
        const auto dest_t{get_operand_type(inst.operands[0])};
        const auto val_t{get_operand_type(inst.operands[1])};
        if (dest_t) {
            if (dest_t->get_kind() == type_kind::POINTER) {
                const auto ptr_data{dest_t->get_data().as_opt<types::pointer>()};
                if (ptr_data) {
                    if (dest_t->is_constant()) {
                        emit_diagnostic("Cannot assign to constant memory through pointer",
                                        error::ASSIGNMENT_TO_CONST,
                                        inst.location);
                        return;
                    }

                    if (val_t && !is_assignable(*val_t, ptr_data->underlying)) {
                        emit_diagnostic(
                            fmt::format("Type mismatch in store: cannot assign '{}' to '{}'",
                                        type_kind_display_name(val_t->get_kind()),
                                        type_kind_display_name(ptr_data->underlying.get_kind())),
                            error::TYPE_MISMATCH,
                            inst.location);
                    }
                }
            } else if (dest_t->get_kind() == type_kind::REFERENCE) {
                const auto ref_data{dest_t->get_data().as_opt<types::reference>()};
                if (ref_data) {
                    if (ref_data->underlying.is_constant()) {
                        emit_diagnostic("Cannot assign to constant memory through reference",
                                        error::ASSIGNMENT_TO_CONST,
                                        inst.location);
                        return;
                    }

                    if (val_t && !is_assignable(*val_t, ref_data->underlying)) {
                        emit_diagnostic(
                            fmt::format("Type mismatch in store: cannot assign '{}' to '{}'",
                                        type_kind_display_name(val_t->get_kind()),
                                        type_kind_display_name(ref_data->underlying.get_kind())),
                            error::TYPE_MISMATCH,
                            inst.location);
                    }
                }
            }
        }
    }
}

} // namespace ghoti::sema
