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
    for (const auto* inst : seg.get_instructions()) { check_instruction(fn, seg, *inst); }
}

} // namespace ghoti::sema
