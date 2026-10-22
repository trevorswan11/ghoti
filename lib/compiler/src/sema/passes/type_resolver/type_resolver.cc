#include <utility>

#include <gsl/util>
#include <stdx/profiler.hh>
#include <stdx/types.hh>

#include "compiler/ast/statement.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/passes/type_resolver.hh"

namespace ghoti::sema {

auto type_resolver::resolve_types(mod::module_t& module, context& ctx) -> mod::module_state {
    PROFILE_FUNCTION();

    // A module already poisoned by symbol collection stays poisoned
    const auto poisoned_collection{module.state == mod::module_state::POISONED_SYMBOL_COLLECTION};

    if (!module.is_resolvable()) { return module.state; }

    // Only the outermost resolution resolves the queued tests, once nothing is mid-resolution
    auto& pending{ctx.pending_tests};
    ++pending.resolve_depth;
    const auto depth_restore{gsl::finally([&] { --pending.resolve_depth; })};

    module.state = poisoned_collection ? mod::module_state::POISONED_TYPE_RESOLVING
                                       : mod::module_state::TYPE_RESOLVING;
    ctx.inject_prelude();

    type_resolver resolver{module, ctx};
    resolver.pre_register_impls();
    for (const auto& node : module.ast) {
        if (ctx.cancelled()) { break; }
        if (ctx.testing && module.ast.get_as_opt<ast::test_stmt>(node)) {
            module.deferred_tests.emplace_back(node);
            continue;
        }
        resolver.resolve(node);
    }
    if (!module.deferred_tests.empty()) { pending.modules.emplace_back(&module); }

    // Appended, so what symbol collection already reported isn't replaced by its consequences
    if (!ctx.diags.empty()) {
        module.absorb_sema_diagnostics(std::move(ctx.diags));
        module.state = mod::module_state::POISONED_TYPE_RESOLVED;
    } else if (module.is_poisoned()) {
        module.state = mod::module_state::POISONED_TYPE_RESOLVED;
    } else {
        module.state = mod::module_state::TYPE_RESOLVED;
    }

    if (pending.resolve_depth == 1) { resolve_pending_tests(ctx); }
    return module.state;
}

auto type_resolver::resolve_pending_tests(context& ctx) -> void {
    PROFILE_FUNCTION();
    auto& pending{ctx.pending_tests};
    // A test's own `import` can resolve, and queue, further modules
    for (usize i{0}; i < pending.modules.size(); ++i) {
        auto&      module{*pending.modules[i]};
        const auto tests{std::exchange(module.deferred_tests, {})};

        context       test_ctx{ctx};
        type_resolver resolver{module, test_ctx};
        for (const auto& node : tests) {
            if (ctx.cancelled()) { break; }
            resolver.resolve(node);
        }
        if (!test_ctx.diags.empty()) {
            module.absorb_sema_diagnostics(std::move(test_ctx.diags));
            module.state = mod::module_state::POISONED_TYPE_RESOLVED;
        }
    }
    pending.modules.clear();
}

} // namespace ghoti::sema
