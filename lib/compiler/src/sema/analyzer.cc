#include "compiler/sema/analyzer.hh"

#include <filesystem>
#include <utility>

#include <stdx/profiler.hh>
#include <stdx/result.hh>

#include "compiler/gir/emitter.hh"
#include "compiler/gir/module.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/passes/symbol_collector.hh"
#include "compiler/sema/passes/type_checker.hh"
#include "compiler/sema/passes/type_resolver.hh"
#include "compiler/syntax/error.hh"

namespace ghoti::sema {

auto analyzer::analyze(const std::filesystem::path& entry_path) -> stdx::result<void, diagnostic> {
    PROFILE_FUNCTION();
    auto module_result{modules_.try_get_file_module(entry_path)};
    if (!module_result) {
        return make_sema_err(std::move(module_result.error()).get_message(),
                             error::MODULE_LOAD_ERROR);
    }

    auto module{*module_result};
    if (module->diagnostics.is<syntax::diagnostics>()) { module->print_diagnostics(error_stream_); }

    collect_symbols(*module);
    resolve_types(*module);

    if (module->is_poisoned()) {
        module->print_diagnostics(error_stream_);
        return {};
    }

    auto gir_mod{emit_gir(*module)};
    if (module->is_poisoned()) {
        module->print_diagnostics(error_stream_);
        return {};
    }

    check_types(gir_mod, *module);
    if (module->is_poisoned()) {
        module->print_diagnostics(error_stream_);
        return {};
    }

    return {};
}

auto analyzer::collect_symbols(mod::module& module) -> mod::module_state {
    return symbol_collector::collect_symbols(module, ctx_);
}

auto analyzer::resolve_types(mod::module& module) -> mod::module_state {
    return type_resolver::resolve_types(module, ctx_);
}

auto analyzer::emit_gir(mod::module& module) -> gir::module {
    gir::emitter emitter{ctx_, module};
    return emitter.emit();
}

auto analyzer::check_types(gir::module& gir_module, mod::module& ast_module) -> mod::module_state {
    return type_checker::check_types(gir_module, ast_module, ctx_);
}

} // namespace ghoti::sema
