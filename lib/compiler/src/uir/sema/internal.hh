#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <gsl/span>
#include <gsl/util>
#include <stdx/memory.hh>
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
#include "compiler/syntax/builtins.hh"
#include "compiler/uir/sema/sema.hh"
#include "compiler/uir/uir.hh"
#include "support/big_int.hh"
#include "support/diagnostic.hh"
#include "support/float128.hh"

namespace ghoti::uir::sema {

namespace gs = ::ghoti::sema;

// Why an analysis step stopped
enum class stop : u8 {
    POISON,      // A reported error
    UNSUPPORTED, // A construct this phase doesn't handle
    RUNTIME,     // A comptime evaluation needed a runtime value, which its boundary reports
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

// A comptime allocation's slot: a `comptime let mut`, or a `let mut` in a comptime body
struct comptime_place {
    u32 alloc;
};

// A comptime value, a runtime value, a function, a place, a pending untyped merge, a global, or a
// comptime allocation
using operand = stdx::
    variant<ip::index, gir::value, function_ref, place, merge_ref, global_ref, comptime_place>;

// Mutable compile-time state, which only stores change (§7)
struct comptime_alloc {
    // None until an untyped allocation's first store gives it one
    stdx::option<gs::type&> type;
    stdx::option<ip::index> value;
    // The analysis that owns it, which alone may store to it
    u32 frame;
    // The runtime branches around its declaration, which a store may not add to (Q6)
    u32  runtime_depth;
    bool keeps_comptime_type;
};

// One compile-time call in progress, for the recursion limit and the notes of its errors
struct comptime_frame {
    gsl::not_null<const unit*> caller;
    source                     call_src;
    std::string_view           callee;
    // Allocations from here on belong to the call, so storing below it makes the call impure
    u32  alloc_base;
    bool impure{false};
};

// A memoized compile-time call: the function and its interned arguments (Q7)
struct memo_key {
    const instance*        fn;
    std::vector<ip::index> args;

    [[nodiscard]] auto operator==(const memo_key&) const noexcept -> bool = default;
};

struct memo_key_hash {
    using is_avalanching = void;
    [[nodiscard]] auto operator()(const memo_key& key) const noexcept -> u64;
};

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
    // Returns `type`, so every call runs at compile time and there is no runtime body
    bool comptime_only{false};
};

// The quotas every compile-time loop and call is bounded by (§9 rule 5)
struct eval_limits {
    usize recursion;
    usize unroll;
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

    // The declaration's value, analyzing it on first use.
    // A use during its own analysis is a cycle reported at `use`
    [[nodiscard]] auto analyze_decl(const unit& u, decl_id decl, const source& use)
        -> result<operand>;
    // Puts the function's body on the work queue
    auto request_body(instance& fn) -> void;
    // Declares the function `decl`'s `func` value creates, leaving its body for the queue
    [[nodiscard]] auto declare_function(const unit& u, decl_id decl, inst_id func)
        -> result<operand>;

    // Reports a diagnostic at `src`, with the compile-time calls in progress as notes, and returns
    // the poison stop. An identical diagnostic already reported isn't repeated
    [[nodiscard]] auto report(const unit&                          u,
                              const source&                        src,
                              gs::error                            code,
                              std::string                          message,
                              const stdx::option<diagnostic_note>& note = stdx::none)
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

    // Runs `fn` at compile time on interned arguments, memoized when the call is pure (Q7)
    [[nodiscard]] auto
    call_comptime(const unit& caller, const source& src, instance& fn, std::vector<ip::index> args)
        -> result<operand>;
    [[nodiscard]] auto limits() noexcept -> eval_limits& { return limits_; }

    // Comptime memory (§7)
    [[nodiscard]] auto new_alloc(const comptime_alloc& alloc) -> comptime_place;
    [[nodiscard]] auto alloc_at(comptime_place p) -> comptime_alloc& { return allocs_[p.alloc]; }
    [[nodiscard]] auto alloc_count() const noexcept -> u32 {
        return static_cast<u32>(allocs_.size());
    }
    // Drops the allocations from `base` on, once the analysis that made them ends
    auto release_allocs(u32 base) -> void { allocs_.resize(base); }
    // A store makes impure every call in progress that didn't make the allocation
    auto               note_store(comptime_place p) -> void;
    [[nodiscard]] auto next_frame() noexcept -> u32 { return next_frame_++; }

