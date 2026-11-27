#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gsl/span>
#include <gsl/util>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/ast/attributes.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/ip/index.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"
#include "support/big_int.hh"
#include "support/int128.hh"

namespace ghoti::uir::sema {

namespace {

constexpr std::string_view CONST_CONSTANT{"`const` initializer must be known at compile time"};
constexpr std::string_view ASSIGN_CONSTANT{"Cannot assign to constant variable"};
constexpr std::string_view ASSIGN_NOT_FOLDABLE{
    "`comptime let mut` assignment must be known at compile time"};
constexpr std::string_view STORE_UNDER_RUNTIME{
    "`comptime let mut` assignment depends on a runtime condition"};

// A binding's written type, none when it is `auto` and takes its initializer's
[[nodiscard]] auto written_type_of(stdx::option<ref> type) -> stdx::option<ref> {
    if (type && as_well_known(*type) == well_known::auto_type) { return stdx::none; }
    return type;
}

} // namespace

auto analyzer::local_alignment(gsl::span<const inst_id> attrs) -> result<stdx::option<u64>> {
    if (attrs.empty()) { return stdx::none; }
    const auto resolved{TRY(fold_attributes(
        attrs, {.target = ast::attribute_target::DECL, .returns_void = false, .declared_by = {}}))};
    return resolved.alignment;
}

auto analyzer::analyze_alloc_mut(inst_id inst, bool is_comptime_alloc) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    rec.skip_str();
    const auto              type_ref{written_type_of(rec.ref_field())};
    const auto              alignment{TRY(local_alignment(rec.body_field()))};
    stdx::option<gs::type&> type;
    if (type_ref) { type.emplace(*TRY(resolve_type(*type_ref, src))); }

    // Mutable state at compile time lives in a comptime allocation
    if (is_comptime_alloc || is_comptime()) {
        return comp_.new_alloc({
            .type                = type,
            .value               = stdx::none,
            .frame               = frame_,
            .runtime_depth       = runtime_depth_,
            .keeps_comptime_type = is_comptime_alloc,
        });
    }

    // A type only exists at compile time, so a runtime slot can't hold one
    if (type && type->get_kind() == gs::type_kind::TYPE) {
        return comp_.report(
            u_, src, gs::error::MUTABLE_TYPE_BINDING, std::string{messages::MUTABLE_TYPE_BINDING});
    }
    if (type && type->get_kind() == gs::type_kind::NORETURN) {
        return comp_.report(u_,
                            src,
                            gs::error::TYPE_MISMATCH,
                            "'noreturn' has no values, so nothing can be stored with this type");
    }

    if (!type) {
        if (alignment) { return comp_.unsupported(u_, src, "an aligned `let mut` with no type"); }

        // The first store gives an untyped `let mut` its type
        return place{
            .address = {}, .pointee = &builtin(gs::type_kind::UNDEFINED), .is_mutable = true};
    }

    const auto slot{builder_.emit_alloca(*type, {}, false, alignment)};
    return place{.address = gir::value{slot, *type}, .pointee = type.get(), .is_mutable = true};
}

auto analyzer::store_comptime(const comptime_place& p, const operand& value, const source& src)
    -> result<void> {
    auto& alloc{comp_.alloc_at(p)};
    if (!p.borrowed && alloc.frame != frame_ && alloc.frame != env_frame_) {
        return comp_.unsupported(u_, src, "a store to another analysis's comptime allocation");
    }

    if (!p.borrowed && alloc.runtime_depth != runtime_depth_) {
        return comp_.report(
            u_, src, gs::error::COMPTIME_MUT_ASSIGN_NOT_FOLDABLE, std::string{STORE_UNDER_RUNTIME});
    }

    // An element or field is stored by rebuilding the value around it
    if (!p.path.empty()) {
        auto&      element_type{*p.path.back().type};
        const auto stored{[&] {
            const auto scope{comptime_scope()};
            return coerce_to(value, element_type, src, {.what = use_site::kind::STORE});
        }()};
        const auto typed{TRY(stored)};
        const auto element{TRY(require_comptime(typed, src))};
        const auto whole_type{comp_.alloc_at(p).type};
        if (!whole_type) {
            return comp_.unsupported(u_, src, "an element of untyped comptime memory");
        }
        const auto rebuilt{
            TRY(with_element(comp_.alloc_at(p).value, *whole_type, p.path, element, src))};
        comp_.alloc_at(p).value = rebuilt;
        comp_.note_store(p);
        return {};
    }

    // An untyped `.{...}` or `.name` is known once it takes the slot's type
    if (is_untyped_literal(value)) {
        if (!alloc.type) {
            return comp_.unsupported(u_, src, "an untyped literal with no type to take");
        }

        auto&      type{*alloc.type};
        const auto typed{[&] {
            const auto scope{comptime_scope()};
            return coerce_to(value, type, src, {.what = use_site::kind::STORE});
        }()};
        return store_comptime(p, TRY(typed), src);
    }

    DISCARD(TRY(require_comptime(value, src)));
    if (!alloc.type) {
        // An untyped `comptime let mut` stays comptime_int, an untyped `let mut` takes a type
        auto& from{type_of(value)};
        alloc.type.emplace(alloc.keeps_comptime_type ? from : type_of(TRY(concrete(value, src))));
    }

    const auto stored{TRY(coerce_to(value, *alloc.type, src, {.what = use_site::kind::STORE}))};

    // `alloc` may have moved while coercing
    comp_.alloc_at(p).value = *comptime_value(stored);
    comp_.note_store(p);
    return {};
}

auto analyzer::comptime_range(const operand& dest, const source& src) -> result<memory_view> {
    const auto into{view_of(dest)};
    if (!into) { return comp_.unsupported(u_, src, "a copy into this range at compile time"); }
    if (!into->is_mutable) {
        return comp_.report(u_, src, gs::error::ASSIGNMENT_TO_CONST, std::string{ASSIGN_CONSTANT});
    }

    return *into;
}

