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
#include "compiler/sema/pattern.hh"
#include "compiler/sema/type.hh"
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

// Where GIR's built-in aggregates keep their parts
inline constexpr u64 SLICE_PTR_FIELD{0};
inline constexpr u64 SLICE_LEN_FIELD{1};
inline constexpr u64 ARRAY_PTR_FIELD{0};
inline constexpr u64 UNION_TAG_FIELD{0};
inline constexpr u64 UNION_PAYLOAD_FIELD{1};

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

// A tagged union's field whose payload address isn't computed yet, since a read checks the tag
// before it
struct union_field {
    gir::value       union_address;
    u64              index;
    std::string_view name;
};

// An addressable slot that loads read and stores write
struct place {
    gir::value               address;
    gsl::not_null<gs::type*> pointee;
    bool                     is_mutable;
    stdx::option<union_field> pending{};
};

// The values a `for` range's counter takes, which let it convert to any integer holding them all
struct value_range {
    big_int lo;
    big_int hi;
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

// An immutable runtime value kept in a slot, read by loading it and addressed where it lies
struct slot_value {
    gir::value               address;
    gsl::not_null<gs::type*> type;
    // A parameter the body assigns to, whose slot stores may write
    bool is_mutable{false};
    // A `for` range capture with known bounds
    stdx::option<value_range> range{};
};

// An untyped `.{ ... }` literal, which takes the type it is coerced to
struct anon_init {
    inst_id inst;
};

// An untyped `.name`, which names a variant of the enum it is coerced to
struct enum_literal {
    inst_id inst;
};

// A module-level aggregate `const`, which runtime code reads through its GIR global
struct global_const {
    gsl::not_null<const std::string*> symbol;
    gsl::not_null<gs::type*>          type;
    ip::index                         value;
};

// A comptime value, a runtime value, a function, a place, a pending untyped merge, a global, a
// comptime allocation, a value in a slot, an untyped literal, or an aggregate global
using operand = stdx::variant<ip::index,
                              gir::value,
                              function_ref,
                              place,
                              merge_ref,
                              global_ref,
                              comptime_place,
                              slot_value,
                              anon_init,
                              enum_literal,
                              global_const>;

// Mutable compile-time state, which only stores change
struct comptime_alloc {
    // None until an untyped allocation's first store gives it one
    stdx::option<gs::type&> type;
    stdx::option<ip::index> value;
    // The analysis that owns it, which alone may store to it
    u32 frame;
    // The runtime branches around its declaration, which a store may not add to
    u32  runtime_depth;
    bool keeps_comptime_type;
};

struct memo_key;

// One compile-time call in progress, for the recursion limit and the notes of its errors
struct comptime_frame {
    gsl::not_null<const unit*>     caller;
    source                         call_src;
    std::string_view               callee;
    gsl::not_null<const memo_key*> key;
    // Allocations from here on belong to the call, so storing below it makes the call impure
    u32  alloc_base;
    bool impure{false};
};

// A memoized compile-time call
struct memo_key {
    gsl::not_null<const instance*> fn;
    std::vector<ip::index>         args;

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
    bool                                  weak{false};
    bool                                  queued{false};
    // Returns `type`, so every call runs at compile time and there is no runtime body
    bool comptime_only{false};
};

// The quotas every compile-time loop and call is bounded by
struct eval_limits {
    usize recursion;
    usize unroll;
};

enum class nominal_state : u8 {
    DECLARED, // Identity and name, enough for a pointer or slice to it
    FIELDS_RESOLVING,
    RESOLVED,
    LAYOUT_RESOLVING, // Inside an `@sizeOf` or `@alignOf` of itself
    FAILED,
};

// A struct, union or enum type a `*_decl` instruction created
struct nominal {
    gsl::not_null<const unit*> owner;
    inst_id                    inst;
    gsl::not_null<gs::type*>   type;
    std::string                name;
    nominal_state              state{nominal_state::DECLARED};
    // Created inside a comptime call, whose instance its members would need
    bool in_call{false};
    // An enum's value for each variant, in declaration order
    std::vector<i64> enum_values{};
};

// A nominal type's identity: its instruction, and the comptime call it was created in
struct nominal_key {
    u32                    unit;
    inst_id                inst;
    std::vector<ip::index> args;

