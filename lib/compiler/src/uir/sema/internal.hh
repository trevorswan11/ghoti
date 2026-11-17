#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>
#include <stdx/variant.hh>

#include "compiler/gir/builder.hh"
#include "compiler/gir/function.hh"
#include "compiler/gir/instruction.hh"
#include "compiler/gir/module.hh"
#include "compiler/gir/semantics.hh"
#include "compiler/ip/index.hh"
#include "compiler/ip/pool.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/context.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/sema/sema.hh"
#include "compiler/uir/uir.hh"

namespace ghoti::uir::sema {

namespace gs = ::ghoti::sema;

// Why an analysis step stopped: a reported error, or a construct this phase doesn't handle
enum class stop : u8 {
    POISON,
    UNSUPPORTED,
};

template <typename T> using result = stdx::result<T, stop>;

// One lowered module and the source it came from
struct unit {
    gsl::not_null<const mod::module_t*> source;
    gsl::not_null<const module_t*>      uir;
    u32                                 index;
};

struct instance;

// A function declaration used as a value
struct function_ref {
    gsl::not_null<instance*> fn;
};

// An addressable slot that loads read and stores write
struct place {
    gir::value               address;
    gsl::not_null<gs::type*> pointee;
    bool                     is_mutable;
};

// A runtime merge whose arms are all untyped constants, given a type where it is used
struct merge_ref {
    u32 index;
};

// A module-level `let mut`, which each use reaches through its GIR global's address
struct global_ref {
    gsl::not_null<const std::string*> symbol;
    gsl::not_null<gs::type*>          type;
};

// A comptime value, a runtime value, a function, a place, a pending untyped merge, or a global
using operand = stdx::variant<ip::index, gir::value, function_ref, place, merge_ref, global_ref>;

enum class decl_status : u8 {
    UNRESOLVED,
    IN_PROGRESS,
    RESOLVED,
    FAILED,
};

struct decl_state {
    decl_status                          status{decl_status::UNRESOLVED};
    stdx::option<operand>                value{};
    stdx::option<stdx::box<std::string>> global_symbol{};
};

// A non-generic function's signature, and the GIR function its body is analyzed into
struct instance {
    gsl::not_null<const unit*>            owner;
    decl_id                               decl;
    inst_id                               func;
    std::string                           symbol;
    gsl::not_null<gs::type*>              type;
    std::vector<gsl::not_null<gs::type*>> params;
    gsl::not_null<gs::type*>              return_type;
    gir::linkage                          linkage{gir::linkage::INTERNAL};
    bool                                  queued{false};
};

class compilation {
  public:
    compilation(gs::context& ctx, const program& prog, const options& opts);
    ~compilation() = default;
    MAKE_PINNED(compilation);

    [[nodiscard]] auto run() -> outcome;

    [[nodiscard]] auto ctx() noexcept -> gs::context& { return ctx_; }
    [[nodiscard]] auto constants() noexcept -> ip::pool& { return ctx_.pool.intern(); }
    [[nodiscard]] auto gir() noexcept -> gir::module_t& { return gir_; }
    [[nodiscard]] auto ptr_bits() const noexcept -> u32 { return ptr_bits_; }

    // The declaration's value, analyzing it on first use. A use during its own analysis is a
    // cycle, reported at `use`
    [[nodiscard]] auto analyze_decl(const unit& u, decl_id decl, const source& use)
        -> result<operand>;
    // Puts the function's body on the work queue, once
    auto request_body(instance& fn) -> void;
    // Declares the function `decl`'s `func` value creates, leaving its body for the queue
    [[nodiscard]] auto declare_function(const unit& u, decl_id decl, inst_id func)
        -> result<operand>;

    // Reports a diagnostic at `src` and returns the poison stop
    [[nodiscard]] auto report(const unit& u, const source& src, gs::error code, std::string message)
        -> stdx::err<stop>;
    // Records the first construct this phase doesn't analyze and returns the unsupported stop
    [[nodiscard]] auto unsupported(const unit& u, const source& src, std::string what)
        -> stdx::err<stop>;

    [[nodiscard]] auto location_of(const unit& u, const source& src) const -> source_location;
    [[nodiscard]] auto span_of(const unit& u, const source& src) const -> source_span;

