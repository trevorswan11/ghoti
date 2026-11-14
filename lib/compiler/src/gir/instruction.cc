#include "compiler/gir/instruction.hh"

#include <string_view>
#include <utility>

#include <magic_enum/magic_enum.hpp>
#include <stdx/enum.hh>
#include <stdx/fixed/enum_map.hh>
#include <stdx/option.hh>

#include "compiler/ip/pool.hh"
#include "compiler/sema/type.hh"
#include "support/float128.hh"
#include "support/string_utils.hh"

namespace ghoti::gir {

auto value::constant(ip::pool&                 pool,
                     ip::value_data            payload,
                     stdx::option<sema::type&> t,
                     stdx::option<math_origin> computed) -> value {
    if (auto f{payload.as_opt<f128>()}; f && t) {
        *f = sema::fit_float(*f, *t, computed);
        if (sema::is_float(t->get_kind())) { computed.reset(); }
    }
    return value{pool.intern(std::move(payload), t), t, computed};
}

auto value::type_constant(ip::pool&                 pool,
                          stdx::option<sema::type&> denoted,
                          stdx::option<sema::type&> t) -> value {
    if (denoted) { return value{denoted->get_ip_index(), t}; }
    return value{pool.intern(ip::no_type_value{}, t), t};
}

auto value::retyped(ip::pool&                 pool,
                    stdx::option<sema::type&> t,
                    stdx::option<math_origin> computed) const -> value {
    if (is_local() || pool.is_type(constant_index())) { return value{data, t, computed}; }
    return constant(pool, pool.value_at(constant_index()).data, t, computed);
}

namespace {

constexpr auto INSTRUCTION_KIND_NAMES{[] {
    stdx::fixed::enum_map<instruction_kind, string_utils::lowercase_str<32>> map{};
    for (const auto kind : stdx::enum_range<instruction_kind>()) {
        map[kind] = string_utils::lowercase_str<32>{magic_enum::enum_name(kind)};
    }
    return map;
}()};

} // namespace

auto instruction_kind_name(instruction_kind kind) noexcept -> std::string_view {
    return INSTRUCTION_KIND_NAMES[kind].view();
}

} // namespace ghoti::gir
