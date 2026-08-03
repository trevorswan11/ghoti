#include "compiler/gir/const_value.hh"

#include <string>
#include <string_view>

#include <stdx/option.hh>

#include "compiler/gir/instruction.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/type.hh"

namespace ghoti::gir {

auto const_array::operator==(const const_array& other) const noexcept -> bool {
    return elements == other.elements;
}

auto const_struct::get_field_opt(std::string_view name) const noexcept
    -> stdx::option<const const_value&> {
    if (auto it{fields.find(name)}; it != fields.end()) { return it->second; }
    return stdx::none;
}

auto const_struct::operator==(const const_struct& other) const noexcept -> bool {
    return fields == other.fields;
}

auto const_union::operator==(const const_union& other) const noexcept -> bool {
    return active_field == other.active_field && payload == other.payload;
}

auto const_value::make_string(sema::context& ctx, std::string str) -> const_value {
    auto& t_u8{ctx.get_builtin_resolved_type(sema::type_kind::U8)};
    auto& t_c_str{ctx.get_slice(sema::types::mut::CONSTANT, true, t_u8)};
    return const_value{std::string{str}, t_c_str};
}

auto const_value::to_gir_value() const noexcept -> value {
    return data_.visit([this](const poison_val&) -> value { return value{undefined_val{}, type_}; },
                       [this](const const_array&) -> value { return value{void_val{}, type_}; },
                       [this](const const_struct&) -> value { return value{void_val{}, type_}; },
                       [this](const const_enum& e) -> value { return value{e.value, type_}; },
                       [this](const const_union&) -> value { return value{void_val{}, type_}; },
                       [this](const auto& v) -> value { return value{v, type_}; });
}

auto const_value::operator==(const const_value& other) const noexcept -> bool {
    if (data_.index() != other.data_.index()) {
        const auto l_int{as_int_opt()};
        const auto r_int{other.as_int_opt()};
        if (l_int && r_int) { return *l_int == *r_int; }
        return false;
    }
    return data_ == other.data_;
}

} // namespace ghoti::gir