    // Records where a comptime evaluation needed a runtime value and returns the deferred stop
    [[nodiscard]] auto needs_runtime(const unit& u, const source& src) -> stdx::err<stop>;
    // Notes that the runtime need came through the immutable `let` named `name`
    auto note_runtime_let(const unit& u, std::string_view name, const source& init) -> void;
    // Reports the deferred need at `at`, or else at the operand that wasn't known
    [[nodiscard]] auto report_runtime_need(const unit&                 u,
                                           const stdx::option<source>& at,
                                           gs::error                   code,
                                           std::string                 message) -> stdx::err<stop>;

  private:
    struct runtime_site {
        gsl::not_null<const unit*>    u;
        source                        src;
        stdx::option<diagnostic_note> through_let{};
    };

    // A finished call's value, or none for one whose failure was already reported
    using memo_entry = stdx::option<operand>;

  private:
    auto               analyze_roots() -> void;
    auto               drain_queue() -> void;
    [[nodiscard]] auto analyze_body(instance& fn) -> result<void>;
    // A declaration whose whole value is `@compileError(...)`, which fires where it is used
    [[nodiscard]] auto is_deferred_error(const unit& u, const decl& d) const -> bool;
    [[nodiscard]] auto deferred_error_message(const unit& u, const decl& d) const -> std::string;
    [[nodiscard]] auto root_values() -> ankerl::unordered_dense::map<std::string, ip::index>;
    // Notes naming the compile-time calls in progress, innermost first
    auto add_call_notes(gs::diagnostic& d) const -> void;

  private:
    gs::context&                                                      ctx_;
    const program&                                                    prog_;
    options                                                           opts_;
    std::vector<unit>                                                 units_;
    std::vector<std::vector<decl_state>>                              decls_;
    std::vector<stdx::box<instance>>                                  instances_;
    std::vector<gsl::not_null<instance*>>                             queue_;
    gir::module_t                                                     gir_;
    gs::diagnostics                                                   diagnostics_;
    stdx::option<sema::unsupported>                                   stopped_at_;
    u32                                                               ptr_bits_;
    std::vector<comptime_alloc>                                       allocs_;
    std::vector<comptime_frame>                                       calls_;
    ankerl::unordered_dense::map<memo_key, memo_entry, memo_key_hash> memo_;
    eval_limits                                                       limits_;
    stdx::option<runtime_site>                                        runtime_site_;
    u32                                                               next_frame_{0};
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
    // A compile-time call of `fn`
    analyzer(compilation& comp, const unit& u, instance& fn) noexcept;
    ~analyzer();
    MAKE_PINNED(analyzer);

    // A comptime body's yielded value
    [[nodiscard]] auto eval_comptime(gsl::span<const inst_id> body) -> result<operand>;
    // Runs the function's body at compile time on arguments of its parameter types
    [[nodiscard]] auto eval_call(gsl::span<const ip::index> args) -> result<operand>;
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

    // A comptime `break`, `continue` or `return` unwinding to what consumes it
    struct jump {
        enum class kind : u8 {
            BREAK,
            REPEAT,
            RETURN,
        };
        kind                  what;
        stdx::option<inst_id> target;
        stdx::option<operand> value;
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
    [[nodiscard]] auto
    fold_shift(const binary_info& info, const operand& lhs, const operand& rhs, const source& src)
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
    // Whether `gir::semantics` folds `t`'s values, which it does up to 128 bits
    [[nodiscard]] auto folds_exactly(const gs::type& t) const -> bool;
    // Whether the runtime instruction checks `op` on `t`, as the old emitter decides it
    [[nodiscard]] auto checks_overflow(gir::semantics::int_op op, const gs::type& t) const -> bool;

