#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/id.hh"
#include "compiler/sema/type.hh"

namespace ghoti::mod { struct module; } // namespace ghoti::mod

namespace ghoti::sema {

// One resolved `impl [I for] T { ... }` block. `interface_type` is null for an inherent impl.
struct impl_record {
    struct method {
        std::string_view          name;
        ast::node_id              decl;    // the `decl_stmt` in the impl body
        stdx::option<const type&> fn_type; // resolved once conformance runs
        bool                      is_pub;
    };

    stdx::option<const type&>        interface_type;
    stdx::option<const type&>        target_type;
    ast::node_id                     site;           // the `impl_stmt`, for diagnostics
    stdx::option<const mod::module&> enclosing;      // module the impl was written in
    usize                            body_scope_idx; // the impl block's own symbol table
    std::vector<method>              methods{};

    // Set for a record produced by expanding an `impl(P) ...` for one concrete target
    bool        from_parameterized{false};
    std::string gir_prefix{}; // The per-instantiation symbol prefix

    template <typename Self>
    [[nodiscard]] auto find_method(this Self&& self, std::string_view name) noexcept
        -> stdx::option<stdx::const_dispatch_t<Self, method>&> {
        if (auto it{std::ranges::find(self.methods, name, &method::name)};
            it != self.methods.end()) {
            return *it;
        }
        return stdx::none;
    }
};

// A method reachable on a type through some impl, as surfaced to `.`-resolution.
struct extension_method {
    gsl::not_null<const impl_record*>         record;
    gsl::not_null<const impl_record::method*> method;
};

// A `impl(P: type, ...) [I for] Ctor(P) { ... }` held un-expanded
struct parameterized_impl {
    ast::node_id                     site;           // the `impl_stmt`
    stdx::option<const type&>        interface_type; // none for an inherent parameterized impl
    ast::node_id                     base_ctor_fn;   // the generic ctor's `function_expr` node
    stdx::option<const mod::module&> enclosing;      // module the impl body lives in
    usize                            body_scope_idx; // the impl block's own symbol table
    // none here means the param could not be matched positionally; the impl is skipped
    std::vector<stdx::opt_size> param_to_ctor_arg{};
};

// Lives on the shared registry so a monomorphization triggered from any consuming module can remap
// the impl's method signatures. Method bodies are re-resolved per instantiation, not remapped.
struct param_impl_template {
    stdx::option<const type&> abstract_target{};
    std::vector<const type*>  sentinels{}; // per impl param; null for a constexpr one
};

class impl_registry {
  public:
    using template_param_map =
        ankerl::unordered_dense::map<const parameterized_impl*, param_impl_template>;
    using param_worklist = ankerl::unordered_dense::set<const parameterized_impl*>;
    using expanded_param_map =
        ankerl::unordered_dense::map<const type*,
                                     ankerl::unordered_dense::set<const parameterized_impl*>>;

  public:
    explicit impl_registry(arena_alloc& arena) noexcept : arena_{arena} {}
    ~impl_registry() = default;
    MAKE_MOVE_CONSTRUCTABLE_ONLY(impl_registry)

    // Records an impl skeleton in the sema arena, enforcing one impl per  pair program-wide
    [[nodiscard]] auto record(impl_record rec)
        -> stdx::result<gsl::not_null<impl_record*>, ast::node_id>;

    template <typename Self>
    [[nodiscard]] auto lookup(this Self&& self, const type& target, const type& iface) noexcept
        -> stdx::option<stdx::const_dispatch_t<Self, impl_record>&> {
        for (auto* r : self.records_) {
            if (r->target_type == &target && r->interface_type == &iface) { return *r; }
        }
        return stdx::none;
    }

    // The record whose `impl_stmt` node is `site` (inherent or trait).
    [[nodiscard]] auto find_by_site(ast::node_id site) noexcept -> stdx::option<impl_record&>;
    [[nodiscard]] auto implements(const type& target, const type& iface) const noexcept -> bool;

    // Parameterized `impl(P) ...` blocks, held un-expanded until a concrete target materializes.
    auto record_parameterized(parameterized_impl pimpl) -> gsl::not_null<parameterized_impl*>;
    [[nodiscard]] auto param_records(this auto&& self) noexcept -> auto& {
        return self.param_records_;
    }
    [[nodiscard]] auto param_record_by_site(ast::node_id       site,
                                            const mod::module& enclosing) noexcept
        -> stdx::option<parameterized_impl&>;

    // The shared template resolution of a parameterized impl (see `param_impl_template`).
    auto set_template(const parameterized_impl& pimpl, param_impl_template tmpl) -> void {
        templates_.insert_or_assign(&pimpl, std::move(tmpl));
    }
    [[nodiscard]] auto get_template(const parameterized_impl& pimpl) const
        -> const param_impl_template* {
        const auto it{templates_.find(&pimpl)};
        return it == templates_.end() ? nullptr : &it->second;
    }
    // Records that `pimpl` has been expanded onto `target`; returns false if it already was.
    [[nodiscard]] auto mark_expanded(const type& target, const parameterized_impl& pimpl) -> bool {
        return expanded_[&target].emplace(&pimpl).second;
    }

    // Re-entrancy guard for `build_param_impl_template`
    [[nodiscard]] auto begin_build(const parameterized_impl& pimpl) -> bool {
        return building_.emplace(&pimpl).second;
    }
    auto end_build(const parameterized_impl& pimpl) -> void { building_.erase(&pimpl); }

    // Every method reachable on `target` through any impl (inherent or trait). A name carried by
    // two records appears twice
    [[nodiscard]] auto methods_of(const type& target) const -> std::vector<extension_method>;
    [[nodiscard]] auto records(this auto&& self) noexcept -> auto& { return self.records_; }

  private:
    arena_alloc&                     arena_;
    std::vector<impl_record*>        records_;
    std::vector<parameterized_impl*> param_records_;
    template_param_map               templates_;
    expanded_param_map               expanded_;
    param_worklist                   building_;
};

} // namespace ghoti::sema