    // The GIR symbol and linkage of a module-level declaration
    [[nodiscard]] auto        symbol_for(const unit& u, const decl& d) const -> std::string;
    [[nodiscard]] static auto linkage_of(const decl& d) noexcept -> gir::linkage;
    // Keeps a `let mut` global's symbol alive for the `global_ref`s that name it
    [[nodiscard]] auto own_symbol(const unit& u, decl_id decl, std::string symbol)
        -> const std::string&;

  private:
    auto               analyze_roots() -> void;
    auto               drain_queue() -> void;
    [[nodiscard]] auto analyze_body(instance& fn) -> result<void>;

  private:
    gs::context&                          ctx_;
    const program&                        prog_;
    options                               opts_;
    std::vector<unit>                     units_;
    std::vector<std::vector<decl_state>>  decls_;
    std::vector<stdx::box<instance>>      instances_;
    std::vector<gsl::not_null<instance*>> queue_;
    gir::module_t                         gir_;
    gs::diagnostics                       diagnostics_;
    stdx::option<sema::unsupported>       stopped_at_;
    u32                                   ptr_bits_;
};

// The result of analyzing a body: the value it yields, or none when control left it
struct flow {
    stdx::option<operand> yielded;
};

enum class op_class : u8 {
    ARITHMETIC,
    BITWISE,
    SHIFT,
    EQUALITY,
    ORDERING,
};

// What a binary operator tag lowers to
struct binary_info {
    gir::instruction_kind kind;
    op_class              cls;
    bool                  wrapping{false};
    bool                  saturating{false};
};

// A break or a fallthrough reaching a merge: where it left from and what it carried
struct incoming {
    gir::segment_id seg;
    operand         value;
    source          src;
};

// Analyzes one body: a function's runtime body, or a comptime body such as a declaration's value
class analyzer {
  public:
    // A comptime analyzer, with no GIR function. `declaring` names the declaration whose value
    // it computes, which a `func` there declares
    analyzer(compilation&          comp,
             const unit&           u,
             stdx::option<decl_id> declaring = stdx::none) noexcept;
    // A runtime analyzer for `fn`'s body
    analyzer(compilation& comp, const unit& u, instance& fn, gir::function& gir_fn) noexcept;
    ~analyzer() = default;
    MAKE_PINNED(analyzer);

    // A comptime body's yielded value
    [[nodiscard]] auto eval_comptime(gsl::span<const inst_id> body) -> result<operand>;
    // A type body's denoted type
    [[nodiscard]] auto eval_type(gsl::span<const inst_id> body, const source& src)
        -> result<gsl::not_null<gs::type*>>;
    // Analyzes the function's parameters and body into its GIR function
    [[nodiscard]] auto analyze_function() -> result<void>;
    // A module-level declaration's value, emitting its GIR global when it has one
    [[nodiscard]] auto analyze_decl(decl_id decl) -> result<operand>;

    // The comptime value of `op`, none when it is only known at runtime
    [[nodiscard]] auto comptime_value(const operand& op) const -> stdx::option<ip::index>;
    [[nodiscard]] auto type_of(const operand& op) -> gs::type&;
    [[nodiscard]] auto denoted_type(const operand& op, const source& src)
        -> result<gsl::not_null<gs::type*>>;

  private:
    // A break or repeat destination, and what reaches its merge
    struct target {
        inst_id                       inst;
        stdx::option<gir::segment_id> exit;
        stdx::option<gir::segment_id> repeat;
        std::vector<incoming>         arrivals;
    };

    // Arms that all carry untyped constants, waiting for a use to type them
    struct pending_merge {
        gir::segment_id          seg;
        std::vector<incoming>    arrivals;
        stdx::option<gir::value> materialized;
    };

    // Where a value is converted to a known type, which names the mismatch when it can't be
    struct use_site {
        enum class kind : u8 {
            STORE,
            RETURN,
            ARGUMENT,
        };
        kind             what;
        usize            arg_index{0};
        std::string_view callee{};
    };