    // Coercion (coerce.cc)
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
    [[nodiscard]] auto analyze_alloc_mut(inst_id inst, bool is_comptime_alloc) -> result<operand>;
    [[nodiscard]] auto store_comptime(comptime_place p, const operand& value, const source& src)
        -> result<void>;
    [[nodiscard]] auto load_comptime(comptime_place p) -> operand;
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
    [[nodiscard]] auto analyze_block_comptime(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_condbr_comptime(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_eval_branch(inst_id inst) -> result<operand>;
    // A block at compile time, taking a `break` that targets it as its value
    [[nodiscard]] auto run_block(inst_id inst, gsl::span<const inst_id> body) -> result<operand>;
    [[nodiscard]] auto run_loop(inst_id inst) -> result<operand>;
    // Analyzes what follows at compile time until the guard ends
    [[nodiscard]] auto comptime_scope() {
        ++comptime_depth_;
        return gsl::finally([this] { --comptime_depth_; });
    }
    // Counts a runtime branch around what follows until the guard ends (Q6)
    [[nodiscard]] auto runtime_branch() {
        ++runtime_depth_;
        return gsl::finally([this] { --runtime_depth_; });
    }
    // Reports a runtime need that reached a comptime boundary in a runtime function
    [[nodiscard]] auto at_comptime_boundary(result<operand> value) -> result<operand>;
    [[nodiscard]] auto known_bool(const operand& op, const source& src) -> result<bool>;
    [[nodiscard]] auto finish_merge(const std::vector<incoming>& arrivals,
                                    gir::segment_id              merge,
                                    const source&                src) -> result<operand>;
    [[nodiscard]] auto target_of(ref r) -> target&;
    auto               arrive(target& t, const operand& value, const source& src) -> void;
    [[nodiscard]] auto condition(const operand& op, const source& src) -> result<gir::value>;
    [[nodiscard]] auto void_value() -> operand;

    // Calls (calls.cc)
    [[nodiscard]] auto analyze_call(inst_id inst) -> result<operand>;
    [[nodiscard]] auto call_at_comptime(instance&            target,
                                        gsl::span<const ref> args,
                                        const source&        src) -> result<operand>;

    // Builtins (builtins.cc)
    [[nodiscard]] auto analyze_builtin(inst_id inst) -> result<operand>;
    [[nodiscard]] auto builtin_compile_error(gsl::span<const ref> args, const source& src)
        -> result<operand>;
    [[nodiscard]] auto builtin_limit(gsl::span<const ref> args, const source& src, bool recursion)
        -> result<operand>;
    [[nodiscard]] auto builtin_layout(gsl::span<const ref> args, const source& src, bool size)
        -> result<operand>;
    [[nodiscard]] auto builtin_assert(gsl::span<const ref> args, const source& src)
        -> result<operand>;

    // Comptime knownness
    // The operand's comptime value, or the runtime need when it has none
    [[nodiscard]] auto require_comptime(const operand& op, const source& src) -> result<ip::index>;
    // `r` evaluated at compile time, running its instructions again when they were runtime
    [[nodiscard]] auto reevaluate(ref r, const source& src) -> result<operand>;

    // Declarations (decls.cc)
    [[nodiscard]] auto analyze_global(decl_id                 decl,
                                      const operand&          value,
                                      stdx::option<gs::type&> declared,
                                      const source&           src) -> result<operand>;

    // GIR glue (emit.cc)
    [[nodiscard]] auto is_comptime() const noexcept -> bool {
        return !gir_fn_ || comptime_depth_ > 0;
    }
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
    // The runtime branches enclosing the instruction being analyzed (Q6)
    u32                frame_;
    u32                runtime_depth_{0};
    u32                alloc_base_;
    stdx::option<jump> jump_;
};

// Messages shared with the old pipeline's checks (messages.cc)
namespace messages {

constexpr std::string_view MUTABLE_TYPE_BINDING{
    "a 'type' value cannot be stored in a 'let' or 'let mut' binding; use 'const' instead"};

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

// `@compileError`'s message, read from its string literal argument
[[nodiscard]] auto compile_error_message(const module_t& m, gsl::span<const ref> args)
    -> std::string;

// The spelling of an operator tag in diagnostics, `+` for `add`
[[nodiscard]] auto operator_spelling(tag t) noexcept -> std::string_view;

// `value` as a `type` constant, at the narrowest integer width that holds it
[[nodiscard]] auto intern_int(ip::pool& pool, const big_int& value, gs::type& type) -> ip::index;
// An integer constant's value, none for any other kind of constant
[[nodiscard]] auto int_of(const ip::value& v) -> stdx::option<big_int>;

} // namespace ghoti::uir::sema
