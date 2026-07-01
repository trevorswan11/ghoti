#pragma once

#include <filesystem>
#include <ostream>

#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/generic.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"

namespace ghoti::sema {

// The manager for all steps of semantic analysis.
class analyzer {
  public:
    explicit analyzer(mod::module_manager& modules,
                      std::ostream&        error_stream,
                      stdx::option<bool>   in_terminal) noexcept
        : modules_{modules}, error_stream_{error_stream}, in_terminal_{in_terminal},
          ctx_{modules_,
               registry_,
               pool_,
               generic_functions_,
               instantiation_cache_,
               diagnostics{in_terminal_},
               error_stream_} {}
    ~analyzer() = default;
    MAKE_MOVE_CONSTRUCTABLE_ONLY(analyzer);

    // Runs the entire sema pipeline
    auto analyze(const std::filesystem::path& entry_path) -> stdx::result<void, diagnostic>;

    [[nodiscard]] auto get_table(this auto&& self, usize idx) -> auto& {
        return self.registry_.get(idx);
    }

    [[nodiscard]] auto get_table_opt(this auto&& self, usize idx) noexcept {
        return self.registry_.get_opt(idx);
    }

    MAKE_DEDUCING_GETTER(registry);
    MAKE_DEDUCING_GETTER(pool);
    MAKE_DEDUCING_GETTER(generic_functions);
    MAKE_DEDUCING_GETTER(instantiation_cache);
    MAKE_DEDUCING_GETTER(ctx);

    [[nodiscard]] auto get_prelude_index_opt() const noexcept -> stdx::opt_size {
        return ctx_.prelude_index;
    }

    auto collect_symbols(mod::module& module) -> mod::module_state;
    auto resolve_types(mod::module& module) -> mod::module_state;

  private:
    mod::module_manager&        modules_;
    symbol_table_registry       registry_;
    type_pool                   pool_;
    generic_function_registry   generic_functions_;
    generic_instantiation_cache instantiation_cache_;
    std::ostream&               error_stream_;
    stdx::option<bool>          in_terminal_;

    context ctx_;
};

} // namespace ghoti::sema
