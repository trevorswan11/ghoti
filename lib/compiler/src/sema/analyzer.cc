#include "compiler/sema/analyzer.hh"

#include <filesystem>
#include <utility>

#include <stdx/profiler.hh>
#include <stdx/result.hh>

#include "compiler/module/module.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/passes/symbol_collector.hh"
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

    // Perform a final diagnostic flush if poisoned
    if (module->is_poisoned()) { module->print_diagnostics(error_stream_); }
    return {};
}

auto analyzer::collect_symbols(mod::module& module) -> mod::module_state {
    return symbol_collector::collect_symbols(module, ctx_);
}

auto analyzer::resolve_types(mod::module& module) -> mod::module_state {
    return type_resolver::resolve_types(module, ctx_);
}

} // namespace ghoti::sema