    // Instructions
    [[nodiscard]] auto analyze_body(gsl::span<const inst_id> body) -> result<flow>;
    [[nodiscard]] auto analyze_inst(inst_id inst) -> result<operand>;
    [[nodiscard]] auto resolve(ref r, const source& src) -> result<operand>;
    [[nodiscard]] auto resolve_value(ref r, const source& src) -> result<operand>;
    // The type a type operand denotes
    [[nodiscard]] auto resolve_type(ref r, const source& src) -> result<gsl::not_null<gs::type*>>;
    [[nodiscard]] auto well_known_operand(well_known w, const source& src) -> result<operand>;

    // Literals and types (types.cc)
    [[nodiscard]] auto analyze_int_value(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_int_lit(inst_id inst) -> result<operand>;
    [[nodiscard]] auto
    negated_to_fit(inst_id inst, const big_int& value, const gs::type& type) const -> bool;
    [[nodiscard]] auto analyze_float_lit(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_int_type(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_func(inst_id inst) -> result<operand>;

    // Operators (arith.cc)
    [[nodiscard]] auto analyze_binary(inst_id inst, tag t) -> result<operand>;
    [[nodiscard]] auto analyze_unary(inst_id inst, tag t) -> result<operand>;
    [[nodiscard]] auto analyze_bool_op(inst_id inst, bool is_and) -> result<operand>;
    [[nodiscard]] auto
    analyze_shift(inst_id inst, const binary_info& info, const operand& lhs, const operand& rhs)
        -> result<operand>;
    // `lhs op rhs` when both are known and the operation doesn't fault, else none
    [[nodiscard]] auto fold_binary(const binary_info& info,
                                   gs::type&          peer,
                                   const operand&     lhs,
                                   const operand&     rhs,
                                   const source&      src) -> result<stdx::option<operand>>;
    [[nodiscard]] auto fold_float(const binary_info& info, gs::type& peer, f128 lhs, f128 rhs)
        -> stdx::option<operand>;
    [[nodiscard]] auto domain_of(const gs::type& t) const -> gir::semantics::int_domain;
    // Whether the runtime instruction checks `op` on `t`, as the old emitter decides it
    [[nodiscard]] auto checks_overflow(gir::semantics::int_op op, const gs::type& t) const -> bool;

    // Coercion (coerce.cc)
    // `op` as a `target`, none when no implicit conversion exists. A constant that doesn't fit is
    // reported here
    [[nodiscard]] auto coerce(const operand& op, gs::type& target, const source& src)
        -> result<stdx::option<operand>>;
    // `coerce`, reporting a missing conversion in the words of `site`
    [[nodiscard]] auto
    coerce_to(const operand& op, gs::type& target, const source& src, const use_site& site)
        -> result<operand>;
    [[nodiscard]] auto peer_type(gsl::span<const operand> ops, const source& src)
        -> result<gsl::not_null<gs::type*>>;
    [[nodiscard]] auto as_runtime(const operand& op, const source& src) -> result<gir::value>;
    [[nodiscard]] auto materialize_merge(merge_ref m, gs::type& type, const source& src)
        -> result<gir::value>;
    [[nodiscard]] auto concrete(const operand& op, const source& src) -> result<operand>;

    // Locals and memory (memory.cc)
    [[nodiscard]] auto analyze_alloc_mut(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_let_value(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_const_local(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_store(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_load(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_decl_val(inst_id inst, bool as_place) -> result<operand>;
    [[nodiscard]] auto analyze_discard(inst_id inst) -> result<operand>;

    // Control flow (control.cc)
    [[nodiscard]] auto analyze_block(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_loop(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_condbr(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_break(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_repeat(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_ret(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_unreachable(inst_id inst) -> result<operand>;
    // The value every arrival reaches `merge` with, leaving the builder there
    [[nodiscard]] auto finish_merge(const std::vector<incoming>& arrivals,
                                    gir::segment_id              merge,
                                    const source&                src) -> result<operand>;
    [[nodiscard]] auto target_of(ref r) -> target&;
    auto               arrive(target& t, const operand& value, const source& src) -> void;
    // `op` as a branch condition, which must be a `bool`
    [[nodiscard]] auto condition(const operand& op, const source& src) -> result<gir::value>;
    [[nodiscard]] auto void_value() -> operand;

    // Calls (calls.cc)
    [[nodiscard]] auto analyze_call(inst_id inst) -> result<operand>;

    // Declarations (decls.cc)
    [[nodiscard]] auto analyze_global(decl_id                 decl,
                                      const operand&          value,
                                      stdx::option<gs::type&> declared,
                                      const source&           src) -> result<operand>;

    // GIR glue (emit.cc)
    [[nodiscard]] auto is_comptime() const noexcept -> bool {
        return !gir_fn_ || comptime_depth_ > 0;
    }
    // Fails in a comptime body, where `what` can't be emitted
    [[nodiscard]] auto require_runtime(const source& src) -> result<void>;
    [[nodiscard]] auto constant(ip::index value, gs::type& type) -> gir::value;
    [[nodiscard]] auto builder() -> gir::builder& { return builder_; }
    [[nodiscard]] auto terminated() const -> bool;
    auto               switch_to(gir::segment_id seg) -> void;
    [[nodiscard]] auto new_segment() -> gir::segment_id;
    auto               emit_goto(gir::segment_id seg) -> void;
    auto               set_location(const source& src) -> void;

    [[nodiscard]] auto src_of(inst_id inst) const -> const source& { return mod().src_of(inst); }
    [[nodiscard]] auto mod() const -> const module_t& { return *u_.uir; }
    [[nodiscard]] auto record(inst_id inst) const -> record_reader {
        return record_reader{mod(), mod().data_of(inst).pl.extra};
    }
    [[nodiscard]] auto interned_type(gs::type& t) -> ip::index;
    [[nodiscard]] auto builtin(gs::type_kind kind) -> gs::type& {
        return comp_.ctx().get_builtin_resolved_type(kind);
    }

  private:
    compilation&                                   comp_;
    const unit&                                    u_;
    stdx::option<decl_id>                          declaring_;
    stdx::option<instance&>                        fn_;
    stdx::option<gir::function&>                   gir_fn_;
    gir::builder                                   builder_;
    ankerl::unordered_dense::map<inst_id, operand> values_;
    std::vector<target>                            targets_;
    std::vector<pending_merge>                     merges_;
    // Nonzero inside a comptime body of a runtime function, where nothing may emit GIR
    usize comptime_depth_{0};
    // Set when a comptime body stopped because it needed a runtime value
    bool needed_runtime_{false};
};

// Messages shared with the old pipeline's checks (messages.cc)
namespace messages {

[[nodiscard]] auto
store_mismatch(const gs::context& ctx, const gs::type& from, const gs::type& to, u32 ptr_bits)
    -> std::string;
[[nodiscard]] auto
return_mismatch(const gs::context& ctx, const gs::type& from, const gs::type& to, u32 ptr_bits)
    -> std::string;
[[nodiscard]] auto argument_mismatch(const gs::context& ctx,
                                     usize              index,
                                     const gs::type&    from,
                                     const gs::type&    to,
                                     std::string_view   callee,
                                     u32                ptr_bits) -> std::string;
[[nodiscard]] auto operator_mismatch(const gs::context& ctx,
                                     std::string_view   op,
                                     const gs::type&    lhs,
                                     const gs::type&    rhs) -> std::string;
[[nodiscard]] auto literal_out_of_range(const gs::context& ctx, const gs::type& to) -> std::string;
[[nodiscard]] auto
value_out_of_range(const gs::context& ctx, const big_int& value, const gs::type& to) -> std::string;
[[nodiscard]] auto condition_not_bool(const gs::context& ctx, const gs::type& found) -> std::string;

} // namespace messages

// The spelling of an operator tag in diagnostics, `+` for `add`
[[nodiscard]] auto operator_spelling(tag t) noexcept -> std::string_view;

// `value` as a `type` constant, at the narrowest integer width that holds it
[[nodiscard]] auto intern_int(ip::pool& pool, const big_int& value, gs::type& type) -> ip::index;
// An integer constant's value, none for any other kind of constant
[[nodiscard]] auto int_of(const ip::value& v) -> stdx::option<big_int>;

} // namespace ghoti::uir::sema
