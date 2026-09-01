#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <gsl/span>
#include <gsl/span_ext>
#include <stdx/assert.hh>
#include <stdx/hash.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/gir/const_value.hh"
#include "compiler/sema/generic.hh"

namespace ghoti::mod { enum class if_branch : u8; } // namespace ghoti::mod

namespace ghoti::sema {

class type;

// Constexpr argument values per monomorphization, keyed by mangled name
using constexpr_arg_map = ankerl::unordered_dense::map<std::string,
                                                       std::vector<gir::const_value>,
                                                       stdx::string_transparent_hash,
                                                       stdx::string_transparent_eq>;

// Per-monomorphization body typing, replayed at emit time: `[n]T` with a `constexpr n`, and the
// `@this()` shape of a `fn(T): type` constructor's member functions.
struct body_type_diff {
    std::vector<std::pair<usize, stdx::option<type&>>> node_types;
    std::vector<std::pair<usize, stdx::option<type&>>> explicit_types;
    std::vector<std::pair<usize, mod::if_branch>>      if_branches;
    std::vector<std::pair<usize, usize>>               match_arms;

    [[nodiscard]] auto empty() const noexcept -> bool {
        return node_types.empty() && explicit_types.empty() && if_branches.empty() &&
               match_arms.empty();
    }
};
using body_type_diff_map = ankerl::unordered_dense::
    map<std::string, body_type_diff, stdx::string_transparent_hash, stdx::string_transparent_eq>;

struct generic_instantiation_key {
    gsl::not_null<type*>              generic_fn_type;
    gsl::span<type*>                  arg_types;
    gsl::span<const gir::const_value> constexpr_args; // in parameter order

    [[nodiscard]] auto hash() const noexcept -> u64 {
        stdx::hasher h{reinterpret_cast<u64>(generic_fn_type.get())};
        for (const auto& arg : arg_types) {
            VERIFY(arg, "Null argument leaked from resolution");
            h.combine(reinterpret_cast<u64>(arg));
        }
        for (const auto& cx : constexpr_args) { h.combine(cx.hash()); }
        return h.finalize();
    }

    [[nodiscard]] auto operator==(const generic_instantiation_key& other) const noexcept
        -> bool = default;
};

} // namespace ghoti::sema

template <> struct ankerl::unordered_dense::hash<ghoti::sema::generic_instantiation_key> {
    using is_avalanching = void;
    using key_t          = ghoti::sema::generic_instantiation_key;
    [[nodiscard]] auto operator()(const key_t& key) const noexcept { return key.hash(); }
};

namespace ghoti::sema {

class generic_instantiation_cache {
  public:
    generic_instantiation_cache() noexcept = default;
    ~generic_instantiation_cache()         = default;
    MAKE_MOVE_CONSTRUCTABLE_ONLY(generic_instantiation_cache);

    [[nodiscard]] auto find(const generic_instantiation_key& key) const noexcept
        -> stdx::option<const generic_instantiation_entry&> {
        if (auto it{cache_.find(key)}; it != cache_.end()) { return it->second; }
        return stdx::none;
    }

    auto insert(generic_instantiation_key key, type& return_type, std::string mangled_name)
        -> void {
        cache_.emplace(std::move(key),
                       generic_instantiation_entry{
                           .return_type  = &return_type,
                           .mangled_name = std::move(mangled_name),
                       });
    }

    // Emit-time replay data, keyed by mangled instantiation name
    auto set_body_type_diff(std::string key, body_type_diff diff) -> void {
        body_type_diffs_.insert_or_assign(std::move(key), std::move(diff));
    }

    [[nodiscard]] auto get_body_type_diff(std::string_view key) const noexcept
        -> stdx::option<const body_type_diff&> {
        if (const auto it{body_type_diffs_.find(std::string{key})}; it != body_type_diffs_.end()) {
            return it->second;
        }
        return stdx::none;
    }

    auto set_constexpr_args(std::string key, std::vector<gir::const_value> args) -> void {
        constexpr_args_.insert_or_assign(std::move(key), std::move(args));
    }

    [[nodiscard]] auto get_constexpr_args(std::string_view key) const noexcept
        -> stdx::option<const std::vector<gir::const_value>&> {
        if (const auto it{constexpr_args_.find(std::string{key})}; it != constexpr_args_.end()) {
            return it->second;
        }
        return stdx::none;
    }

  private:
    ankerl::unordered_dense::map<generic_instantiation_key, generic_instantiation_entry> cache_;
    body_type_diff_map body_type_diffs_;
    constexpr_arg_map  constexpr_args_;
};

} // namespace ghoti::sema