auto analyzer::comptime_range_copy(const operand&   dest,
                                   const operand&   from,
                                   std::string_view what,
                                   const source&    src) -> result<void> {
    const auto into{TRY(comptime_range(dest, src))};
    auto&      pool{comp_.constants()};

    // The source is read whole before any element is written, so the two may overlap
    std::vector<operand> elements;
    if (const auto view{view_of(from)}) {
        for (u64 i{0}; i < view->len; ++i) {
            auto element{view->array};
            element.path.emplace_back(
                comptime_step{static_cast<u32>(view->offset + i), view->elem});
            elements.emplace_back(load_comptime(element));
        }
    } else {
        auto& array{comp_.ctx().get_array(gs::types::mut::CONSTANT, false, into.len, *into.elem)};
        const auto source_value{
            is_untyped_literal(from)
                ? TRY(coerce_to(from, array, src, {.what = use_site::kind::STORE}))
                : TRY(read_through_reference(from, src))};
        const auto  known{TRY(require_comptime(source_value, src))};
        const auto& data{pool.value_at(known).data};
        if (const auto agg{data.as_opt<ip::aggregate_value>()}) {
            for (const auto& e : agg->elements) {
                elements.emplace_back(e ? operand{*e}
                                        : operand{pool.intern(ip::undefined_value{}, *into.elem)});
            }
        } else if (const auto text{data.as_opt<std::string>()}) {
            for (const char c : *text) {
                elements.emplace_back(pool.intern(u64{static_cast<u8>(c)}, *into.elem));
            }
        } else {
            return comp_.unsupported(u_, src, "a copy from this value at compile time");
        }
    }

    if (elements.size() != into.len) {
        return comp_.report(u_,
                            src,
                            gs::error::COMPTIME_EVALUATION_FAILED,
                            fmt::format("{} with mismatched slice lengths", what));
    }

    for (u64 i{0}; i < into.len; ++i) {
        auto element{into.array};
        element.path.emplace_back(comptime_step{static_cast<u32>(into.offset + i), into.elem});
        TRY(store_comptime(element, elements[i], src));
    }

    return {};
}

auto analyzer::comptime_fill(const operand& dest, const operand& value, const source& src)
    -> result<void> {
    const auto into{TRY(comptime_range(dest, src))};
    auto&      ctx{comp_.ctx()};
    if (!gs::is_same_unqualified(*into.elem, ctx.get_int(8, false)) &&
        !gs::is_same_unqualified(*into.elem, ctx.get_int(8, true))) {
        return comp_.unsupported(u_, src, "a `@memset` of wide elements at compile time");
    }

    const auto byte{
        TRY(coerce_to(value, comp_.ctx().get_int(8, false), src, {.what = use_site::kind::STORE}))};
    for (u64 i{0}; i < into.len; ++i) {
        auto element{into.array};
        element.path.emplace_back(comptime_step{static_cast<u32>(into.offset + i), into.elem});
        TRY(store_comptime(element, byte, src));
    }

    return {};
}

auto analyzer::load_comptime(const comptime_place& p) -> operand {
    auto& pool{comp_.constants()};
    auto& alloc{comp_.alloc_at(p)};
    auto& type{alloc.type.value_or(builtin(gs::type_kind::UNDEFINED))};
    auto  value{alloc.value.value_or(pool.intern(ip::undefined_value{}, type))};
    auto* container{&type};
    for (const auto& step : p.path) {
        const auto& data{pool.value_at(value).data};
        const auto  ut{container->get_data().as_opt<gs::types::union_t>()};
        container = step.type.get();

        // A union's field holds its payload while it is the active one
        if (const auto un{data.as_opt<ip::union_value>()}; un && ut) {
            const bool active{step.index < ut->field_names.size() &&
                              un->active_field == ut->field_names[step.index]};
            value = active && !un->payload.empty() ? un->payload.front()
                                                   : pool.intern(ip::undefined_value{}, *step.type);
            continue;
        }

        if (const auto agg{data.as_opt<ip::aggregate_value>()};
            agg && step.index < agg->elements.size() && agg->elements[step.index]) {
            value = *agg->elements[step.index];
        } else if (const auto text{data.as_opt<std::string>()}; text && step.index < text->size()) {
            const auto byte{static_cast<u8>((*text)[step.index])};
            value = intern_int(pool, big_int{static_cast<u128>(byte)}, *step.type);
        } else {
            value = pool.intern(ip::undefined_value{}, *step.type);
        }
    }
    return value;
}

auto analyzer::comptime_copy(const comptime_place& p, const source& src) -> result<place> {
    auto&      type{type_of(operand{p})};
    const auto value{TRY(as_runtime(load_comptime(p), src))};
    return spill(value, type, false);
}

auto analyzer::memory_address(const comptime_place& p, gs::type& type) -> ip::index {
    std::vector<u32> path;
    path.reserve(p.path.size());
    for (const auto& step : p.path) { path.emplace_back(step.index); }
    return comp_.constants().intern(ip::memory_value{.alloc = p.alloc, .path = std::move(path)},
                                    type);
}

