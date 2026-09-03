#pragma once

#include <algorithm>
#include <string_view>
#include <vector>

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

class impl_registry {
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

    // Every method reachable on `target` through any impl (inherent or trait). A name carried by
    // two records appears twice
    [[nodiscard]] auto methods_of(const type& target) const -> std::vector<extension_method>;
    [[nodiscard]] auto records(this auto&& self) noexcept -> auto& { return self.records_; }

  private:
    arena_alloc&              arena_;
    std::vector<impl_record*> records_;
};

} // namespace ghoti::sema
