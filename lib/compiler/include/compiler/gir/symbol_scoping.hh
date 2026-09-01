#pragma once

#include <stdx/hash.hh>
#include <string>
#include <string_view>

#include <ankerl/unordered_dense.h>
#include <gsl/span>
#include <stdx/types.hh>

namespace ghoti::mod { struct module; } // namespace ghoti::mod

namespace ghoti::sema { struct context; } // namespace ghoti::sema

namespace ghoti::gir {

// Program-wide policy for disambiguating same-named GIR symbols.
//
// This policy scans every module that participates in a compilation and marks the pairs that
// really collide since GIR is a flat rep compared to normal namespaced decls
class symbol_scoping {
  public:
    using str_set_t = ankerl::unordered_dense::
        set<std::string, stdx::string_transparent_hash, stdx::string_transparent_eq>;

  public:
    symbol_scoping() = default;

    // Builds the policy from `entry_mod` plus every transitively imported module in `deps`.
    [[nodiscard]] static auto build(const sema::context&          ctx,
                                    const mod::module&            entry_mod,
                                    gsl::span<mod::module* const> deps) -> symbol_scoping;

    // The GIR symbol name to use for a declaration named `name` owned by the given table
    [[nodiscard]] auto name_for(usize owner_table_idx, std::string_view name) const -> std::string;

    // Whether `name_for(owner_table_idx, name)` would qualify the name.
    [[nodiscard]] auto is_scoped(usize owner_table_idx, std::string_view name) const -> bool;

  private:
    static auto key_of(usize owner_table_idx, std::string_view name) -> std::string;

  private:
    str_set_t scopable_defs_;
    str_set_t collided_names_;
};

} // namespace ghoti::gir