    [[nodiscard]] auto operator==(const nominal_key&) const noexcept -> bool = default;
};

struct nominal_key_hash {
    using is_avalanching = void;
    [[nodiscard]] auto operator()(const nominal_key& key) const noexcept -> u64;
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

    // The GIR symbol and linkage of a module-level declaration. Two declarations claiming one
    // name stop, as qualifying them arrives with multi-module programs
    [[nodiscard]] auto symbol_for(const unit& u, decl_id decl, const source& src)
        -> result<std::string>;
    [[nodiscard]] static auto linkage_of(const decl& d) noexcept -> gir::linkage;
    // Keeps a `let mut` global's symbol alive for the `global_ref`s that name it
    [[nodiscard]] auto own_symbol(const unit& u, decl_id decl, std::string symbol)
        -> const std::string&;

    // Runs `fn` at compile time on interned arguments, memoized when the call is pure
    [[nodiscard]] auto
    call_comptime(const unit& caller, const source& src, instance& fn, std::vector<ip::index> args)
        -> result<operand>;
    [[nodiscard]] auto limits() noexcept -> eval_limits& { return limits_; }

    // Comptime memory
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

    // The struct, union or enum `inst` declares, created the first time the current comptime call
    // reaches it
    [[nodiscard]] auto declare_nominal(const unit& u, inst_id inst, gs::type_kind kind) -> nominal&;
    [[nodiscard]] auto nominal_of(const gs::type& t) -> stdx::option<nominal&>;
    // A copy of `name` that lives as long as the context
    [[nodiscard]] auto lasting_name(std::string_view name) -> std::string_view;
    // The nominal `inst` declared outside any comptime call, if it was declared yet
    [[nodiscard]] auto existing_nominal(const unit& u, inst_id inst) -> stdx::option<nominal&>;
    // Names an anonymous nominal type for the declaration that declared it
    auto name_nominal(gs::type& t, std::string_view name) -> void;
    // Resolves `t`'s fields the first time something needs them. A use while they resolve is a
    // cycle, reported at `use`
    [[nodiscard]] auto require_fields(const unit& u, const source& use, gs::type& t)
        -> result<void>;

    // A declaration of the compiler's `builtin` module, analyzed the first time `use` needs it
    [[nodiscard]] auto prelude_decl(std::string_view name, const unit& user, const source& use)
        -> result<operand>;
    [[nodiscard]] auto prelude_function(std::string_view name, const unit& user, const source& use)
        -> result<gsl::not_null<instance*>>;
    [[nodiscard]] auto prelude_type(std::string_view name, const unit& user, const source& use)
        -> result<gsl::not_null<gs::type*>>;

    // What a speculative compile-time evaluation could leave behind, so it can be undone
    struct speculation {
        usize diagnostics;
        bool  stopped;
        bool  runtime_site;
    };
    [[nodiscard]] auto speculate() const -> speculation;
    // Drops the diagnostics, runtime need and, unless kept, unsupported stop recorded since `s`
    auto abandon(const speculation& s, bool keep_unsupported = false) -> void;

    // The `and` / `or` whose left side is the `and` / `or` `inst`, if any
    [[nodiscard]] auto bool_parent(const unit& u, inst_id inst) -> stdx::option<inst_id>;

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
    // Analyzes the member declarations of a type a module-level declaration declared
    auto analyze_members(const unit& u, gs::type& owner) -> void;
    // The type a declaration's value body declares directly, as `const P = struct { ... }` does
    [[nodiscard]] auto declared_type(const unit&                   u,
                                     const stdx::option<body_ref>& body,
                                     const operand& value) -> stdx::option<gs::type&>;
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
    std::vector<stdx::box<nominal>>                                   nominals_;
    ankerl::unordered_dense::map<nominal_key, u32, nominal_key_hash>  nominal_ids_;
    // The `builtin` module, lowered here since the program's modules don't import it
    stdx::option<lowered> prelude_;
    stdx::option<u32>     prelude_unit_;
    // Per unit, each `and` / `or` that is the left side of another, built on first use
    std::vector<stdx::option<ankerl::unordered_dense::map<inst_id, inst_id>>> bool_parents_;
    // Each GIR symbol a declaration claimed, with the unit and declaration that claimed it
    ankerl::unordered_dense::map<std::string, std::pair<u32, decl_id>> symbols_;
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
    // Resolves a nominal type's fields, or an enum's values
    [[nodiscard]] auto resolve_fields(nominal& n) -> result<void>;

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

