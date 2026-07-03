#include "compiler/gir/const_value.hh"

#include <string_view>

#include <stdx/option.hh>

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

} // namespace ghoti::gir
