#pragma once

#include <string>
#include <string_view>

#include <gsl/pointers>
#include <stdx/option.hh>

#include "compiler/sema/impl_registry.hh"
#include "compiler/sema/type.hh"

namespace ghoti::gir { class symbol_scoping; } // namespace ghoti::gir

namespace ghoti::sema {

struct context;

struct unwrap_info {
    gsl::not_null<const type*>        operand_type;
    gsl::not_null<const type*>        output_type;
    gsl::not_null<const type*>        residual_type;
    bool                              residual_is_void{false};
    gsl::not_null<const impl_record*> impl;

    [[nodiscard]] auto gir_method_name(std::string_view           method_name,
                                       const gir::symbol_scoping& scoping) const -> std::string;
};

struct rewrap_info {
    gsl::not_null<const type*>        return_type;
    gsl::not_null<const type*>        from_type;
    gsl::not_null<const impl_record*> impl;

    [[nodiscard]] auto gir_method_name(std::string_view           method_name,
                                       const gir::symbol_scoping& scoping) const -> std::string;
};

[[nodiscard]] auto unwrap_shape_of(context& ctx, const type& operand) -> stdx::option<unwrap_info>;
[[nodiscard]] auto rewrap_shape_of(context& ctx, const type& return_type)
    -> stdx::option<rewrap_info>;

} // namespace ghoti::sema