auto analyzer::memory_place(const operand& op) -> stdx::option<comptime_place> {
    const auto index{comptime_value(op)};
    auto&      pool{comp_.constants()};
    if (!index || pool.is_type(*index)) { return stdx::none; }
    const auto memory{pool.value_at(*index).data.as_opt<ip::memory_value>()};
    if (!memory) { return stdx::none; }

    // Each step's type, from the array, struct or union holding it
    comptime_place p{.alloc = memory->alloc, .path = {}, .borrowed = true};
    auto           type{comp_.alloc_at(p).type};
    for (const auto i : memory->path) {
        if (!type) { return stdx::none; }
        stdx::option<gs::type&> next;
        if (const auto arr{type->get_data().as_opt<gs::types::array>()}) {
            next.emplace(arr->underlying);
        } else if (const auto st{type->get_data().as_opt<gs::types::struct_t>()};
                   st && i < st->fields.size()) {
            next = st->fields[i];
        } else if (const auto ut{type->get_data().as_opt<gs::types::union_t>()};
                   ut && i < ut->field_names.size()) {
            next.emplace(ut->type_at(i));
        }

        if (!next) { return stdx::none; }
        p.path.emplace_back(comptime_step{i, next.get()});
        type = next;
    }

    return p;
}

auto analyzer::memory_target(const operand& object) -> stdx::option<comptime_place> {
    if (const auto p{object.as_opt<comptime_place>()}) {
        if (!gs::pointee_of(type_of(object))) { return stdx::none; }
        return memory_place(load_comptime(*p));
    }

    return memory_place(object);
}

auto analyzer::held_address(const operand& object) -> operand {
    const auto p{object.as_opt<comptime_place>()};
    if (!p) { return object; }
    auto& type{type_of(object)};
    if (!gs::pointee_of(type) && type.get_kind() != gs::type_kind::SLICE) { return object; }
    return load_comptime(*p);
}

auto analyzer::view_of(const operand& object) -> stdx::option<memory_view> {
    const auto array_view{[&](const comptime_place& p,
                              u64                   offset,
                              stdx::option<u64>     len) -> stdx::option<memory_view> {
        auto&      type{type_of(operand{p})};
        const auto arr{type.get_data().as_opt<gs::types::array>()};
        if (!arr) { return stdx::none; }
        return memory_view{.array      = p,
                           .offset     = offset,
                           .len        = len.value_or(arr->len),
                           .elem       = &arr->underlying,
                           .is_mutable = !type.is_constant()};
    }};

    const auto held{held_address(object)};
    if (const auto p{held.as_opt<comptime_place>()}) { return array_view(*p, 0, stdx::none); }

    const auto index{comptime_value(held)};
    const auto place{memory_place(held)};
    if (!index || !place) { return stdx::none; }
    const auto& memory{comp_.constants().value_at(*index).data.as<ip::memory_value>()};
    auto&       type{type_of(held)};
    if (type.get_kind() == gs::type_kind::SLICE) {
        return array_view(*place, memory.offset, memory.len);
    }

    return array_view(*place, 0, stdx::none);
}

auto analyzer::memory_slice(const memory_view& view, u64 lo, u64 hi) -> ip::index {
    auto&            slice_type{comp_.ctx().get_slice(
        view.is_mutable ? gs::types::mut::MUTABLE : gs::types::mut::CONSTANT, false, *view.elem)};
    std::vector<u32> path;
    for (const auto& step : view.array.path) { path.emplace_back(step.index); }
    return comp_.constants().intern(ip::memory_value{.alloc  = view.array.alloc,
                                                     .path   = std::move(path),
                                                     .offset = view.offset + lo,
                                                     .len    = hi - lo},
                                    slice_type);
}

auto analyzer::memory_element(const operand& object, ref index_ref, const source& src)
    -> result<stdx::option<comptime_place>> {
    const auto held{held_address(object)};
    auto&      type{type_of(held)};
    const bool slice{type.get_kind() == gs::type_kind::SLICE};
    const auto pointee{gs::pointee_of(type)};
    const bool element_pointer{type.get_kind() == gs::type_kind::POINTER && pointee &&
                               !pointee->get_data().is<gs::types::array>()};
    if (!slice && !element_pointer) { return stdx::option<comptime_place>{}; }

    // A slice indexes the array it views, an element pointer the elements around it
    stdx::option<memory_view> view;
    if (slice) {
        view = view_of(held);
    } else if (const auto at{memory_place(held)}; at && !at->path.empty()) {
        auto parent{*at};
        parent.path.pop_back();
        if (auto v{view_of(operand{parent})}) {
            v->offset = at->path.back().index;
            v->len    = v->len - v->offset;
            view      = v;
        }
    }
    if (!view) { return stdx::option<comptime_place>{}; }

    const auto at{comptime_value(TRY(resolve_value(index_ref, src)))};
    const auto number{at ? int_of(comp_.constants().value_at(*at)) : stdx::none};
    const auto small{number ? number->to_u128() : stdx::none};
    if (!small) { return comp_.needs_runtime(u_, src); }
    if (*small >= view->len) {
        return comp_.report(u_, src, gs::error::COMPTIME_EVALUATION_FAILED, "index out of bounds");
    }

    auto element{view->array};
    element.path.emplace_back(
        comptime_step{static_cast<u32>(view->offset + static_cast<u64>(*small)), view->elem});
    return stdx::option<comptime_place>{std::move(element)};
}

auto analyzer::memory_field(const operand& object, std::string_view name) -> stdx::option<operand> {
    if (name != "len" && name != "ptr") { return stdx::none; }
    const auto held{held_address(object)};
    if (!held.is<comptime_place>() && !memory_place(held)) { return stdx::none; }
    const auto view{view_of(held)};
    if (!view) { return stdx::none; }
    if (name == "len") {
        return operand{comp_.constants().intern(view->len, builtin(gs::type_kind::USIZE))};
    }

    auto first{view->array};
    first.path.emplace_back(comptime_step{static_cast<u32>(view->offset), view->elem});
    auto& ptr_type{comp_.ctx().get_pointer(
        view->is_mutable ? gs::types::mut::MUTABLE : gs::types::mut::CONSTANT, *view->elem)};
    return operand{memory_address(first, ptr_type)};
}

