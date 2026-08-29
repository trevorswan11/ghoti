#pragma once

#include <string>
#include <utility>

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

namespace ghoti::sema {

class type;

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

  private:
    ankerl::unordered_dense::map<generic_instantiation_key, generic_instantiation_entry> cache_;
};

} // namespace ghoti::sema
