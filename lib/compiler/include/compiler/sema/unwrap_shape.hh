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

namespace builtin_impl {

inline constexpr std::string_view UNWRAPPABLE{"Unwrappable"};
inline constexpr std::string_view REWRAPPABLE{"Rewrappable"};
inline constexpr std::string_view FLOW{"Flow"};
inline constexpr std::string_view OUTPUT{"Output"};
inline constexpr std::string_view RESIDUAL{"Residual"};
inline constexpr std::string_view FROM{"From"};
inline constexpr std::string_view BRANCH{"branch"};
inline constexpr std::string_view FROM_RESIDUAL{"fromResidual"};
inline constexpr std::string_view FLOW_CONTINUE{"continue"};
inline constexpr std::string_view FLOW_BREAK{"break"};

} // namespace builtin_impl

[[nodiscard]] auto remap_type(context& ctx, type& t, const type& from, type& to) -> type&;

[[nodiscard]] auto find_assoc_type_alias(context&           ctx,
                                         const impl_record& rec,
                                         std::string_view   name) -> stdx::option<const type&>;

struct unwrap_info {
    gsl::not_null<const type*>        operand_type;
    gsl::not_null<const type*>        output_type;
    gsl::not_null<const type*>        residual_type;
    stdx::option<const type&>         flow_type{};
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
