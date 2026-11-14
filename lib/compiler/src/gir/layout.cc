#include "compiler/gir/layout.hh"

#include <algorithm>
#include <functional>
#include <ranges>

#include <stdx/assert.hh>
#include <stdx/profiler.hh>
#include <stdx/types.hh>

#include "compiler/codegen/target.hh"
#include "compiler/sema/type.hh"

namespace ghoti::gir {

namespace {

[[nodiscard]] auto int_abi_bytes(u16 bits) -> usize {
    if (bits == 0) { return 0; }
    if (bits <= 8) { return 1; }
    if (bits <= 16) { return 2; }
    if (bits <= 32) { return 4; }
    if (bits <= 64) { return 8; }
    usize bytes{1};
    while (bytes * 8 < bits) { bytes *= 2; }
    return bytes;
}

// Integers wider than 32 bits and at most 64 take the target's 64-bit alignment, which is 4 on
// i386 SysV
[[nodiscard]] auto int_abi_align(u16 bits, const codegen::type_layout& layout) -> usize {
    if (bits > 32 && bits <= 64) { return layout.i64_align; }
    return std::max<usize>(int_abi_bytes(bits), 1);
}

[[nodiscard]] auto capture_field_align(const sema::types::closure_capture& capture,
                                       const codegen::type_layout&         layout) -> usize {
    return align_of(*capture.storage_type, layout);
}

[[nodiscard]] auto capture_field_size(const sema::types::closure_capture& capture,
                                      const codegen::type_layout&         layout) -> usize {
    return size_of(*capture.storage_type, layout);
}

[[nodiscard]] auto vector_bits(const sema::types::vector& vec, const codegen::type_layout& layout)
    -> u64 {
    return sema::vector_bits(vec, static_cast<u32>(layout.ptr_bytes) * 8);
}

} // namespace

auto struct_field_align(const sema::types::struct_t& st,
                        usize                        idx,
                        const codegen::type_layout&  layout) -> usize {
    const auto natural{align_of(*st.fields[idx], layout)};
    return std::max(natural, static_cast<usize>(st.explicit_field_alignment(idx)));
}

auto align_of(const sema::type& type, const codegen::type_layout& layout) -> usize {
    PROFILE_FUNCTION();
    switch (type.get_kind()) {
    case sema::type_kind::INT:            return int_abi_align(sema::int_width(type), layout);
    case sema::type_kind::BOOL:           return 1;
    case sema::type_kind::F16:            return 2;
    case sema::type_kind::F32:            return 4;
    case sema::type_kind::COMPTIME_INT:   return 4; // materializes as i32
    case sema::type_kind::F64:
    case sema::type_kind::COMPTIME_FLOAT: return layout.f64_align; // materializes as f64
    case sema::type_kind::F80:            return layout.f80_align;
    case sema::type_kind::F128:           return 16;
    case sema::type_kind::ISIZE:
    case sema::type_kind::USIZE:
    case sema::type_kind::POINTER:
    case sema::type_kind::REFERENCE:
    case sema::type_kind::FUNCTION:
    case sema::type_kind::SLICE:          return layout.ptr_bytes;
    case sema::type_kind::ARRAY:
        if (const auto arr{type.get_data().as_opt<sema::types::array>()}) {
            return align_of(arr->underlying, layout);
        }
        if (const auto def{type.get_data().as_opt<sema::types::deferred_array>()}) {
            return align_of(def->underlying, layout);
        }
        UNREACHABLE("type_kind::ARRAY associated with improper type");
    case sema::type_kind::VECTOR:
        return layout.vector_align(vector_bits(type.get_data().as<sema::types::vector>(), layout));
    case sema::type_kind::STRUCT:
        if (const auto st{type.get_data().as_opt<sema::types::struct_t>()}) {
            if (st->is_bit_packed()) {
                if (const auto bits{
                        sema::packed_backing_bits(*st, static_cast<u32>(layout.ptr_bytes) * 8)}) {
                    return int_abi_align(static_cast<u16>(*bits), layout);
                }
            }
            usize max_align{1};
            for (usize i{0}; i < st->fields.size(); ++i) {
                if (!st->fields[i]) { continue; }
                max_align = std::max(max_align, struct_field_align(*st, i, layout));
            }
            return max_align;
        }
        UNREACHABLE("type_kind::STRUCT associated with improper type");
    case sema::type_kind::UNION:
        if (const auto un{type.get_data().as_opt<sema::types::union_t>()}) {
            if (un->is_bit_packed()) {
                if (const auto bits{sema::packed_union_backing_bits(
                        *un, static_cast<u32>(layout.ptr_bytes) * 8)}) {
                    return int_abi_align(static_cast<u16>(*bits), layout);
                }
            }
            return std::ranges::fold_left(
                un->fields | std::views::filter([](const auto f) { return f.has_value(); }) |
                    std::views::transform([&layout](const auto f) { return align_of(*f, layout); }),
                1UZ,
                [](usize a, usize b) { return std::max(a, b); });
        }
        UNREACHABLE("type_kind::UNION associated with improper type");
    case sema::type_kind::ENUM:
        if (const auto en{type.get_data().as_opt<sema::types::enum_t>()}) {
            return align_of(en->underlying, layout);
        }
        UNREACHABLE("type_kind::ENUM associated with improper type");
    case sema::type_kind::CLOSURE:
        if (const auto cl{type.get_data().as_opt<sema::types::closure_t>()}) {
            return std::ranges::fold_left(cl->captures |
                                              std::views::transform([&layout](const auto& c) {
                                                  return capture_field_align(c, layout);
                                              }),
                                          1UZ,
                                          [](usize a, usize b) { return std::max(a, b); });
        }
        UNREACHABLE("type_kind::CLOSURE associated with improper type");
    default: return 1;
    }
}

auto size_of(const sema::type& type, const codegen::type_layout& layout) -> usize {
    PROFILE_FUNCTION();
    switch (type.get_kind()) {
    case sema::type_kind::VOID_:          return 0;
    case sema::type_kind::INT:            return int_abi_bytes(sema::int_width(type));
    case sema::type_kind::BOOL:           return 1;
    case sema::type_kind::F16:            return 2;
    case sema::type_kind::F32:            return 4;
    case sema::type_kind::COMPTIME_INT:   return 4; // materializes as i32
    case sema::type_kind::F64:
    case sema::type_kind::COMPTIME_FLOAT: return 8; // materializes as f64
    case sema::type_kind::F80:            return layout.f80_size;
    case sema::type_kind::F128:           return 16;
    case sema::type_kind::POINTER:
    case sema::type_kind::REFERENCE:      {
        // `&dyn I` / `^dyn I` is a fat `{ data, vtable }` pair, matching its LLVM lowering
        const auto referent{sema::pointee_of(type)};
        const bool is_fat{(referent && referent->get_kind() == sema::type_kind::DYN) ||
                          sema::is_fat_callable(type)};
        return is_fat ? 2 * layout.ptr_bytes : layout.ptr_bytes;
    }
    case sema::type_kind::FUNCTION:
        return sema::is_erased_fn(type) ? 2 * layout.ptr_bytes : layout.ptr_bytes;
    case sema::type_kind::ISIZE:
    case sema::type_kind::USIZE: return layout.ptr_bytes;
    case sema::type_kind::SLICE: return 2 * layout.ptr_bytes;
    case sema::type_kind::ARRAY:
        if (const auto arr{type.get_data().as_opt<sema::types::array>()}) {
            const auto elem_size{size_of(arr->underlying, layout)};
            const auto elem_align{align_of(arr->underlying, layout)};
            const auto elem_stride{elem_align > 0
                                       ? (elem_size + elem_align - 1) / elem_align * elem_align
                                       : elem_size};
            // A sentinel-terminated array stores one extra element for the terminator.
            const auto count{arr->len + (arr->null_terminated ? 1 : 0)};
            return count * (elem_stride == 0 ? elem_size : elem_stride);
        }
        UNREACHABLE("type_kind::ARRAY associated with improper type");
    case sema::type_kind::VECTOR:
        return layout.vector_size(vector_bits(type.get_data().as<sema::types::vector>(), layout));
    case sema::type_kind::STRUCT:
        if (const auto st{type.get_data().as_opt<sema::types::struct_t>()}) {
            if (st->is_bit_packed()) {
                if (const auto bits{
                        sema::packed_backing_bits(*st, static_cast<u32>(layout.ptr_bytes) * 8)}) {
                    return int_abi_bytes(static_cast<u16>(*bits));
                }
            }
            usize current_offset{0};
            usize max_align{1};
            for (usize i{0}; i < st->fields.size(); ++i) {
                const auto field{st->fields[i]};
                if (!field) { continue; }
                const auto f_align{struct_field_align(*st, i, layout)};
                max_align = std::max(max_align, f_align);
                if (f_align > 0) {
                    current_offset = (current_offset + f_align - 1) / f_align * f_align;
                }
                current_offset += size_of(*field, layout);
            }

            return max_align > 0 ? (current_offset + max_align - 1) / max_align * max_align
                                 : current_offset;
        }
        UNREACHABLE("type_kind::STRUCT associated with improper type");
    case sema::type_kind::UNION:
        if (const auto un{type.get_data().as_opt<sema::types::union_t>()}) {
            if (un->is_bit_packed()) {
                if (const auto bits{sema::packed_union_backing_bits(
                        *un, static_cast<u32>(layout.ptr_bytes) * 8)}) {
                    return int_abi_bytes(static_cast<u16>(*bits));
                }
            }
            usize max_size{0};
            usize max_align{1};
            for (const auto field : un->fields) {
                if (!field) { continue; }
                max_align = std::max(max_align, align_of(*field, layout));
                max_size  = std::max(max_size, size_of(*field, layout));
            }
            return max_align > 0 ? (max_size + max_align - 1) / max_align * max_align : max_size;
        }
        UNREACHABLE("type_kind::UNION associated with improper type");
    case sema::type_kind::ENUM:
        if (const auto en{type.get_data().as_opt<sema::types::enum_t>()}) {
            return size_of(en->underlying, layout);
        }
        UNREACHABLE("type_kind::ENUM associated with improper type");
    case sema::type_kind::CLOSURE:
        if (const auto cl{type.get_data().as_opt<sema::types::closure_t>()}) {
            usize current_offset{0};
            usize max_align{1};
            for (const auto& capture : cl->captures) {
                const auto c_align{capture_field_align(capture, layout)};
                max_align = std::max(max_align, c_align);
                if (c_align > 0) {
                    current_offset = (current_offset + c_align - 1) / c_align * c_align;
                }
                current_offset += capture_field_size(capture, layout);
            }
            return max_align > 0 ? (current_offset + max_align - 1) / max_align * max_align
                                 : current_offset;
        }
        UNREACHABLE("type_kind::CLOSURE associated with improper type");
    default: return 0;
    }
}

} // namespace ghoti::gir
