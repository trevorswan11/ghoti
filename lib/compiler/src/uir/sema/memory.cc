#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gsl/span>
#include <gsl/util>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/utility.hh>

#include "compiler/gir/instruction.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/syntax/builtins.hh"
#include "compiler/uir/uir.hh"
#include "internal.hh"
#include "support/big_int.hh"

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

auto analyzer::analyze_alloc_mut(inst_id inst, bool is_comptime_alloc) -> result<operand> {
    auto        rec{record(inst)};
    const auto& src{src_of(inst)};
    rec.skip_str();
    const auto type_ref{written_type_of(rec.ref_field())};
    if (!rec.body_field().empty()) { return comp_.unsupported(u_, src, "a local's attributes"); }
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

    if (!type) {
        // The first store gives an untyped `let mut` its type
        return place{
            .address = {}, .pointee = &builtin(gs::type_kind::UNDEFINED), .is_mutable = true};
    }

    const auto slot{builder_.emit_alloca(*type)};
    return place{.address = gir::value{slot, *type}, .pointee = type.get(), .is_mutable = true};
}

auto analyzer::store_comptime(const comptime_place& p, const operand& value, const source& src)
    -> result<void> {
    auto& alloc{comp_.alloc_at(p)};
    if (alloc.frame != frame_ && alloc.frame != env_frame_) {
        return comp_.unsupported(u_, src, "a store to another analysis's comptime allocation");
    }

    if (alloc.runtime_depth != runtime_depth_) {
        return comp_.report(
            u_, src, gs::error::COMPTIME_MUT_ASSIGN_NOT_FOLDABLE, std::string{STORE_UNDER_RUNTIME});
    }

    // An element or field is stored by rebuilding the value around it
    if (!p.path.empty()) {
        auto&      element_type{*p.path.back().type};
        const auto stored{
            TRY(coerce_to(value, element_type, src, {.what = use_site::kind::STORE}))};
        const auto element{TRY(require_comptime(stored, src))};
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

        auto& type{*alloc.type};
        return store_comptime(
            p, TRY(coerce_to(value, type, src, {.what = use_site::kind::STORE})), src);
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

auto analyzer::load_comptime(const comptime_place& p) -> operand {
    auto& pool{comp_.constants()};
    auto& alloc{comp_.alloc_at(p)};
    auto& type{alloc.type.value_or(builtin(gs::type_kind::UNDEFINED))};
    auto  value{alloc.value.value_or(pool.intern(ip::undefined_value{}, type))};
    for (const auto& step : p.path) {
        const auto& data{pool.value_at(value).data};
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

auto analyzer::with_element(stdx::option<ip::index>        current,
                            gs::type&                      type,
                            gsl::span<const comptime_step> path,
                            ip::index                      value,
                            const source&                  src) -> result<ip::index> {
    if (path.empty()) { return value; }
    auto& pool{comp_.constants()};

    // Each element's type, from the array or struct holding them
    std::vector<stdx::option<gs::type&>> types;
    if (const auto arr{type.get_data().as_opt<gs::types::array>()}) {
        types.assign(arr->len, arr->underlying);
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
    const auto small{at ? int_of(comp_.constants().value_at(*at)).and_then([](const big_int& b) {
        return b.to_i128();
    })
                        : stdx::none};
    if (!arr || !small) { return comp_.unsupported(u_, src, "an element of comptime memory"); }
    if (*small < 0 || *small >= static_cast<i128>(arr->len)) {
        return comp_.report(u_, src, gs::error::COMPTIME_EVALUATION_FAILED, "index out of bounds");
    }

    auto inner{p};
    inner.path.emplace_back(comptime_step{static_cast<u32>(*small), &arr->underlying});
    return operand{std::move(inner)};
}

auto analyzer::comptime_field_place(const comptime_place& p,
                                    std::string_view      name,
                                    const source&         src) -> result<operand> {
    auto&      container{type_of(operand{p})};
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
    if (!rec.body_field().empty()) { return comp_.unsupported(u_, src, "a local's attributes"); }
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
        return comp_.unsupported(u_, src, "a function bound to a `let`");
    }

    if (const auto index{comptime_value(init)}; index && comp_.constants().is_type(*index)) {
        return comp_.report(
            u_, src, gs::error::MUTABLE_TYPE_BINDING, std::string{messages::MUTABLE_TYPE_BINDING});
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

    if (is_comptime()) { return value; }

    if (gs::is_vector(type_of(value))) { TRY(vector_at_runtime(src)); }

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

    // An aggregate binding keeps one address for every use, so it gets a slot of its own
    if ((!gs::is_structural(type.get_kind()) && !type.is_generic_value() && !place_reference) ||
        value.is<merge_ref>()) {
        return value;
    }

    // An `undefined` initializer leaves the fresh slot as it is
    const auto slot{builder_.emit_alloca(type)};
    const auto init_index{comptime_value(init)};
    if (!init_index || !comp_.constants().value_at(*init_index).data.is<ip::undefined_value>()) {
        builder_.emit_store(slot, TRY(as_runtime(value, src))).is_initializer = true;
    }

    // An immutable binding of a known value reads as that value at compile time
    const bool undefined_init{
        init_index && comp_.constants().value_at(*init_index).data.is<ip::undefined_value>()};
    // A closure's environment is a body's `&mut self`, so its binding is always writable
    return slot_value{.address      = gir::value{slot, type},
                      .type         = &type,
                      .is_mutable   = type.get_kind() == gs::type_kind::CLOSURE,
                      .known        = undefined_init ? stdx::none : comptime_value(value),
                      .global_value = init.is<global_const>()};
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
        ++const_inits_;
        const auto done{gsl::finally([&] { --const_inits_; })};
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
    } else if (yielded.is<cast_ref>()) {
        const auto scope{comptime_scope()};
        yielded = TRY(concrete(yielded, init_src));
    }

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

    if (!type_ref) { return yielded; }
    auto& type{*TRY(resolve_type(*type_ref, src))};
    return coerce_to(yielded, type, init_src, {.what = use_site::kind::STORE});
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
        const auto found{mem_views_.find(*lhs)};
        if (found == mem_views_.end()) {
            return comp_.unsupported(u_, src, "a copy into a slice range");
        }

        const auto dest{found->second};
        const auto given{TRY(resolve_value(d.bin.rhs, src))};
        const auto from{TRY(buffer_view(d.bin.rhs, given, false, src))};
        DISCARD(builder_.emit_builtin_call(syntax::builtin_name(syntax::builtin_t::MEMMOVE),
                                           {dest.data, from.data, *dest.bytes},
                                           builtin(gs::type_kind::VOID_)));
        return void_value();
    }

    const auto value{TRY(resolve_value(d.bin.rhs, src))};

    if (const auto comptime_slot{target.as_opt<comptime_place>()}) {
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

    // A reference binding written through is spilled to a slot first
    auto through{target};
    if (const auto lhs{as_inst(d.bin.lhs)};
        lhs && is_binding(mod().tag_of(*lhs)) && !target.is<place>() &&
        type_of(target).get_kind() == gs::type_kind::REFERENCE) {
        const auto read{as_inst(d.bin.rhs).and_then([&](inst_id rhs) {
            const auto known{reference_reads_.find(rhs)};

            // `a += b` lowers its operator at the assignment, `a = a + b` at the `+`
            const bool compound{known != reference_reads_.end() &&
                                mod().data_of(rhs).bin.lhs == d.bin.lhs &&
                                same_node(src_of(rhs), src)};
            return compound ? stdx::option<gir::value>{known->second} : stdx::option<gir::value>{};
        })};
        through = read ? *read : load(TRY(address_of(d.bin.lhs, src)));
    }

    auto destination{TRY(store_destination(through, src))};
    if (!destination.is_mutable) {
        return comp_.report(u_, src, gs::error::ASSIGNMENT_TO_CONST, std::string{ASSIGN_CONSTANT});
    }

    if (!destination.address.type) {
        // An untyped `let mut` takes its initializer's concrete type
        const auto typed{TRY(concrete(value, src))};
        auto&      type{type_of(typed)};
        destination.address = gir::value{builder_.emit_alloca(type), type};
        destination.pointee = &type;
        values_.insert_or_assign(*as_inst(d.bin.lhs), operand{destination});
    }

    const auto stored{
        TRY(coerce_to(value, *destination.pointee, src, {.what = use_site::kind::STORE}))};

    // Storing `undefined` leaves the slot as it is, so nothing is emitted
    const auto known{comptime_value(value)};
    if (!known || !comp_.constants().value_at(*known).data.is<ip::undefined_value>()) {
        builder_.emit_store(destination.address, TRY(as_runtime(stored, src)));
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

    // A field of a compile-time value was already read
    if (comptime_value(target)) { return target; }
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
    return declaration_use(value, as_place, src);
}

auto analyzer::declaration_use(const operand& value, bool as_place, const source& src)
    -> result<operand> {
    const auto global{value.as_opt<global_ref>()};
    if (!global) {
        if (as_place) {
            return comp_.unsupported(u_, src, "a place on a non-`let mut` declaration");
        }

        return value;
    }

    if (is_comptime()) {
        // A runtime body folding a condition keeps a `let mut` global's read at runtime
        if (gir_fn_ && !as_place) { return comp_.needs_runtime(u_, src); }

        // A global's initializer may take another global's address
        if (!gir_fn_ && as_place) { return value; }
        return comp_.unsupported(u_, src, "a compile-time use of a `let mut` global");
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
