#include "compiler/sema/impl_registry.hh"

#include <utility>
#include <vector>

#include <gsl/pointers>
#include <stdx/option.hh>
#include <stdx/result.hh>

#include "compiler/ast/id.hh"
#include "compiler/sema/type.hh"

namespace ghoti::sema {

auto impl_registry::record(impl_record rec)
    -> stdx::result<gsl::not_null<impl_record*>, ast::node_id> {
    // Only trait impls are one-per-`(I, T)`; several inherent `impl T` blocks may coexist.
    if (rec.interface_type) {
        for (const auto* existing : records_) {
            if (existing->interface_type == rec.interface_type &&
                existing->target_type == rec.target_type) {
                return stdx::err{existing->site};
            }
        }
    }

    auto* stored{arena_.make<impl_record>(std::move(rec)).get()};
    records_.emplace_back(stored);
    return gsl::not_null{stored};
}

auto impl_registry::record_parameterized(parameterized_impl pimpl)
    -> gsl::not_null<parameterized_impl*> {
    auto* stored{arena_.make<parameterized_impl>(std::move(pimpl)).get()};
    param_records_.emplace_back(stored);
    return gsl::not_null{stored};
}

auto impl_registry::find_by_site(ast::node_id site) noexcept -> stdx::option<impl_record&> {
    for (auto* r : records_) {
        if (r->site.get_index() == site.get_index() && r->site.get_kind() == site.get_kind()) {
            return *r;
        }
    }
    return stdx::none;
}

auto impl_registry::implements(const type& target, const type& iface) const noexcept -> bool {
    return lookup(target, iface).has_value();
}

auto impl_registry::methods_of(const type& target) const -> std::vector<extension_method> {
    std::vector<extension_method> out;
    for (const auto* r : records_) {
        if (r->target_type != &target) { continue; }
        for (const auto& m : r->methods) {
            out.emplace_back<extension_method>({
                .record = gsl::not_null<const impl_record*>{r},
                .method = gsl::not_null<const impl_record::method*>{&m},
            });
        }
    }
    return out;
}

} // namespace ghoti::sema