    // A `for` range's bounds once resolved
    struct range_operands {
        stdx::option<operand> lo;
        stdx::option<operand> hi;
        bool                  lo_is_literal{false};
    };

    // One `for` iterable while its loop is emitted: a range's counter, or an index into a container
    struct iterable {
        bool                      is_range;
        bool                      inclusive;
        bool                      open_upper;
        gir::value                counter;
        gsl::not_null<gs::type*>  elem;
        gir::value                end;
        stdx::option<gir::value>  descending;
        gir::value                container;
        bool                      is_slice;
        stdx::option<value_range> values{};
    };

    // One `match` arm as UIR lists it
    struct match_arm {
        gsl::span<const ref>     patterns;
        stdx::option<ref>        capture;
        gsl::span<const inst_id> guard;
        gsl::span<const inst_id> body;
    };

    // A name a `match` arm binds, held in its own slot
    struct capture_slot {
        std::string_view         name;
        gsl::not_null<gs::type*> type;
        gir::value               address;
        bool                     is_void;
    };

    // A runtime `match` while its decision tree is emitted
    struct match_lowering {
        const gs::pattern::decision_tree&      tree;
        gir::value                             root;
        std::vector<gir::segment_id>           bodies;
        std::vector<std::vector<capture_slot>> slots;
        source                                 src;
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
    // `^T`, `&T` and `[]T`
    [[nodiscard]] auto analyze_elem_type(inst_id inst, tag t) -> result<operand>;
    [[nodiscard]] auto analyze_array_type(inst_id inst) -> result<operand>;
    // An array length, which must be known at compile time
    [[nodiscard]] auto array_length(ref len_ref, const source& src) -> result<usize>;

    // Nominal types (aggregates.cc)
    [[nodiscard]] auto analyze_nominal_decl(inst_id inst, gs::type_kind kind) -> result<operand>;
    [[nodiscard]] auto resolve_struct(nominal& n) -> result<void>;
    [[nodiscard]] auto resolve_union(nominal& n) -> result<void>;
    [[nodiscard]] auto resolve_enum(nominal& n) -> result<void>;
    [[nodiscard]] auto field_type_of(gsl::span<const inst_id> body,
                                     std::string_view         name,
                                     const source& src) -> result<gsl::not_null<gs::type*>>;
    // The enum's variant `name` as a constant
    [[nodiscard]] auto enum_constant(gs::type& enum_type, std::string_view name, const source& src)
        -> result<operand>;
    // `T.name`: an enum variant, or a member declaration
    [[nodiscard]] auto member_of(gs::type& owner, std::string_view name, const source& src)
        -> result<operand>;

