#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/gir/instruction.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"

namespace ghoti::uir::sema {

namespace { constexpr std::string_view SOURCE_LOCATION{"SourceLocation"}; } // namespace

auto analyzer::usize_constant(u64 value) -> gir::value {
    auto& usize_type{builtin(gs::type_kind::USIZE)};
    return constant(comp_.constants().intern(value, usize_type), usize_type);
}

auto analyzer::string_slice(std::string_view text, gs::type& slice_type) -> gir::value {
    // The text may live in the constant pool, which interning it below can move
    const std::string owned{text};
    auto&             ctx{comp_.ctx()};
    const auto&       slice{slice_type.get_data().as<gs::types::slice>()};

    // The field address is mutable whatever the slice's own mutability
    auto&      elem_ptr_type{ctx.get_pointer(gs::types::mut::MUTABLE, slice.underlying)};
    const auto slot{builder_.emit_alloca(slice_type)};
    const auto ptr_field{builder_.emit_get_element_ptr(
        gir::value{slot, slice_type}, {usize_constant(SLICE_PTR_FIELD)}, elem_ptr_type)};
    builder_
        .emit_store(gir::value{ptr_field, elem_ptr_type},
                    constant(comp_.constants().intern(owned, elem_ptr_type), elem_ptr_type))
        .is_initializer = true;
    auto&      usize_type{builtin(gs::type_kind::USIZE)};
    const auto len_field{builder_.emit_get_element_ptr(
        gir::value{slot, slice_type}, {usize_constant(SLICE_LEN_FIELD)}, usize_type)};
    builder_.emit_store(gir::value{len_field, usize_type}, usize_constant(owned.size()))
        .is_initializer = true;
    return gir::value{builder_.emit_load(gir::value{slot, slice_type}, slice_type), slice_type};
}

auto analyzer::string_as_slice(std::string_view text, gs::type& slice_type) -> gir::value {
    // A constant slice reads the string in place, a mutable one gets a buffer of its own
    if (slice_type.is_constant()) { return string_slice(text, slice_type); }
    const std::string owned{text};
    auto&             ctx{comp_.ctx()};
    const auto&       slice{slice_type.get_data().as<gs::types::slice>()};
    auto& arr_type{ctx.get_array(gs::types::mut::MUTABLE, true, owned.size(), slice.underlying)};
    const auto slot{builder_.emit_alloca(arr_type)};
    builder_
        .emit_store(gir::value{slot, arr_type},
                    constant(comp_.constants().intern(owned, arr_type), arr_type))
        .is_initializer = true;
    auto&      ptr_type{ctx.get_pointer(gs::types::mut::MUTABLE, slice.underlying)};
    auto&      view_type{ctx.get_slice(gs::types::mut::MUTABLE, true, slice.underlying)};
    auto&      usize_type{builtin(gs::type_kind::USIZE)};
    const auto view{builder_.emit_alloca(view_type)};
    const auto elem0{
        builder_.emit_get_element_ptr(gir::value{slot, arr_type}, {usize_constant(0)}, ptr_type)};
    const auto ptr_field{builder_.emit_get_element_ptr(
        gir::value{view, view_type}, {usize_constant(SLICE_PTR_FIELD)}, ptr_type)};
    builder_.emit_store(gir::value{ptr_field, ptr_type}, gir::value{elem0, ptr_type})
        .is_initializer = true;
    const auto len_field{builder_.emit_get_element_ptr(
        gir::value{view, view_type}, {usize_constant(SLICE_LEN_FIELD)}, usize_type)};
    builder_.emit_store(gir::value{len_field, usize_type}, usize_constant(owned.size()))
        .is_initializer = true;
    return gir::value{builder_.emit_load(gir::value{view, view_type}, view_type), slice_type};
}

auto analyzer::emit_panic(std::string_view message, const source& src) -> result<void> {
    auto& ctx{comp_.ctx()};
    auto& handler{*TRY(comp_.prelude_function(PANIC_HANDLER, u_, src))};
    auto& location_type{*TRY(comp_.prelude_type(SOURCE_LOCATION, u_, src))};
    auto& pool{comp_.constants()};
    auto& u32_type{ctx.get_int(32, false)};

    const auto at{comp_.location_of(u_, src).one_based()};
    auto&      path_type{ctx.get_slice(gs::types::mut::CONSTANT, false, ctx.get_int(8, false))};
    std::vector<gir::value> location_args;
    location_args.emplace_back(
        constant(pool.intern(u_.source->path.string(), path_type), path_type));
    location_args.emplace_back(constant(pool.intern(u64{at.line}, u32_type), u32_type));
    location_args.emplace_back(constant(pool.intern(u64{at.column}, u32_type), u32_type));
    const auto location{
        builder_.emit_builtin_call("@src", std::move(location_args), location_type)};

    auto& message_type{ctx.get_slice(gs::types::mut::CONSTANT, true, ctx.get_int(8, false))};
    std::vector<gir::value> args;
    args.emplace_back(string_slice(message, message_type));
    args.emplace_back(gir::value{*location, location_type});
    DISCARD(builder_.emit_call(handler.symbol, std::move(args), *handler.return_type));
    builder_.emit_unreachable();
    return {};
}

auto analyzer::guard(gir::value ok, std::string_view message, const source& src) -> result<void> {
    const auto pass{new_segment()};
    const auto fail{new_segment()};
    builder_.emit_cond_goto(ok, pass, fail);
    switch_to(fail);
    TRY(emit_panic(message, src));
    switch_to(pass);
    return {};
}

auto analyzer::check_not_null(const gir::value& ptr, const source& src) -> result<void> {
    if (!runtime_safety() || !ptr.type || ptr.type->get_kind() != gs::type_kind::POINTER) {
        return {};
    }

    auto&      bool_type{builtin(gs::type_kind::BOOL)};
    auto&      null_type{builtin(gs::type_kind::NULLPTR)};
    const auto is_null{builder_.emit_binary(
        gir::instruction_kind::EQ,
        ptr,
        constant(comp_.constants().intern(ip::nullptr_value{}, null_type), null_type),
        bool_type)};
    // The failing edge comes first here
    const auto fail{new_segment()};
    const auto pass{new_segment()};
    builder_.emit_cond_goto(gir::value{is_null, bool_type}, fail, pass);
    switch_to(fail);
    TRY(emit_panic("dereference of null pointer", src));
    switch_to(pass);
    return {};
}

auto analyzer::check_active_field(const gir::value& union_address,
                                  u64               field,
                                  std::string_view  name,
                                  const source&     src) -> result<void> {
    if (!runtime_safety()) { return {}; }
    auto&      i32_type{comp_.ctx().get_int(32, true)};
    auto&      bool_type{builtin(gs::type_kind::BOOL)};
    const auto tag_ptr{
        builder_.emit_get_element_ptr(union_address, {usize_constant(UNION_TAG_FIELD)}, i32_type)};
    const auto tag{builder_.emit_load(gir::value{tag_ptr, i32_type}, i32_type)};
    const auto active{builder_.emit_binary(
        gir::instruction_kind::EQ,
        gir::value{tag, i32_type},
        constant(comp_.constants().intern(static_cast<i64>(field), i32_type), i32_type),
        bool_type)};
    return guard(gir::value{active, bool_type},
                 fmt::format("accessed inactive union field '{}'", name),
                 src);
}

} // namespace ghoti::uir::sema