auto analyzer::comptime_destination(const operand& target) -> stdx::option<comptime_place> {
    // Assigning to a reference writes what it refers to
    const auto through{[&](const operand& held) -> stdx::option<comptime_place> {
        if (type_of(held).get_kind() != gs::type_kind::REFERENCE) { return stdx::none; }
        return memory_place(held);
    }};

    if (const auto p{target.as_opt<comptime_place>()}) {
        if (const auto referent{through(load_comptime(*p))}) { return referent; }
        return *p;
    }

    return through(target);
}

auto analyzer::with_element(stdx::option<ip::index>        current,
                            gs::type&                      type,
                            gsl::span<const comptime_step> path,
                            ip::index                      value,
                            const source&                  src) -> result<ip::index> {
    if (path.empty()) { return value; }
    auto& pool{comp_.constants()};

    // Storing a union's field makes it the active one
    if (const auto ut{type.get_data().as_opt<gs::types::union_t>()}) {
        const auto& step{path.front()};
        if (step.index >= ut->field_names.size()) {
            return comp_.unsupported(u_, src, "a store into an element of this comptime value");
        }

        const std::string       name{ut->field_names[step.index]};
        stdx::option<ip::index> held;
        if (current) {
            const auto un{pool.value_at(*current).data.as_opt<ip::union_value>()};
            if (un && un->active_field == name && !un->payload.empty()) {
                held = un->payload.front();
            }
        }

        const auto inner{TRY(with_element(held, *step.type, path.subspan(1), value, src))};
        std::vector<ip::index> payload;
        if (step.type->get_kind() != gs::type_kind::VOID_) { payload.emplace_back(inner); }
        return pool.intern(ip::union_value{.active_field = name, .payload = std::move(payload)},
                           type);
    }

    // Each element's type, from the array or struct holding them
    std::vector<stdx::option<gs::type&>> types;
    if (const auto arr{type.get_data().as_opt<gs::types::array>()}) {
        types.assign(arr->len, arr->underlying);
    } else if (const auto vec{type.get_data().as_opt<gs::types::vector>()}) {
        types.assign(vec->len, vec->underlying);
    } else if (const auto st{type.get_data().as_opt<gs::types::struct_t>()}) {
        types.assign(st->fields.begin(), st->fields.end());
    } else {
        return comp_.unsupported(u_, src, "a store into an element of this comptime value");
    }

    // What the value held, an element nothing has written being undefined
    std::vector<stdx::option<ip::index>> elements(types.size());
    if (current) {
        const auto data{pool.value_at(*current).data};
        if (const auto agg{data.as_opt<ip::aggregate_value>()};
            agg && agg->elements.size() == types.size()) {
            elements = agg->elements;
        } else if (const auto text{data.as_opt<std::string>()};
                   text && text->size() >= types.size()) {
            for (usize i{0}; i < types.size(); ++i) {
                const auto byte{static_cast<u8>((*text)[i])};
                elements[i] = intern_int(pool, big_int{static_cast<u128>(byte)}, *types[i]);
            }
        }
    }

    for (usize i{0}; i < types.size(); ++i) {
        if (!elements[i] && types[i]) {
            elements[i] = pool.intern(ip::undefined_value{}, *types[i]);
        }
    }

    const auto& step{path.front()};
    elements[step.index] =
        TRY(with_element(elements[step.index], *step.type, path.subspan(1), value, src));
    return pool.intern(ip::aggregate_value{std::move(elements)}, type);
}

auto analyzer::comptime_elem_place(const comptime_place& p, ref index_ref, const source& src)
    -> result<operand> {
    const auto at{comptime_value(TRY(resolve_value(index_ref, src)))};
    auto&      container{type_of(operand{p})};
    const auto arr{container.get_data().as_opt<gs::types::array>()};
    const auto vec{container.get_data().as_opt<gs::types::vector>()};
    const auto small{at ? int_of(comp_.constants().value_at(*at)).and_then([](const big_int& b) {
        return b.to_i128();
    })
                        : stdx::none};
    if ((!arr && !vec) || !small) {
        return comp_.unsupported(u_, src, "an element of comptime memory");
    }

    // A vector's lanes are its elements
    const u64 len{arr ? arr->len : vec->len};
    auto&     elem{arr ? arr->underlying : vec->underlying};
    if (*small < 0 || *small >= static_cast<i128>(len)) {
        return comp_.report(u_, src, gs::error::COMPTIME_EVALUATION_FAILED, "index out of bounds");
    }

    auto inner{p};
    inner.path.emplace_back(comptime_step{static_cast<u32>(*small), &elem});
    return operand{std::move(inner)};
}

auto analyzer::comptime_field_place(const comptime_place& p,
                                    std::string_view      name,
                                    const source&         src) -> result<operand> {
    auto& container{type_of(operand{p})};
    if (const auto ut{container.get_data().as_opt<gs::types::union_t>()}) {
        const auto index{TRY(field_index(container, name, src))};
        auto       inner{p};
        inner.path.emplace_back(comptime_step{static_cast<u32>(index), &ut->type_at(index)});
        return operand{std::move(inner)};
    }

    const auto st{container.get_data().as_opt<gs::types::struct_t>()};
    if (!st) { return comp_.unsupported(u_, src, "a field of comptime memory"); }
    const auto index{TRY(field_index(container, name, src))};
    const auto field{st->fields[index]};
    if (!field) { return comp_.unsupported(u_, src, "a field of comptime memory"); }

    auto inner{p};
    inner.path.emplace_back(comptime_step{static_cast<u32>(index), &*field});
    return operand{std::move(inner)};
}

