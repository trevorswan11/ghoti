#pragma once

#include <string_view>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/module.hh"
#include "compiler/gir/segment.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "support/diagnostic.hh"

namespace ghoti::sema {

class type_checker {
  public:
    static auto check_types(gir::module& gir_mod, mod::module& ast_mod, context& ctx)
        -> mod::module_state;

  private:
    struct local_info {
        gsl::not_null<type*> type;
        bool                 is_alloca{false};
        bool                 is_const{false};
    };

  private:
    explicit type_checker(gir::module& gir_mod, context& ctx) noexcept
        : gir_mod_{gir_mod}, ctx_{ctx} {}

    auto check_function(gir::function& fn) -> void;
    auto check_segment(gir::function& fn, gir::segment& seg) -> void;
    auto check_instruction(gir::function& fn, const gir::instruction& inst) -> void;
    auto check_store(const gir::instruction& inst) -> void;

    auto emit_diagnostic(std::string_view message, error err, stdx::option<source_location> loc)
        -> void;

    auto get_operand_type(const gir::value& val) -> stdx::option<type&>;
    auto find_function(std::string_view name) const -> stdx::option<gir::function&>;

  private:
    gir::module&                                            gir_mod_;
    context&                                                ctx_;
    ankerl::unordered_dense::map<gir::local_id, local_info> locals_;
};

} // namespace ghoti::sema
