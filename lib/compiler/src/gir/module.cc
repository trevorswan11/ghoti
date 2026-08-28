#include "compiler/gir/module.hh"

#include <algorithm>
#include <stdx/profiler.hh>
#include <stdx/types.hh>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/span>
#include <stdx/option.hh>

#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/sema/type.hh"

namespace ghoti::gir {

auto module::add_type(std::string name, sema::type& type) -> type_decl& {
    return *types_.emplace_back(arena_.make<type_decl>(std::move(name), type));
}

auto module::add_global(std::string         name,
                        sema::type&         type,
                        bool                is_const,
                        stdx::option<value> init,
                        gir::linkage        linkage,
                        std::string         abi_name) -> global_decl& {
    return *globals_.emplace_back(arena_.make<global_decl>(std::move(name),
                                                           type,
                                                           std::move(init),
                                                           linkage,
                                                           std::move(abi_name),
                                                           std::string{},
                                                           is_const,
                                                           false,
                                                           false));
}

auto module::add_function(std::string  name,
                          sema::type&  type,
                          bool         is_test,
                          bool         is_constexpr,
                          bool         is_variadic,
                          gir::linkage linkage,
                          std::string  abi_name) -> function& {
    const auto idx{functions_.size()};
    functions_.emplace_back(arena_.make<function>(arena_,
                                                  std::move(name),
                                                  type,
                                                  is_test,
                                                  is_constexpr,
                                                  is_variadic,
                                                  linkage,
                                                  std::move(abi_name)));
    if (is_test) { tests_.emplace_back(idx); }
    return *functions_.back();
}

auto module::get_required_libraries() const -> std::vector<std::string> {
    std::vector<std::string> libraries;
    const auto               add_if_required{[&](gir::linkage linkage, std::string_view abi_name) {
        if (linkage != gir::linkage::EXTERN || abi_name == "c") { return; }
        if (!std::ranges::contains(libraries, abi_name)) { libraries.emplace_back(abi_name); }
    }};

    for (const auto* fn : functions_) { add_if_required(fn->get_linkage(), fn->get_abi_name()); }
    for (const auto* global : globals_) { add_if_required(global->linkage, global->abi_name); }
    return libraries;
}

auto module::prune_unreachable(gsl::span<const std::string_view> roots) -> void {
    PROFILE_FUNCTION();
    if (!import_boundary_marked_) { return; }

    const auto is_extern{[](gir::linkage l) { return l == gir::linkage::EXTERN; }};

    ankerl::unordered_dense::map<std::string_view, const function*> fn_by_name;
    for (const auto* fn : functions_) { fn_by_name.try_emplace(fn->get_name(), fn); }
    ankerl::unordered_dense::set<std::string_view> global_names;
    for (const auto* g : globals_) { global_names.insert(g->name); }

    ankerl::unordered_dense::set<std::string_view> reachable_fns, referenced_globals;
    std::vector<const function*>                   worklist;

    const auto enqueue_fn{[&](std::string_view name) {
        if (const auto it{fn_by_name.find(name)}; it != fn_by_name.end()) {
            if (reachable_fns.insert(it->first).second) { worklist.emplace_back(it->second); }
        }
    }};
    const auto note_symbol{[&](std::string_view name) {
        if (global_names.contains(name)) { referenced_globals.insert(name); }
        enqueue_fn(name);
    }};
    const auto scan_value{[&](const value& v) {
        if (const auto s{v.as_opt<std::string>()}) { note_symbol(*s); }
    }};

    // Roots: caller entry points + every non-extern root-module function
    for (const auto root : roots) { enqueue_fn(root); }
    for (usize i{0}; i < import_boundary_fn_ && i < functions_.size(); ++i) {
        if (!is_extern(functions_[i]->get_linkage())) { enqueue_fn(functions_[i]->get_name()); }
    }

    // Over-approximate: keep anything any global initializer names.
    for (const auto* g : globals_) {
        if (g->init_value) { scan_value(*g->init_value); }
    }

    while (!worklist.empty()) {
        const auto* fn{worklist.back()};
        worklist.pop_back();
        for (const auto* seg : fn->get_segments()) {
            for (const auto* inst : seg->get_instructions()) {
                if (inst->callee_name) { note_symbol(*inst->callee_name); }
                for (const auto& op : inst->operands) { scan_value(op); }
                if (inst->asm_info) {
                    for (const auto& op : inst->asm_info->output_addrs) { scan_value(op); }
                }
            }
        }
    }

    // Candidates for removal: any `extern` decl, plus any imported (non-root) decl.
    // Everything else -- the root module's own definitions -- is always retained;
    // trimming unused non-extern `pub` code is left to P4.2 (InternalizePass/GlobalDCE).
    std::vector<function*> kept_fns;
    kept_fns.reserve(functions_.size());
    for (usize i{0}; i < functions_.size(); ++i) {
        const bool candidate{is_extern(functions_[i]->get_linkage()) || i >= import_boundary_fn_};
        if (!candidate || reachable_fns.contains(functions_[i]->get_name())) {
            kept_fns.emplace_back(functions_[i]);
        }
    }
    functions_ = std::move(kept_fns);

    std::vector<global_decl*> kept_globals;
    kept_globals.reserve(globals_.size());
    for (usize i{0}; i < globals_.size(); ++i) {
        const bool candidate{is_extern(globals_[i]->linkage) || i >= import_boundary_global_};
        if (!candidate || referenced_globals.contains(globals_[i]->name)) {
            kept_globals.emplace_back(globals_[i]);
        }
    }
    globals_ = std::move(kept_globals);

    // `tests_` indexes into `functions_`; rebuild after the shrink.
    tests_.clear();
    for (usize i{0}; i < functions_.size(); ++i) {
        if (functions_[i]->get_is_test()) { tests_.emplace_back(i); }
    }
}

} // namespace ghoti::gir