auto analyzer::analyze_let_value(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    rec.skip_str();
    const auto type_ref{written_type_of(rec.ref_field())};
    const auto init_ref{*rec.ref_field()};
    const auto alignment{TRY(local_alignment(rec.body_field()))};
    TRY(reject_unlabeled_loop(init_ref, src));
    auto init{TRY(resolve_value(init_ref, src))};

    // A `let` of `&f` names the function, like a `let` of `f`
    if (const auto init_inst{as_inst(init_ref)};
        init_inst && mod().tag_of(*init_inst) == tag::ref_of && init.is<gir::value>()) {
        record_reader ref_rec{record(*init_inst)};
        const auto    target{TRY(resolve(*ref_rec.ref_field(), src))};
        if (target.is<function_ref>()) { init = target; }
    }

    // An immutable `let` of a known address's number holds that number
    if (const auto init_inst{as_inst(init_ref)};
        !is_comptime() && init_inst && mod().tag_of(*init_inst) == tag::builtin &&
        static_cast<syntax::builtin_t>(record(*init_inst).u32_field()) ==
            syntax::builtin_t::INT_FROM_PTR) {
        const auto before{comp_.speculate()};
        const auto folded{[&] {
            const auto scope{comptime_scope()};
            return analyze_inst(*init_inst);
        }()};
        const auto known{folded ? comptime_value(*folded) : stdx::none};
        if (known) {
            init = operand{*known};
        } else {
            comp_.abandon(before, false);
        }
    }
    // An immutable binding of a function names it, so calls through it stay direct
    if (const auto fn{init.as_opt<function_ref>()}) {
        if (!type_ref || names_function(*fn, *TRY(resolve_type(*type_ref, src)))) { return init; }
    }

    if (const auto index{comptime_value(init)}; index && comp_.constants().is_type(*index)) {
        return comp_.report(
            u_, src, gs::error::MUTABLE_TYPE_BINDING, std::string{messages::MUTABLE_TYPE_BINDING});
    }

    // A range with no type takes its bounds' type
    if (const auto literal{init.as_opt<anon_init>()};
        !type_ref && literal && mod().tag_of(literal->inst) == tag::range) {
        init = TRY(concrete(init, src));
    }

    if (!type_ref && (init.is<anon_init>() || init.is<enum_literal>())) {
        return comp_.unsupported(u_, src, "an untyped literal bound with no type");
    }

    auto                    value{init};
    stdx::option<gs::type&> declared;
    if (type_ref) {
        declared.emplace(*TRY(resolve_type(*type_ref, src)));
        value = TRY(coerce_to(
            init,
            *declared,
            src,
            {.what = use_site::kind::STORE, .from = init_ref, .computes_in_target = true}));
    } else if (init.is<cast_ref>()) {
        // An untyped binding gives a one-argument cast no target to take
        value = TRY(concrete(init, src));
    }

    // An array of mutable elements lives in comptime memory, which slices of it may write
    if (is_comptime() && !gir_fn_) {
        auto& type{type_of(value)};
        if (type.get_kind() == gs::type_kind::ARRAY && !type.is_constant()) {
            return comp_.new_alloc({
                .type                = type,
                .value               = TRY(require_comptime(value, src)),
                .frame               = frame_,
                .runtime_depth       = runtime_depth_,
                .keeps_comptime_type = false,
            });
        }
    }

    if (is_comptime()) { return value; }
    if (gs::is_vector(type_of(value))) {
        TRY(require_runtime(src));

        // A known vector is built where it is bound, as the old emitter does
        const auto known{comptime_value(value)};
        if (known && !alignment &&
            !comp_.constants().value_at(*known).data.is<ip::undefined_value>()) {
            return bind_known_vector(*known, src);
        }
    }

    // A reference binding keeps one address for every use
    auto& type{declared && gs::is_structural(declared->get_kind()) ? *declared : type_of(value)};
    const bool place_reference{[&] {
        const auto init_inst{as_inst(init_ref)};
        if (!init_inst || mod().tag_of(*init_inst) != tag::ref_of ||
            type.get_kind() != gs::type_kind::REFERENCE) {
            return false;
        }

        // Only `&mut x` of a scalar `let mut` folds to an address the old emitter stores
        auto       ref_rec{record(*init_inst)};
        const auto target{as_inst(*ref_rec.ref_field())};
        const auto bits{ref_rec.u32_field()};
        const auto pointee{gs::pointee_of(type)};
        return (bits & flags::MUT) != 0 && (bits & flags::POLY) == 0 && target &&
               mod().tag_of(*target) == tag::alloc_mut && pointee &&
               (gs::is_numeric(pointee->get_kind()) || pointee->get_kind() == gs::type_kind::BOOL ||
                pointee->get_kind() == gs::type_kind::ENUM);
    }()};

    // An aggregate binding keeps one address for every use as does an aligned one
    if (!alignment &&
        ((!gs::is_structural(type.get_kind()) && !type.is_generic_value() && !place_reference) ||
         value.is<merge_ref>())) {
        return value;
    }

    // An `undefined` initializer leaves the fresh slot as it is
    const auto slot{builder_.emit_alloca(type, {}, false, alignment)};
    const auto init_index{comptime_value(init)};
    if (!init_index || !comp_.constants().value_at(*init_index).data.is<ip::undefined_value>()) {
        builder_.emit_store(slot, TRY(as_runtime(value, src))).is_initializer = true;
    }

    // An immutable binding of a known value reads as that value at compile time
    const bool undefined_init{
        init_index && comp_.constants().value_at(*init_index).data.is<ip::undefined_value>()};
    // A closure's environment is a body's `&mut self`, so its binding is always writable
    return slot_value{.address    = gir::value{slot, type},
                      .type       = &type,
                      .is_mutable = type.get_kind() == gs::type_kind::CLOSURE,
                      .known = undefined_init || alignment ? stdx::none : comptime_value(value),
                      .global_value = init.is<global_const>(),
                      .built = undefined_init || alignment ? stdx::none : built_value(value)};
}