    // Field and element access (access.cc)
    [[nodiscard]] auto analyze_field_val(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_field_ptr(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_elem_val(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_elem_ptr(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_ref_of(inst_id inst, bool is_pointer) -> result<operand>;
    [[nodiscard]] auto analyze_deref(inst_id inst) -> result<operand>;
    // The storage `r` names, spilling a value that has none into a slot
    [[nodiscard]] auto address_of(ref r, const source& src) -> result<place>;
    [[nodiscard]] auto spill(const gir::value& v, gs::type& type, bool to_entry) -> place;
    [[nodiscard]] auto load(const place& p) -> gir::value;
    // A place with its address computed, a tagged union's payload included
    [[nodiscard]] auto settle(const place& p) -> place;
    // Loads a place, checking a tagged union's active field first
    [[nodiscard]] auto read(const place& p, const source& src) -> result<gir::value>;
    // The storage of an operand whose instruction isn't at hand, spilling a value with none
    [[nodiscard]] auto operand_address(const operand& op, const source& src) -> result<place>;
    [[nodiscard]] auto global_place(std::string_view symbol, gs::type& type, bool is_mutable)
        -> place;
    // `type` as a binding named by `r` holds it, mutable unless the binding is immutable
    [[nodiscard]] auto binding_view(ref r, gs::type& type) -> gs::type&;
    // What a pointer or reference place points at, else the place itself
    [[nodiscard]] auto through_pointer(const place& base, const source& src) -> result<place>;
    // The place a pointer or reference value points at
    [[nodiscard]] auto pointer_place(const operand& ptr, const source& src) -> result<place>;
    [[nodiscard]] auto field_index(gs::type& type, std::string_view name, const source& src)
        -> result<usize>;
    // A field's type as a place reaches it, with an array's elements mutable through a mutable one
    [[nodiscard]] auto mutable_view(gs::type& field, bool through_mutable) -> gs::type&;
    [[nodiscard]] auto field_place(const place& object, std::string_view name, const source& src)
        -> result<place>;
    [[nodiscard]] auto index_operand(ref index_ref, const source& src) -> result<gir::value>;
    [[nodiscard]] auto
    bounds_check(const gir::value& index, const gir::value& bound, bool sentinel, const source& src)
        -> result<void>;
    [[nodiscard]] auto elem_place(const place& object, ref index_ref, const source& src)
        -> result<place>;
    [[nodiscard]] auto comptime_field(ip::index object, std::string_view name, const source& src)
        -> result<operand>;
    [[nodiscard]] auto comptime_elem(ip::index object, ip::index at, const source& src)
        -> result<operand>;

    // Ranges, slicing and `for` (iteration.cc)
    [[nodiscard]] auto analyze_range(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_bind(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_slice(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_for(inst_id inst) -> result<operand>;
    [[nodiscard]] auto usize_bound(ref r, const source& src) -> result<gir::value>;
    // The type a `for` range counts in
    [[nodiscard]] auto counter_type(const range_operands& r) -> gs::type&;
    // Every value a range takes, when both its bounds are known
    [[nodiscard]] auto known_range(const range_operands& r, bool inclusive)
        -> stdx::option<value_range>;
    // A `for` at compile time, binding each iteration's values as constants
    [[nodiscard]] auto run_for(inst_id                                   inst,
                               gsl::span<const ref>                      iterables,
                               const std::vector<stdx::option<inst_id>>& binds,
                               gsl::span<const inst_id>                  body,
                               gsl::span<const inst_id> else_body) -> result<operand>;

    // `match` (match.cc)
    [[nodiscard]] auto analyze_match(inst_id inst) -> result<operand>;
    [[nodiscard]] auto match_arms(inst_id inst) -> std::vector<match_arm>;
    [[nodiscard]] auto capture_name(const ast::capture_t& capture) const -> std::string_view;
    // A pattern's value, which must be known at compile time
    [[nodiscard]] auto pattern_value(gsl::span<const inst_id> body,
                                     gs::type&                type,
                                     const source&            src) -> result<ip::index>;
    [[nodiscard]] auto pattern_node(inst_id pat, gs::type& type) -> result<gs::pattern::node_ptr>;
    // The value of a runtime match that runs entirely at compile time to a number or `bool`
    [[nodiscard]] auto fold_match(const std::vector<match_arm>&              arms,
                                  const std::vector<gs::pattern::row_input>& rows,
                                  ref                                        operand_ref,
                                  const source& src) -> result<stdx::option<operand>>;
    // A `match` at compile time, which analyzes only the arm it takes
    [[nodiscard]] auto run_match(const std::vector<match_arm>&              arms,
                                 const std::vector<gs::pattern::row_input>& rows,
                                 const operand&                             scrutinee,
                                 const source&                              src) -> result<operand>;
    [[nodiscard]] auto emit_match(const std::vector<match_arm>&              arms,
                                  const std::vector<gs::pattern::row_input>& rows,
                                  const gs::pattern::decision_tree&          tree,
                                  ref                                        operand_ref,
                                  const operand&                             scrutinee,
                                  const source& src) -> result<operand>;
    [[nodiscard]] auto
    scrutinee_address(ref operand_ref, const operand& scrutinee, bool aggregate, const source& src)
        -> result<gir::value>;
    [[nodiscard]] auto occurrence_address(const match_lowering& m, usize occ) -> gir::value;
    [[nodiscard]] auto emit_decision(const match_lowering& m, usize node) -> result<void>;

    // Initializers (init.cc)
    [[nodiscard]] auto analyze_init_struct(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_init_array(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_array_lit(inst_id inst) -> result<operand>;
    // The initializer `inst` builds as a `type`
    [[nodiscard]] auto init_as(inst_id inst, gs::type& type, const source& src) -> result<operand>;
    [[nodiscard]] auto init_struct(inst_id inst, gs::type& type, const source& src)
        -> result<operand>;
    [[nodiscard]] auto init_union(inst_id inst, gs::type& type, const source& src)
        -> result<operand>;
    [[nodiscard]] auto init_array(inst_id inst, gs::type& type, const source& src)
        -> result<operand>;
    // Each field's default body, empty for a field with none
    [[nodiscard]] auto field_defaults(gs::type& type, const source& src)
        -> result<std::vector<gsl::span<const inst_id>>>;
    [[nodiscard]] auto
    field_default(gs::type& type, usize index, gsl::span<const inst_id> body, const source& src)
        -> result<operand>;
    // Builds a folded aggregate at runtime, as the old emitter materializes a constant
    [[nodiscard]] auto materialize(ip::index index, const source& src) -> result<gir::value>;

    // Runtime safety (safety.cc)
    [[nodiscard]] auto usize_constant(u64 value) -> gir::value;
    [[nodiscard]] auto string_slice(std::string_view text, gs::type& slice_type) -> gir::value;
    // A string constant viewed as a slice of `slice_type`, as the old emitter builds it
    [[nodiscard]] auto string_as_slice(std::string_view text, gs::type& slice_type) -> gir::value;
    [[nodiscard]] auto emit_panic(std::string_view message, const source& src) -> result<void>;
    // Continues where `ok` holds, panicking with `message` where it doesn't
    [[nodiscard]] auto guard(gir::value ok, std::string_view message, const source& src)
        -> result<void>;
    [[nodiscard]] auto check_not_null(const gir::value& ptr, const source& src) -> result<void>;
    [[nodiscard]] auto check_active_field(const gir::value& union_address,
                                          u64               field,
                                          std::string_view  name,
                                          const source&     src) -> result<void>;

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
    // `u == .field` on a tagged union, none for any other comparison
    [[nodiscard]] auto
    compare_tag(const binary_info& info, const operand& lhs, const operand& rhs, const source& src)
        -> result<stdx::option<operand>>;
    // A comparison of pointers or `nullptr`, none for any other comparison
    [[nodiscard]] auto compare_pointers(const binary_info& info,
                                        const operand&     lhs,
                                        const operand&     rhs,
                                        const source&      src) -> result<stdx::option<operand>>;
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
    // The array of type `from` at `address` viewed as a slice of its elements, as the old emitter
    // decays it
    [[nodiscard]] auto array_as_slice(const gir::value& address, gs::type& from, gs::type& target)
        -> result<stdx::option<operand>>;
    [[nodiscard]] auto as_runtime(const operand& op, const source& src) -> result<gir::value>;
    [[nodiscard]] auto materialize_merge(merge_ref m, gs::type& type, const source& src)
        -> result<gir::value>;
    // A `.{...}` or `.name` that takes its type from where it is used
    [[nodiscard]] static auto is_untyped_literal(const operand& op) -> bool {
        return op.is<anon_init>() || op.is<enum_literal>();
    }
    [[nodiscard]] auto concrete(const operand& op, const source& src) -> result<operand>;

    // Locals and memory (memory.cc)
    [[nodiscard]] auto analyze_alloc_mut(inst_id inst, bool is_comptime_alloc) -> result<operand>;
    [[nodiscard]] auto store_comptime(comptime_place p, const operand& value, const source& src)
        -> result<void>;
    [[nodiscard]] auto load_comptime(comptime_place p) -> operand;
    [[nodiscard]] auto analyze_let_value(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_const_local(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_store(inst_id inst) -> result<operand>;
    // Where a store to `target` writes: a place, a written parameter, or what a pointer points at
    [[nodiscard]] auto store_destination(const operand& target, const source& src) -> result<place>;
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
    // Counts a runtime branch around what follows until the guard ends
    [[nodiscard]] auto runtime_branch() {
        ++runtime_depth_;
        return gsl::finally([this] { --runtime_depth_; });
    }
    // Reports a runtime need that reached a comptime boundary in a runtime function
    [[nodiscard]] auto at_comptime_boundary(result<operand> value) -> result<operand>;
    [[nodiscard]] auto known_bool(const operand& op, const source& src) -> result<bool>;
    // The result slot of a runtime `and` / `or`, holding the value its left side decides alone
    [[nodiscard]] auto bool_slot(inst_id inst, bool is_and) -> gir::value;
    // Whether a branch body ends yielding nothing, as a statement `if`'s arms do
    [[nodiscard]] auto yields_nothing(gsl::span<const inst_id> body) const -> bool;
    // A runtime condition's value when running it again at compile time knows it
    [[nodiscard]] auto folded_condition(ref cond, const source& src) -> result<stdx::option<bool>>;
    [[nodiscard]] auto finish_merge(const std::vector<incoming>& arrivals,
                                    gir::segment_id              merge,
                                    const source&                src) -> result<operand>;
    [[nodiscard]] auto target_of(ref r) -> target&;
    auto               arrive(target& t, const operand& value, const source& src) -> void;
    [[nodiscard]] auto condition(const operand& op, const source& src) -> result<gir::value>;
    [[nodiscard]] auto void_value() -> operand;

    // Calls (calls.cc)
    [[nodiscard]] auto analyze_call(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_field_call(inst_id inst) -> result<operand>;
    // A call of `target`. `through_member` calls a type's function by its name, as `T.f(x)` does
    [[nodiscard]] auto call_function(instance&            target,
                                     gsl::span<const ref> args,
                                     const source&        src,
                                     const source&        callee_src,
                                     bool                 through_member) -> result<operand>;
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
    [[nodiscard]] auto builtin_trap(const source& src) -> result<operand>;
    [[nodiscard]] auto builtin_as(gsl::span<const ref> args, const source& src) -> result<operand>;
    [[nodiscard]] auto builtin_this(const source& src) -> result<operand>;
    // Resolves what `@sizeOf(type)` needs, reporting a layout that depends on itself
    [[nodiscard]] auto layout_ready(gs::type& type, const source& src) -> result<void>;

    // Comptime knownness
    // The operand's comptime value, or the runtime need when it has none
    [[nodiscard]] auto require_comptime(const operand& op, const source& src) -> result<ip::index>;
    // `r` evaluated at compile time, running its instructions again when they were runtime
    [[nodiscard]] auto reevaluate(ref r, const source& src) -> result<operand>;

    // Declarations (decls.cc)
    // Whether the declaration's value is a type it declares, as `const P = struct { ... }` is
    [[nodiscard]] auto declares_type(const decl& d) const -> bool;
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
    // Result slots an inner `and` / `or` already made for the ones it is the left side of
    ankerl::unordered_dense::map<inst_id, gir::value> bool_slots_;
    std::vector<target>                            targets_;
    std::vector<pending_merge>                     merges_;
    // Nonzero inside a comptime body of a runtime function, where nothing may emit GIR
    usize comptime_depth_{0};
    // The runtime branches enclosing the instruction being analyzed
    u32                frame_;
    u32                runtime_depth_{0};
    u32                alloc_base_;
    stdx::option<jump> jump_;
};

// Messages shared with the old pipeline's checks (messages.cc)
namespace messages {

constexpr std::string_view MUTABLE_TYPE_BINDING{
    "a 'type' value cannot be stored in a 'let' or 'let mut' binding; use 'const' instead"};
constexpr std::string_view MUTABLE_ADDRESS_OF_IMMUTABLE{
    "Cannot take a mutable address of an immutable value"};
// UIR's own: the old pipeline accepts it (docs/uir/known_legacy_bugs.md)
constexpr std::string_view MUTABLE_ADDRESS_OF_ELEMENT{
    "Cannot take a mutable address of an element of a non-mutable array or slice"};

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

// Whether a value of `type` holds a `type` somewhere, which only exists at compile time
[[nodiscard]] auto holds_types(const gs::type& type) -> bool;

// Whether a binding's value lives on past one use, so its spill is reused
[[nodiscard]] constexpr auto is_binding(tag t) noexcept -> bool {
    return t == tag::param || t == tag::let_value || t == tag::bind;
}
// An integer constant's value, none for any other kind of constant
[[nodiscard]] auto int_of(const ip::value& v) -> stdx::option<big_int>;

} // namespace ghoti::uir::sema