auto analyzer::analyze_const_local(inst_id inst) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    const auto  name{rec.str_field()};
    const auto  type_ref{written_type_of(rec.ref_field())};
    const auto  body{rec.body_field()};
    if (!rec.body_field().empty()) { return comp_.unsupported(u_, src, "a local's attributes"); }
    const auto& init_src{body.empty() ? src : src_of(body.back())};

    // A function literal declares a local function, which a runtime body analyzes like a `let`
    const auto literal{!body.empty() && mod().tag_of(body.back()) == tag::yield
                           ? as_inst(mod().data_of(body.back()).un.operand)
                           : stdx::none};
    if (literal && !type_ref && gir_fn_ && !is_comptime() &&
        (mod().tag_of(*literal) == tag::func || mod().tag_of(*literal) == tag::closure)) {
        const auto out{TRY(analyze_body(body))};
        if (out.yielded) {
            const auto value{out.yielded->as_opt<gir::value>()};
            if (!value || !value->type) { return *out.yielded; }
            auto&      type{*value->type};
            const auto slot{builder_.emit_alloca(type)};
            builder_.emit_store(slot, *value).is_initializer = true;
            return slot_value{.address = gir::value{slot, type}, .type = &type, .is_mutable = true};
        }
    }

    auto value{[&] {
        const auto scope{comptime_scope()};
        return analyze_body(body);
    }()};
    if (!value && value.error() == stop::RUNTIME) {
        return comp_.report_runtime_need(
            u_, init_src, gs::error::COMPTIME_MUT_NOT_FOLDABLE, std::string{CONST_CONSTANT});
    }

    const auto out{TRY(std::move(value))};
    if (jump_) { return comp_.unsupported(u_, src, "a jump out of a `const` initializer"); }
    if (!out.yielded) { return comp_.unsupported(u_, src, "a `const` body that doesn't yield"); }

    // A local function is its own name
    auto yielded{*out.yielded};
    if (yielded.is<generic_ref>() && !type_ref) { return yielded; }
    if (const auto fn{yielded.as_opt<function_ref>()}) {
        if (!type_ref || names_function(*fn, *TRY(resolve_type(*type_ref, src)))) {
            return yielded;
        }

        return comp_.unsupported(u_, src, "a local function of another type");
    }

    // An untyped literal takes the declared type before it can be known
    if (type_ref && is_untyped_literal(yielded)) {
        auto&      type{*TRY(resolve_type(*type_ref, src))};
        const auto scope{comptime_scope()};
        auto       typed{coerce_to(yielded, type, init_src, {.what = use_site::kind::STORE})};
        if (!typed && typed.error() == stop::RUNTIME) {
            return comp_.report_runtime_need(
                u_, init_src, gs::error::COMPTIME_MUT_NOT_FOLDABLE, std::string{CONST_CONSTANT});
        }

        yielded = TRY(std::move(typed));
    } else if (const auto range{yielded.as_opt<anon_init>()};
               yielded.is<cast_ref>() || (range && mod().tag_of(range->inst) == tag::range)) {
        const auto scope{comptime_scope()};
        yielded = TRY(concrete(yielded, init_src));
    }

    // An untyped `.name` stays a literal, typed by where it is used
    if (!type_ref && yielded.is<enum_literal>()) { return yielded; }

    const auto known{comptime_value(yielded)};
    if (!known) {
        return comp_.report(
            u_, init_src, gs::error::COMPTIME_MUT_NOT_FOLDABLE, std::string{CONST_CONSTANT});
    }

    // A type declared here is named for the local, `Local` rather than `struct`
    if (comp_.constants().is_type(*known) && !body.empty() &&
        mod().tag_of(body.back()) == tag::yield) {
        auto&      type{comp_.constants().type_at(*known)};
        const auto n{comp_.nominal_of(type)};
        if (n && as_inst(mod().data_of(body.back()).un.operand) == n->inst) {
            comp_.name_nominal(type, name);
            TRY(comp_.require_fields(u_, src, type));
        }
    }

    if (type_ref) {
        auto& type{*TRY(resolve_type(*type_ref, src))};
        yielded = TRY(coerce_to(yielded, type, init_src, {.what = use_site::kind::STORE}));
    }

    // A known vector in a runtime body is built where it is declared
    const auto vector_value{comptime_value(yielded)};
    if (vector_value && gir_fn_ && !is_comptime() && gs::is_vector(type_of(yielded)) &&
        !comp_.constants().value_at(*vector_value).data.is<ip::undefined_value>()) {
        return bind_known_vector(*vector_value, src);
    }

    return yielded;
}

namespace {

[[nodiscard]] auto same_node(const source& a, const source& b) -> bool {
    const auto x{a.as_opt<ast::node_id>()};
    const auto y{b.as_opt<ast::node_id>()};
    return x && y && x->get_index() == y->get_index();
}

} // namespace

auto analyzer::analyze_store(inst_id inst) -> result<operand> {
    const auto& d{mod().data_of(inst)};
    const auto& src{src_of(inst)};

    // `@field(x, name) = v` lowers to a read today, so it names no place to store into
    if (const auto lhs{as_inst(d.bin.lhs)}; lhs && mod().tag_of(*lhs) == tag::builtin) {
        auto rec{record(*lhs)};
        if (static_cast<syntax::builtin_t>(rec.u32_field()) == syntax::builtin_t::FIELD) {
            return comp_.unsupported(u_, src, "an assignment through `@field`");
        }
    }

    const auto target{TRY(resolve(d.bin.lhs, src))};

    // `a[i..j] = v` moves the source's elements into the range, whose length matches statically
    if (const auto lhs{as_inst(d.bin.lhs)}; lhs && mod().tag_of(*lhs) == tag::mem_view) {
        if (is_comptime()) {
            const auto copied{TRY(resolve_value(d.bin.rhs, src))};
            TRY(comptime_range_copy(target, copied, "a range copy", src));
            return void_value();
        }

        const auto found{mem_views_.find(*lhs)};
        if (found == mem_views_.end()) {
            return comp_.unsupported(u_, src, "a copy into a slice range");
        }

        const auto dest{found->second};
        const auto given{TRY(resolve_value(d.bin.rhs, src))};
        const auto read_from{[&] -> result<gir::value> {
            // An untyped `.{...}` is an array of the range's elements, spilled to be read from
            const auto init{given.as_opt<anon_init>()};
            if (!init || mod().tag_of(init->inst) != tag::init_array || !dest.elem) {
                const auto view{TRY(buffer_view(d.bin.rhs, given, false, src))};
                return view.data;
            }

            auto& ctx{comp_.ctx()};
            auto  items{record(init->inst)};
            items.skip_ref();
            auto&      array{ctx.get_array(
                gs::types::mut::CONSTANT, false, items.refs_field().size(), *dest.elem)};
            const auto typed{TRY(coerce_to(given, array, src, {.what = use_site::kind::STORE}))};
            const auto slot{spill(TRY(as_runtime(typed, src)), array, false).address};
            auto&      byte_ptr{ctx.get_pointer(gs::types::mut::MUTABLE, ctx.get_int(8, false))};
            return gir::value{builder_.emit_get_element_ptr(slot, {usize_constant(0)}, byte_ptr),
                              byte_ptr};
        }()};
        const auto from{TRY(read_from)};
        DISCARD(builder_.emit_builtin_call(syntax::builtin_name(syntax::builtin_t::MEMMOVE),
                                           {dest.data, from, *dest.bytes},
                                           builtin(gs::type_kind::VOID_)));
        return void_value();
    }

    const auto value{TRY(resolve_value(d.bin.rhs, src))};

    if (const auto comptime_slot{comptime_destination(target)}) {
        // A runtime function's store into comptime memory runs its value at compile time
        auto known{[&] -> result<operand> {
            if (comptime_value(value)) { return value; }
            const auto scope{comptime_scope()};
            return reevaluate(d.bin.rhs, src);
        }()};
        if (!known && known.error() == stop::RUNTIME) {
            return comp_.report_runtime_need(u_,
                                             src,
                                             gs::error::COMPTIME_MUT_ASSIGN_NOT_FOLDABLE,
                                             std::string{ASSIGN_NOT_FOLDABLE});
        }

        if (!known) { return stdx::err{known.error()}; }
        TRY(store_comptime(*comptime_slot, *known, src));
        return void_value();
    }

    TRY(require_runtime(src));

    // The reference a compound assignment's operator already read its left side through
    const auto compound_read{as_inst(d.bin.rhs).and_then([&](inst_id rhs) {
        const auto known{reference_reads_.find(rhs)};
        if (known == reference_reads_.end()) { return stdx::option<gir::value>{}; }

        // `a += b` lowers its operator at the assignment
        const auto operand_ref{mod().data_of(rhs).bin.lhs};
        const auto operand_inst{as_inst(operand_ref)};
        const bool reads_target{operand_ref == d.bin.lhs ||
                                (operand_inst && mod().tag_of(*operand_inst) == tag::load &&
                                 mod().data_of(*operand_inst).un.operand == d.bin.lhs)};
        const bool compound{reads_target && same_node(src_of(rhs), src)};
        return compound ? stdx::option<gir::value>{known->second} : stdx::option<gir::value>{};
    })};

    // A reference binding written through is spilled to a slot first
    auto through{target};
    if (const auto lhs{as_inst(d.bin.lhs)};
        lhs && is_binding(mod().tag_of(*lhs)) && !target.is<place>() &&
        type_of(target).get_kind() == gs::type_kind::REFERENCE) {
        through = compound_read ? *compound_read : load(TRY(address_of(d.bin.lhs, src)));
    }

    auto destination{TRY(store_destination(through, src))};

    // A `let mut` holding a reference has value semantics
    const auto lhs_inst{as_inst(d.bin.lhs)};
    const bool initializes{lhs_inst && mod().tag_of(*lhs_inst) == tag::alloc_mut &&
                           same_node(src_of(*lhs_inst), src)};
    if (!initializes && destination.address.type && !destination.packed && !destination.lane &&
        destination.pointee->get_kind() == gs::type_kind::REFERENCE) {
        const auto held{compound_read ? *compound_read : load(destination)};
        destination = TRY(pointer_place(operand{held}, src));
    }

    if (!destination.is_mutable) {
        return comp_.report(u_, src, gs::error::ASSIGNMENT_TO_CONST, std::string{ASSIGN_CONSTANT});
    }

    if (!destination.address.type) {
        // A type only exists at compile time, so a runtime slot can't hold one
        if (const auto index{comptime_value(value)};
            index && comp_.constants().is_type(*index)) {
            return comp_.report(u_,
                                src,
                                gs::error::MUTABLE_TYPE_BINDING,
                                std::string{messages::MUTABLE_TYPE_BINDING});
        }

        // An untyped `let mut` takes its initializer's concrete type, never volatile
        const auto typed{TRY(concrete(value, src))};
        auto&      type{*comp_.ctx().pool.strip_volatile(type_of(typed))};
        destination.address = gir::value{builder_.emit_alloca(type), type};
        destination.pointee = &type;
        values_.insert_or_assign(*as_inst(d.bin.lhs), operand{destination});
    }

    const auto stored{
        TRY(coerce_to(value, *destination.pointee, src, {.what = use_site::kind::STORE}))};

    // Storing `undefined` leaves the slot as it is, so nothing is emitted
    const auto known{comptime_value(value)};
    if (!known || !comp_.constants().value_at(*known).data.is<ip::undefined_value>()) {
        const auto runtime{TRY(as_runtime(stored, src))};
        if (destination.packed) {
            packed_store(destination, runtime);
        } else if (const auto& lane{destination.lane}) {
            auto&            vec_type{*lane->vector.type};
            const gir::value updated{builder_.emit_vector_op(gir::instruction_kind::INSERT_LANE,
                                                             {lane->vector, lane->index, runtime},
                                                             vec_type),
                                     vec_type};
            builder_.emit_store(destination.address, updated);
        } else {
            builder_.emit_store(destination.address, runtime);
        }
    }

    return void_value();
}

auto analyzer::store_destination(const operand& target, const source& src) -> result<place> {
    if (const auto slot{target.as_opt<place>()}) { return slot->pending ? settle(*slot) : *slot; }
    if (const auto slot{target.as_opt<slot_value>()}; slot && slot->is_mutable) {
        return place{.address = slot->address, .pointee = slot->type, .is_mutable = true};
    }

    // `*p = v` stores through the pointer or reference `p`
    if (gs::pointee_of(type_of(target)) && !target.is<comptime_place>()) {
        return pointer_place(target, src);
    }

    return place{.address = {}, .pointee = &builtin(gs::type_kind::VOID_), .is_mutable = false};
}

auto analyzer::analyze_load(inst_id inst) -> result<operand> {
    const auto& src{src_of(inst)};
    const auto  target{TRY(resolve(mod().data_of(inst).un.operand, src))};
    if (const auto comptime_slot{target.as_opt<comptime_place>()}) {
        return load_comptime(*comptime_slot);
    }

    if (const auto slot{target.as_opt<slot_value>()}) {
        TRY(require_runtime(src));
        return operand{
            load(place{.address = slot->address, .pointee = slot->type, .is_mutable = false})};
    }

    // A field of a compile-time value was already read, and a module's member is itself
    if (comptime_value(target) || target.is<function_ref>() || target.is<generic_ref>() ||
        target.is<module_ref>()) {
        return target;
    }

    // An array's `.ptr` is already the value read
    if (const auto from{as_inst(mod().data_of(inst).un.operand)};
        from && mod().tag_of(*from) == tag::field_ptr && target.is<gir::value>()) {
        return target;
    }
    // A mutable global is read only at runtime
    if (is_comptime() && target.is<global_ref>()) { return comp_.needs_runtime(u_, src); }

    const auto slot{target.as_opt<place>()};
    if (!slot || (!slot->pending && !slot->address.type)) {
        return comp_.unsupported(u_, src, "a load from a non-place");
    }

    TRY(require_runtime(src));
    return operand{TRY(read(*slot, src))};
}

auto analyzer::analyze_decl_val(inst_id inst, bool as_place) -> result<operand> {
    const auto&   src{src_of(inst)};
    const decl_id decl{mod().data_of(inst).un.aux};
    const auto&   d{mod().decl_at(decl)};
    if (d.kind != decl_kind::VALUE && d.kind != decl_kind::IMPORT) {
        return comp_.unsupported(u_, src, "a use of a non-value declaration");
    }

    // A sibling member of a ctor instance's type is that instance's
    const bool in_scope{scope_ && d.owner == scope_->inst};
    const auto value{TRY(in_scope ? comp_.analyze_member(*scope_, decl, src)
                                  : comp_.analyze_decl(u_, decl, src))};
    warn_deprecated_decl(u_, decl, src);
    if (value.is<function_ref>() || value.is<generic_ref>()) {
        if (auto name{testing_name(u_, decl)}) {
            testing_refs_.insert_or_assign(inst, std::move(*name));
        }
    }

    return declaration_use(value, as_place, src);
}

auto analyzer::declaration_use(const operand& value, bool as_place, const source& src)
    -> result<operand> {
    const auto global{value.as_opt<global_ref>()};
    if (!global) {
        // A module-level `comptime let mut` is a place already
        if (as_place && !value.is<comptime_place>()) {
            return comp_.unsupported(u_, src, "a place on a non-`let mut` declaration");
        }

        return value;
    }

    // `@TypeOf` asks only the global's type
    if (only_types_ && !as_place) { return operand{gir::value{gir::local_id{}, *global->type}}; }

    if (is_comptime()) {
        // A runtime body folding a condition keeps a `let mut` global's read at runtime
        // as does any read of a volatile one
        const bool is_volatile{global->type->is_volatile()};
        if ((gir_fn_ || is_volatile) && !as_place) { return comp_.needs_runtime(u_, src); }

        // A global's initializer may take another global's address
        if (!gir_fn_ && as_place) { return value; }
        return comp_.report(u_,
                            src,
                            gs::error::COMPTIME_EVALUATION_FAILED,
                            "A `let mut` global changes at runtime, so it isn't known at compile "
                            "time");
    }

    auto&       type{*global->type};
    const auto  address{builder_.emit_global_addr(std::string{global->symbol}, type, false)};
    const place slot{.address = gir::value{address, type}, .pointee = &type, .is_mutable = true};
    if (as_place) { return slot; }

    // A tagged union is read where it is used, so its tag is checked in place
    if (const auto ut{type.get_data().as_opt<gs::types::union_t>()}; ut && !ut->is_untagged) {
        return slot_value{.address = slot.address, .type = &type, .is_mutable = true};
    }

    return gir::value{builder_.emit_load(address, type), type};
}

auto analyzer::analyze_discard(inst_id inst) -> result<operand> {
    const auto& src{src_of(inst)};
    const auto  value{TRY(resolve_value(mod().data_of(inst).un.operand, src))};
    if (value.is<merge_ref>() || value.is<cast_ref>()) { DISCARD(TRY(concrete(value, src))); }
    return void_value();
}

} // namespace ghoti::uir::sema
