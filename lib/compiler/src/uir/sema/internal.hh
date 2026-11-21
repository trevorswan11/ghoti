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

#include "compiler/arena.hh"
#include "compiler/ast/attributes.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/id.hh"
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
#include "compiler/sema/side_tables.hh"
#include "compiler/sema/type.hh"
#include "compiler/uir/astgen.hh"
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
    gsl::not_null<mod::module_t*>  source; // mutable since type payloads hold their module
    gsl::not_null<const module_t*> uir;
    u32                            index;
};

struct instance;
struct generic_fn;
struct nominal;

// A function declaration used as a value
struct function_ref {
    gsl::not_null<instance*> fn;
};

// A function with comptime or inferred parameters, which each call instantiates
struct generic_ref {
    gsl::not_null<generic_fn*> fn;
};

// A tagged union's field whose payload address isn't computed yet (read checks tag before it)
struct union_field {
    gir::value       union_address;
    u64              index;
    std::string_view name;
};

// An addressable slot that loads read and stores write
struct place {
    gir::value                address;
    gsl::not_null<gs::type*>  pointee;
    bool                      is_mutable;
    stdx::option<union_field> pending{};
    // A by-value parameter's own copy, whose array fields read with mutable elements
    bool owned_copy{false};
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
    // A comptime parameter's value, which compile-time reads see instead of the slot
    stdx::option<ip::index> known{};
    // An immutable binding spilled only to be addressed, whose value reads use directly
    stdx::option<gir::value> origin{};
    // A by-value parameter's own copy, whose array fields read with mutable elements
    bool owned_copy{false};
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

// An imported module, which only names its declarations
struct module_ref {
    gsl::not_null<const unit*> target;
    // The name the module was reached through, for diagnostics
    std::string_view name;
};

// A one-argument cast, whose target is the type it is used as: `let x: u8 = @intCast(y)`
struct cast_ref {
    inst_id inst;
};

// A pack parameter's values, held by the analysis that bound them
struct pack_ref {
    u32 index;
};

// What analyzing an instruction yields
using operand = stdx::variant<ip::index,      // a comptime value
                              gir::value,     // a runtime value
                              function_ref,   // a function
                              generic_ref,    // a generic function
                              place,          // an addressable place
                              merge_ref,      // a pending merge of untyped branch values
                              global_ref,     // a module-level `let mut`
                              comptime_place, // a place in a comptime allocation
                              slot_value,     // a value held in a slot
                              anon_init,      // an untyped `.{ ... }` literal
                              enum_literal,   // an untyped `.name` literal
                              global_const,   // a module-level aggregate `const`
                              module_ref,     // an imported module
                              cast_ref,       // a cast waiting for its target type
                              pack_ref>;      // a pack parameter's values

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
    // Whether its value was given a GIR global
    bool has_storage{false};
};

// How an instance binds one of its template's parameters
struct param_binding {
    // The comptime value it is bound to, none for a runtime parameter
    stdx::option<ip::index> value;
    // Whether it has a GIR parameter, which a type parameter keeps though its value is known
    bool has_slot;
    // A pack's GIR parameters, one per value it holds
    stdx::option<usize> pack_size{};
};

// A function's signature, and the GIR function its body is analyzed into
struct instance {
    gsl::not_null<const unit*> owner;
    decl_id                    decl;
    inst_id                    func;
    std::string                symbol;
    gsl::not_null<gs::type*>   type;
    // The GIR parameters' types
    std::vector<gsl::not_null<gs::type*>> params;
    gsl::not_null<gs::type*>              return_type;
    gir::linkage                          linkage{gir::linkage::INTERNAL};
    bool                                  weak{false};
    bool                                  queued{false};
    // Returns `type`, so every call runs at compile time and there is no runtime body
    bool comptime_only{false};
    // One per UIR parameter of a generic's instance, empty for a non-generic function
    std::vector<param_binding> bindings{};
    // The return type is `auto` until the body's first `return` fixes it
    bool        infers_return{false};
    bool        analyzed{false};
    std::string source_name{};
    // The values the body's `capture`s read
    std::vector<ip::index> captures{};
    // The type a member function's `self` has, before its modifier
    stdx::option<gs::type&> self_type{};
    // The `self` parameter's modifier, for a method
    stdx::option<ast::type_modifier::modifier> self_modifier{};
    // The type whose namespace declares the function
    stdx::option<nominal&> scope{};
    // Its folded attributes, once known
    stdx::option<gs::resolved_attributes> attributes{};
};

// One `@[...]` entry with the operands of its arguments
struct attribute_item {
    ast::attribute_kind kind;
    std::vector<ref>    args;
    source              src;
    inst_id             inst;
};

// One field of a reflection struct being built
struct info_field {
    std::string_view name;
    ip::index        value;
};

// What an attribute list annotates
struct attribute_site {
    ast::attribute_target     target;
    bool                      returns_void{false};
    stdx::option<const decl&> declared_by{};
};

// A function declaration with comptime, `auto` or `type` parameters
struct generic_fn {
    gsl::not_null<const unit*> owner;
    decl_id                    decl;
    inst_id                    func;
    std::string                name;
    bool                       weak{false};
    std::vector<ip::index>     captures{};
    stdx::option<gs::type&>    self_type{};
    stdx::option<nominal&>     scope{};
    // The ctor instance declaring it, which its instances' symbols end with
    stdx::option<gs::type&> enclosing{};
};

// What a function declared inside a type sees of it
struct member_context {
    std::vector<ip::index>  captures{};
    stdx::option<gs::type&> self_type{};
    // The prefix a ctor instance's members are named under: `Box__u8`
    std::string_view       symbol_prefix{};
    stdx::option<nominal&> scope{};
    // The whole symbol, for a function no declaration names: an interface's default method
    std::string_view symbol{};
};

// A generic function and every parameter's interned binding: a value, or a runtime type
struct instance_key {
    gsl::not_null<const generic_fn*> fn;
    std::vector<ip::index>           args;

    [[nodiscard]] auto operator==(const instance_key&) const noexcept -> bool = default;
};

struct instance_key_hash {
    using is_avalanching = void;
    [[nodiscard]] auto operator()(const instance_key& key) const noexcept -> u64;
};

// An argument of a generic call, with the instruction that computed it when it has one
struct call_arg {
    operand           value;
    stdx::option<ref> written{};
};

// An `impl` bound on one of a function's parameters
struct impl_bound {
    u32              param;
    std::vector<ref> interfaces;
};

// A generic call's arguments bound to its template's parameters
struct generic_binding {
    instance_key                               key;
    std::vector<param_binding>                 bindings{};
    std::vector<gsl::not_null<gs::type*>>      slots{};
    std::vector<std::string>                   mangled_params{};
    std::vector<std::string>                   comptime_suffixes{};
    stdx::option<gs::type&>                    return_type{};
    bool                                       infers_return{false};
    stdx::option<ast::type_modifier::modifier> self_modifier{};
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
    // The comptime values its `capture`s read
    std::vector<ip::index> captures{};
    // The call instance's symbol, which its members are named under
    std::string symbol_prefix{};
    // A ctor instance's member declarations, each analyzed for this instance alone
    ankerl::unordered_dense::map<decl_id, decl_state> members{};
    // The ctor instance whose call created it
    stdx::option<const instance&> creator{};
    // The declaration whose value it is, which an interface's default methods are declared under
    stdx::option<decl_id> declaring_decl{};
};

// An `impl` block, registered before any analysis so member lookups can find it
struct impl_entry {
    gsl::not_null<const unit*> owner;
    decl_id                    decl;
    inst_id                    inst;
};

// An impl applied to one target type, with its parameters bound for that target
struct impl_instance {
    gsl::not_null<const impl_entry*> entry;
    gsl::not_null<gs::type*>         target;
    // One per impl parameter
    std::vector<ip::index>  params;
    stdx::option<gs::type&> interface;
    // The prefix a parameterized impl's methods are named under
    std::string prefix;
    // Each member declaration's analysis for this target
    ankerl::unordered_dense::map<decl_id, decl_state> members{};
    // An interface default method's instance for this target, by method index
    ankerl::unordered_dense::map<u32, operand> defaults{};
    // The vtable a `dyn` of the interface uses for this target, once one was made
    stdx::option<std::string> vtable{};
};

// One method an interface declares, in vtable order
struct interface_method {
    std::string_view name;
    inst_id          func;
};

struct impl_key {
    gsl::not_null<const impl_entry*> entry;
    gsl::not_null<const gs::type*>   target;

    [[nodiscard]] auto operator==(const impl_key&) const noexcept -> bool = default;
};

struct impl_key_hash {
    using is_avalanching = void;
    [[nodiscard]] auto operator()(const impl_key& key) const noexcept -> u64;
};

// A nominal type's instruction and the comptime call it was created in
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

// A declaration that may claim a GIR symbol, which a same-named one in another scope qualifies
struct name_candidate {
    u32     unit;
    decl_id decl;
    // A function or `let mut`, which always has storage, rather than a `const` that may not
    bool certain;
};

// Every name declared where it could claim a symbol, across the program
struct scoped_names {
    ankerl::unordered_dense::map<std::string, std::vector<name_candidate>> candidates;
    // Names a `weak` declaration carries, whose override shares its symbol
    ankerl::unordered_dense::set<std::string> weak;
};

class analyzer;

class compilation {
  public:
    // What a speculative compile-time evaluation could leave behind, so it can be undone
    struct speculation {
        usize diagnostics;
        bool  stopped;
        bool  runtime_site;
    };

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
    // Analyzes a body whose return type is inferred now, since a caller needs that type
    [[nodiscard]] auto infer_return(instance& fn, const unit& caller, const source& use)
        -> result<void>;
    // Declares the function `decl`'s `func` value creates, leaving its body for the queue
    [[nodiscard]] auto
    declare_function(const unit& u, decl_id decl, inst_id func, const member_context& member = {})
        -> result<operand>;
    // Modules (modules.cc)
    // The module an import of `u` loads, reached through the name `name`
    [[nodiscard]] auto
    import_target(const unit& u, import_id import, std::string_view name, const source& src)
        -> result<operand>;
    // The module-level declaration `name` of `m`, as `user` sees it
    [[nodiscard]] auto
    module_member(const module_ref& m, std::string_view name, const unit& user, const source& src)
        -> result<operand>;
    [[nodiscard]] auto prelude_module() -> stdx::option<module_ref>;

    // Impls (impls.cc)
    auto register_impl(const unit& u, decl_id decl) -> void;
    // Analyzes a plain impl's members, which are roots as every non-generic declaration is
    auto analyze_impl_roots(const unit& u, decl_id decl) -> void;
    // Every impl whose target is `target`, applied to it
    [[nodiscard]] auto impls_of(gs::type& target)
        -> result<std::vector<gsl::not_null<impl_instance*>>>;
    // The member `name` an applied impl gives its target, a default method included
    [[nodiscard]] auto impl_member(impl_instance& applied, std::string_view name, const source& use)
        -> result<stdx::option<operand>>;

    // `dyn` (dyn.cc)
    [[nodiscard]] auto interface_methods(const nominal& iface) -> std::vector<interface_method>;
    // Whether the interface's method `name` gives any parameter a default argument
    [[nodiscard]] auto method_has_defaults(const nominal& iface, std::string_view name) -> bool;
    // The vtable of an impl's interface for its target, made and its methods requested on first use
    [[nodiscard]] auto vtable_for(impl_instance& applied) -> result<std::string_view>;
    // Whether an interface declares associated types or constants
    [[nodiscard]] static auto declares_associated_items(const nominal& iface) -> bool;
    // The program's root module, whose list of vtables codegen lowers
    [[nodiscard]] auto root_source() -> mod::module_t&;

    // A member declaration of `n`, analyzed per instance when a comptime call created `n`
    [[nodiscard]] auto analyze_member(nominal& n, decl_id decl, const source& use)
        -> result<operand>;
    // What a function declared in `n`'s namespace sees of it
    [[nodiscard]] auto member_context_of(nominal& n) -> member_context;
    // A method's `self` type for its modifier, none for a modifier this phase doesn't handle
    [[nodiscard]] auto self_param_type(const member_context&        member,
                                       ast::type_modifier::modifier how) -> stdx::option<gs::type&>;

    // Generic functions (generics.cc)
    [[nodiscard]] auto is_generic(const unit& u, inst_id func) const -> bool;
    [[nodiscard]] auto
    declare_generic(const unit& u, decl_id decl, inst_id func, const member_context& member)
        -> result<operand>;
    // The instance a binding names, created on first use
    [[nodiscard]] auto instance_for(generic_fn& g, generic_binding binding)
        -> result<gsl::not_null<instance*>>;
    // A type as a monomorph's symbol spells it
    [[nodiscard]] auto mangle_type(const gs::type& t) -> std::string;
    // A function's symbol, under its ctor instance's prefix when it has one
    [[nodiscard]] auto
    member_symbol(const unit& u, decl_id decl, inst_id func, const member_context& member)
        -> result<std::string>;

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
    [[nodiscard]] auto symbol_for(const unit& u, decl_id decl, const source& src)
        -> result<std::string>;
    // `name` declared in `owner`'s scope of `scope`, qualified when another scope shares it
    [[nodiscard]] auto        scoped_symbol(const unit&           scope,
                                            stdx::option<inst_id> owner,
                                            std::string_view      name,
                                            const unit&           user,
                                            const source&         use) -> result<std::string>;
    [[nodiscard]] static auto linkage_of(const decl& d) noexcept -> gir::linkage;
    [[nodiscard]] static auto
    member_linkage(const unit& u, const decl& d, const member_context& member) -> gir::linkage;
    // Records that a declaration's value was given a GIR global
    auto note_storage(const unit& u, decl_id decl) -> void {
        decls_[u.index][std::to_underlying(decl)].has_storage = true;
    }
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
    // Resolves `t`'s fields the first time something needs them, detecting cycles
    [[nodiscard]] auto require_fields(const unit& u, const source& use, gs::type& t)
        -> result<void>;

    // A declaration of the compiler's `builtin` module, analyzed the first time `use` needs it
    [[nodiscard]] auto prelude_decl(std::string_view name, const unit& user, const source& use)
        -> result<operand>;
    [[nodiscard]] auto prelude_function(std::string_view name, const unit& user, const source& use)
        -> result<gsl::not_null<instance*>>;
    [[nodiscard]] auto prelude_type(std::string_view name, const unit& user, const source& use)
        -> result<gsl::not_null<gs::type*>>;

    [[nodiscard]] auto speculate() const -> speculation;
    // Drops the diagnostics, runtime need and, unless kept, unsupported stop recorded since `s`
    auto abandon(const speculation& s, bool keep_unsupported = false) -> void;

    // The `and` / `or` ops whose left side starts at `inst`, outermost first
    [[nodiscard]] auto bool_ops_starting_at(const unit& u, inst_id inst)
        -> gsl::span<const inst_id>;

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
    // A declaration's value, its analysis state reached through `state_of`
    // `setup` prepares the analyzer, binding what the declaration's namespace provides
    template <typename State, typename Setup>
    [[nodiscard]] auto
    resolve_decl(const unit& u, decl_id decl, const source& use, State&& state_of, Setup&& setup)
        -> result<operand>;
    // How `@typeName` names a type a ctor instance creates: `Box(u8)`
    [[nodiscard]] auto instance_display_name(const instance& call) -> std::string;
    // The impl's parameter values for `target`, none when the impl doesn't apply to it
    [[nodiscard]] auto impl_params_for(const impl_entry& entry, gs::type& target)
        -> result<stdx::option<std::vector<ip::index>>>;
    [[nodiscard]] auto impl_interface(const impl_entry& entry) -> result<stdx::option<gs::type&>>;
    [[nodiscard]] auto impl_symbol_prefix(const impl_entry& entry, gs::type& target) -> std::string;
    // An impl member declaration, analyzed once per target
    [[nodiscard]] auto analyze_impl_decl(impl_instance& applied, decl_id decl, const source& use)
        -> result<operand>;
    // Binds an impl's parameters and target in the analyzer of one of its members
    auto enter_impl(impl_instance& applied, analyzer& a) -> void;
    // What an impl's member functions see of their target
    [[nodiscard]] auto impl_context(impl_instance& applied) -> member_context;
    // The interface's default method `name` instantiated for an impl's target
    [[nodiscard]] auto default_method(impl_instance& applied, std::string_view name)
        -> result<stdx::option<operand>>;
    // The declarations of every module, module-level type and impl that may claim each name
    [[nodiscard]] auto declared_names(const unit& user, const source& use)
        -> result<gsl::not_null<scoped_names*>>;
    // Whether more than one declaration with storage carries `name`
    [[nodiscard]] auto collides(const std::string& name, const unit& user, const source& use)
        -> result<bool>;
    // The number a qualified symbol names its declaring scope by: a module, type or impl
    [[nodiscard]] auto        scope_number(const unit& u, stdx::option<inst_id> owner) -> u32;
    [[nodiscard]] static auto declares_function(const unit& u, const decl& d) -> bool;
    // A template's name for its instances, numbered after the first same-named template
    [[nodiscard]] auto versioned_name(const generic_fn& g) -> std::string;
    // The type a declaration's value body declares outright, if any
    [[nodiscard]] static auto declared_nominal(const unit& u, const decl& d)
        -> stdx::option<inst_id>;
    // What tells a type a compile-time call creates apart from the same call's other instances
    [[nodiscard]] static auto call_identity(const comptime_frame& frame) -> std::vector<ip::index>;
    auto                      analyze_roots() -> void;
    // Analyzes the member declarations of a type a module-level declaration declared
    auto analyze_members(const unit& u, gs::type& owner) -> void;
    // The type a declaration's value body declares directly
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
    gs::context&                         ctx_;
    const program&                       prog_;
    options                              opts_;
    std::vector<unit>                    units_;
    std::vector<std::vector<decl_state>> decls_;
    // Instances and nominals are referenced by address, so the arena keeps them in place
    ghoti::arena                            arena_;
    std::vector<gsl::not_null<instance*>>   instances_;
    std::vector<gsl::not_null<generic_fn*>> generics_;
    std::vector<gsl::not_null<impl_entry*>> impls_;
    ankerl::unordered_dense::map<impl_key, stdx::option<impl_instance&>, impl_key_hash>
        impl_instances_;
    ankerl::unordered_dense::map<instance_key, gsl::not_null<instance*>, instance_key_hash>
                                                                        instances_by_key_;
    ankerl::unordered_dense::map<std::string, gsl::not_null<instance*>> instances_by_symbol_;
    std::vector<gsl::not_null<instance*>>                               queue_;
    gir::module_t                                                       gir_;
    gs::diagnostics                                                     diagnostics_;
    stdx::option<sema::unsupported>                                     stopped_at_;
    u32                                                                 ptr_bits_;
    std::vector<comptime_alloc>                                         allocs_;
    std::vector<comptime_frame>                                         calls_;
    ankerl::unordered_dense::map<memo_key, memo_entry, memo_key_hash>   memo_;
    eval_limits                                                         limits_;
    stdx::option<runtime_site>                                          runtime_site_;
    u32                                                                 next_frame_{0};
    std::vector<gsl::not_null<nominal*>>                                nominals_;
    ankerl::unordered_dense::map<nominal_key, u32, nominal_key_hash>    nominal_ids_;
    // The `builtin` module, lowered here since the program's modules don't import it
    u32                   vtables_{0};
    stdx::option<lowered> prelude_;
    stdx::option<u32>     prelude_unit_;
    // Per unit, the `and` / `or` ops by the first inst of their left side, built on first use
    std::vector<stdx::option<ankerl::unordered_dense::map<inst_id, std::vector<inst_id>>>>
        bool_starts_;
    // Each GIR symbol a declaration claimed, with the unit and declaration that claimed it
    ankerl::unordered_dense::map<std::string, std::pair<u32, decl_id>> symbols_;
    stdx::option<scoped_names>                                         declared_names_;
    ankerl::unordered_dense::map<u64, u32>                             scope_numbers_;
    // The declarations whose symbols are being chosen, innermost last
    std::vector<std::pair<u32, decl_id>> naming_;
    ankerl::unordered_dense::map<std::string, std::vector<std::pair<u32, inst_id>>>
        generic_versions_;
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
    // Binds a comptime parameter of a runtime body, giving an aggregate a slot runtime reads use
    [[nodiscard]] auto bind_comptime_param(inst_id param, ip::index value) -> result<void>;
    // A module-level declaration's value, emitting its GIR global when it has one
    [[nodiscard]] auto analyze_decl(decl_id decl) -> result<operand>;
    // Resolves a nominal type's fields, or an enum's values
    [[nodiscard]] auto resolve_fields(nominal& n) -> result<void>;
    // Folds an attribute list into what codegen reads, checking each against `site`
    [[nodiscard]] auto fold_attributes(gsl::span<const inst_id> body, const attribute_site& site)
        -> result<gs::resolved_attributes>;

    // The comptime value of `op`, none when it is only known at runtime
    [[nodiscard]] auto comptime_value(const operand& op) const -> stdx::option<ip::index>;
    [[nodiscard]] auto type_of(const operand& op) -> gs::type&;
    [[nodiscard]] auto denoted_type(const operand& op, const source& src)
        -> result<gsl::not_null<gs::type*>>;
    // Binds a parameter of the signature this analyzer evaluates
    auto bind(inst_id param, operand value) -> void;
    // Analyzes one instruction of a signature body that isn't a parameter
    [[nodiscard]] auto prepare(inst_id inst) -> result<void>;
    // The operand a signature's instruction produced
    [[nodiscard]] auto value_of(ref r, const source& src) -> result<operand>;
    // Analyzes what follows as declared in `n`'s namespace, reading its captures
    auto enter(nominal& n) -> void;
    // Analyzes what follows reading `member`'s captures, in its type's namespace
    auto enter(const member_context& member) -> void;

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

    // A method's `self` argument, and the mutability of the object it came from
    struct receiver_arg {
        operand                              value;
        gs::types::mut::mutability_modifiers mut;
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
        // The instruction that computed the value, which an integer conversion may fold
        stdx::option<ref> from{};
    };

  private:
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
    [[nodiscard]] auto analyze_capture(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_closure(inst_id inst) -> result<operand>;
    // What a function this analyzer declares sees of the type around it
    [[nodiscard]] auto member_context_here() -> member_context;
    // Whether the declaration's value is a closure of `func`, which declares it instead
    [[nodiscard]] auto closure_declares(inst_id func) const -> bool;

    // Methods (members.cc)
    // The `self` argument a method takes from its object
    [[nodiscard]] auto receiver(ast::type_modifier::modifier how,
                                gs::type&                    self_type,
                                ref                          object_ref,
                                const source&                src) -> result<receiver_arg>;
    // The modifier a method's `self` parameter is written with, none for a plain function
    [[nodiscard]] auto self_modifier_of(const unit& u, inst_id func) const
        -> stdx::option<ast::type_modifier::modifier>;
    // The type whose methods an object's calls reach, through one pointer or reference
    [[nodiscard]] auto receiver_type(const operand& object) -> stdx::option<gs::type&>;
    // A `mut?` pointer or reference type resolved to one call's mutability
    [[nodiscard]] auto with_mutability(gs::type& type, gs::types::mut::mutability_modifiers mut)
        -> gs::type&;
    // The mutability of an object a method reaches through a pointer or reference
    [[nodiscard]] static auto place_mutability(const place& base, const place& object)
        -> gs::types::mut::mutability_modifiers;
    [[nodiscard]] auto call_method(gs::type&            owner,
                                   ref                  object_ref,
                                   std::string_view     name,
                                   gsl::span<const ref> args,
                                   const source&        src) -> result<operand>;
    // `^T`, `&T` and `[]T`
    [[nodiscard]] auto analyze_elem_type(inst_id inst, tag t) -> result<operand>;
    [[nodiscard]] auto analyze_array_type(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_fn_type(inst_id inst) -> result<operand>;
    // An array length, which must be known at compile time
    [[nodiscard]] auto array_length(ref len_ref, const source& src) -> result<usize>;

    // Nominal types (aggregates.cc)
    [[nodiscard]] auto analyze_nominal_decl(inst_id inst, gs::type_kind kind) -> result<operand>;
    [[nodiscard]] auto analyze_interface_decl(inst_id inst) -> result<operand>;

    // `dyn` (dyn.cc)
    [[nodiscard]] auto analyze_dyn_type(inst_id inst) -> result<operand>;
    // A pointer or reference to a type that implements the interface, as a `&dyn I` or `^dyn I`
    [[nodiscard]] auto coerce_to_dyn(const operand& op, gs::type& target, const source& src)
        -> result<stdx::option<operand>>;
    // A method call through a `dyn` object's vtable, none when the object isn't one
    [[nodiscard]] auto call_dyn(const operand&       object,
                                std::string_view     name,
                                gsl::span<const ref> args,
                                const source&        src) -> result<stdx::option<operand>>;
    // The comptime values a type declaration captures from the body around it
    [[nodiscard]] auto captured_values(inst_id inst, gs::type_kind kind)
        -> result<std::vector<ip::index>>;
    [[nodiscard]] auto resolve_struct(nominal& n) -> result<void>;
    [[nodiscard]] auto resolve_union(nominal& n) -> result<void>;
    [[nodiscard]] auto resolve_enum(nominal& n) -> result<void>;
    [[nodiscard]] auto field_type_of(gsl::span<const inst_id> body,
                                     std::string_view         name,
                                     const source& src) -> result<gsl::not_null<gs::type*>>;
    // The enum's variant `name` as a constant
    // The variant an enum constant names, none for any other constant
    [[nodiscard]] auto variant_name(ip::index value) -> stdx::option<std::string_view>;
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
    // An array field's type with immutable elements
    [[nodiscard]] auto immutable_view(gs::type& field) -> gs::type&;
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
    // The value of a pack at a compile-time index
    [[nodiscard]] auto pack_elem(pack_ref pack, ref at_ref, const source& src) -> result<operand>;
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
    // Whether a `break` or `continue` names `loop`
    [[nodiscard]] auto jumped_to(inst_id loop) const -> bool;
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
    // Builds a folded aggregate constant into runtime storage
    [[nodiscard]] auto materialize(ip::index index, const source& src) -> result<gir::value>;
    [[nodiscard]] auto materialize_slice(const ip::aggregate_value& agg,
                                         gs::type&                  type,
                                         const source&              src) -> result<gir::value>;

    // Runtime safety (safety.cc)
    [[nodiscard]] auto usize_constant(u64 value) -> gir::value;
    [[nodiscard]] auto string_slice(std::string_view text, gs::type& slice_type) -> gir::value;
    // A string constant viewed as a slice of `slice_type`, copied when the slice is mutable
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
    // Whether the runtime instruction for `op` on `t` checks for overflow
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
    // The array of type `from` at `address` viewed as a slice of its elements
    [[nodiscard]] auto array_as_slice(const gir::value& address, gs::type& from, gs::type& target)
        -> result<stdx::option<operand>>;
    // A known array as a compile-time slice of the same elements, none when the types disagree
    [[nodiscard]] auto comptime_slice_of(ip::index array, gs::type& from, gs::type& target)
        -> stdx::option<operand>;
    // The slice viewing all of the array at `address`
    [[nodiscard]] auto slice_of_array(const gir::value& address, gs::type& from)
        -> result<gir::value>;
    [[nodiscard]] auto as_runtime(const operand& op, const source& src) -> result<gir::value>;
    [[nodiscard]] auto materialize_merge(merge_ref m, gs::type& type, const source& src)
        -> result<gir::value>;
    // A `.{...}` or `.name` that takes its type from where it is used
    [[nodiscard]] static auto is_untyped_literal(const operand& op) -> bool {
        return op.is<anon_init>() || op.is<enum_literal>() || op.is<cast_ref>();
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
    // A declaration's value where it is used: a `let mut` global's place, or its value loaded
    [[nodiscard]] auto declaration_use(const operand& value, bool as_place, const source& src)
        -> result<operand>;
    // Modules (modules.cc)
    [[nodiscard]] auto analyze_import(inst_id inst) -> result<operand>;
    [[nodiscard]] auto module_member(const module_ref& m, std::string_view name, const source& src)
        -> result<operand>;
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
    // Makes the result slots of the `and` / `or` ops whose left side starts at `inst`
    auto start_bool_ops(inst_id inst) -> void;
    // Whether a branch body ends yielding nothing, like a statement `if`'s arms
    [[nodiscard]] auto yields_nothing(gsl::span<const inst_id> body) const -> bool;
    // A runtime condition's value when running it again at compile time knows it
    [[nodiscard]] auto folded_condition(ref cond, const source& src) -> result<stdx::option<bool>>;
    // What `r` computes when run again at compile time, keeping nothing that records, if computable
    [[nodiscard]] auto speculated_value(ref r, const source& src)
        -> result<stdx::option<ip::index>>;
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
    // Calls a function or generic `callee` evaluated to
    [[nodiscard]] auto call_operand(const operand&       callee,
                                    gsl::span<const ref> args,
                                    const source&        src,
                                    const source&        callee_src) -> result<operand>;
    // A call of `target`. `through_member` calls a type's function by its name: `T.f(x)`
    [[nodiscard]] auto call_function(instance&            target,
                                     gsl::span<const ref> args,
                                     const source&        src,
                                     const source&        callee_src,
                                     bool                 through_member) -> result<operand>;
    // A call of `target` on arguments that match its GIR parameters
    [[nodiscard]] auto
    call_with(instance& target, std::vector<operand> args, const source& src, bool through_member)
        -> result<operand>;

    // Generic calls (generics.cc)
    [[nodiscard]] auto call_generic(generic_fn&           g,
                                    std::vector<call_arg> args,
                                    const source&         src,
                                    const source&         callee_src) -> result<operand>;
    [[nodiscard]] auto bind_generic(generic_fn&               g,
                                    gsl::span<const call_arg> args,
                                    const source&             src) -> result<generic_binding>;
    // A comptime argument's value, reporting one only known at runtime
    [[nodiscard]] auto comptime_argument(const call_arg&         arg,
                                         stdx::option<gs::type&> declared,
                                         const use_site&         use,
                                         const source&           src) -> result<ip::index>;
    // The narrowest of `i32`, `i64`, `u64`, `i128` and `u128` holding a `comptime_int`
    [[nodiscard]] auto fitted_int_type(ip::index value) -> gs::type&;
    // The type an argument gives an `auto` parameter or return
    [[nodiscard]] auto inferred_type(const operand& arg) -> gs::type&;
    // A parameter type with `auto` behind pointers or references filled from the argument's
    [[nodiscard]] auto matched_type(gs::type& declared, const operand& arg, const source& src)
        -> result<gsl::not_null<gs::type*>>;
    // Reports an argument whose type doesn't implement every interface its parameter's bound names
    [[nodiscard]] auto check_bound(analyzer&         sig,
                                   const impl_bound& bound,
                                   gs::type&         type,
                                   const generic_fn& g,
                                   inst_id           param,
                                   const call_arg&   arg,
                                   const source&     src) -> result<void>;

    // Attributes (attributes.cc)
    [[nodiscard]] auto attribute_items(gsl::span<const inst_id> body)
        -> result<std::vector<attribute_item>>;
    [[nodiscard]] auto attribute_arg_src(ref arg, const source& fallback) const -> const source&;
    [[nodiscard]] auto attribute_value(ref arg, stdx::option<gs::type&> expect, const source& src)
        -> stdx::option<ip::index>;
    [[nodiscard]] auto attribute_bool(const attribute_item& item) -> bool;
    [[nodiscard]] auto attribute_variant(const attribute_item& item,
                                         ref                   arg,
                                         std::string_view enum_name) -> stdx::option<std::string>;
    [[nodiscard]] auto attribute_alignment(const attribute_item& item) -> stdx::option<u64>;
    [[nodiscard]] auto attribute_visibility(const attribute_item& item)
        -> stdx::option<ast::symbol_visibility>;
    [[nodiscard]] auto attribute_features(const attribute_item& item) -> std::vector<std::string>;
    auto               check_deprecation_message(const attribute_item& item) -> void;
    auto check_visibility_linkage(gsl::span<const attribute_item> items, const attribute_site& site)
        -> void;
    auto report_misplaced(const attribute_item& item, const attribute_site& site) -> void;

    // Builtins (builtins.cc)
    [[nodiscard]] auto analyze_builtin(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_type_of(inst_id inst) -> result<operand>;

    // Reflection (reflection.cc)
    [[nodiscard]] auto builtin_type_info(gsl::span<const ref> args, const source& src)
        -> result<operand>;
    [[nodiscard]] auto type_info(gs::type&                                    type,
                                 stdx::option<const gs::resolved_attributes&> declared,
                                 const source& src) -> result<ip::index>;
    [[nodiscard]] auto function_info(gs::type&                                    type,
                                     stdx::option<const gs::resolved_attributes&> declared,
                                     const source& src) -> result<ip::index>;
    [[nodiscard]] auto enum_info(gs::type& type, const source& src) -> result<ip::index>;
    [[nodiscard]] auto struct_info(gs::type& type, const source& src) -> result<ip::index>;
    [[nodiscard]] auto union_info(gs::type& type, const source& src) -> result<ip::index>;
    // A prelude struct `type_name` with the given fields set
    [[nodiscard]] auto info_struct(std::string_view            type_name,
                                   gsl::span<const info_field> fields,
                                   const source&               src) -> result<ip::index>;
    // `builtin.TypeInfo` with `tag` active, a void payload when none is given
    [[nodiscard]] auto info_union(std::string_view        tag,
                                  stdx::option<ip::index> payload,
                                  const source&           src) -> result<ip::index>;
    [[nodiscard]] auto info_variant(std::string_view enum_name,
                                    std::string_view variant,
                                    const source&    src) -> result<ip::index>;
    [[nodiscard]] auto info_slice(std::string_view       elem_name,
                                  std::vector<ip::index> elements,
                                  const source&          src) -> result<ip::index>;
    [[nodiscard]] auto info_name(std::string_view name) -> ip::index;
    [[nodiscard]] auto info_bool(bool value) -> ip::index;
    [[nodiscard]] auto info_int(u64 value, gs::type& type) -> ip::index;
    // Field `index`'s default folded at compile time, none when it has none or it doesn't fold
    [[nodiscard]] auto field_default_value(gs::type& type, usize index, const source& src)
        -> result<stdx::option<ip::index>>;
    [[nodiscard]] auto builtin_bit_size(gsl::span<const ref> args, const source& src)
        -> result<operand>;
    [[nodiscard]] auto builtin_type_name(gsl::span<const ref> args, const source& src)
        -> result<operand>;
    [[nodiscard]] auto builtin_tag_name(gsl::span<const ref> args, const source& src)
        -> result<operand>;
    // The value `body` yields, analyzed without emitting anything that would run
    [[nodiscard]] auto unevaluated(gsl::span<const inst_id> body, const source& src)
        -> result<operand>;
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
    // Casts and conversions (casts.cc)
    [[nodiscard]] auto builtin_cast(inst_id              inst,
                                    syntax::builtin_t    which,
                                    gsl::span<const ref> args) -> result<operand>;
    // A one-argument cast given its target, or the cast's own fallback when none is known
    [[nodiscard]] auto cast_target_inferred(cast_ref cast, stdx::option<gs::type&> target)
        -> result<operand>;
    [[nodiscard]] auto
    apply_cast(inst_id inst, syntax::builtin_t which, gs::type& target, ref operand_ref)
        -> result<operand>;
    // Whether a runtime body folds the cast of a known operand, which the old emitter evaluated
    [[nodiscard]] static auto folds_whole(syntax::builtin_t which,
                                          const gs::type&   from,
                                          const gs::type&   target) noexcept -> bool;
    [[nodiscard]] auto        fold_cast(syntax::builtin_t which,
                                        ip::index         index,
                                        const gs::type&   from,
                                        gs::type&         target,
                                        const source&     src,
                                        const source&     operand_src) -> result<operand>;
    [[nodiscard]] auto        unary_cast(inst_id inst, syntax::builtin_t which, ref operand_ref)
        -> result<operand>;
    [[nodiscard]] auto slice_from_ptr(inst_id inst, ref ptr_ref, ref len_ref) -> result<operand>;
    // A cast's operand, read through a reference
    [[nodiscard]] auto cast_operand(ref operand_ref, const source& src) -> result<operand>;
    [[nodiscard]] auto
    cast_value(gir::instruction_kind kind, const gir::value& from, gs::type& target) -> operand;
    [[nodiscard]] auto
    check_bit_cast(const gs::type& from, const gs::type& target, const source& src) -> result<void>;
    // Panics unless the integer fits the target, where the cast could lose it
    [[nodiscard]] auto int_cast_guard(const gir::value& value,
                                      const gs::type&   target,
                                      const source&     src) -> result<void>;
    // Panics unless an integer cast to an enum landed on one of its variants
    [[nodiscard]] auto
    enum_cast_guard(const operand& cast, const gir::value& from, ref operand_ref, const source& src)
        -> result<void>;
    // Where a cast's operand is written in the call
    [[nodiscard]] auto cast_operand_src(inst_id call, ref operand_ref) const -> source;
    [[nodiscard]] auto builtin_this(const source& src) -> result<operand>;
    // Resolves what `@sizeOf(type)` needs, reporting a layout that depends on itself
    [[nodiscard]] auto layout_ready(gs::type& type, const source& src) -> result<void>;

    // Comptime knownness
    // The operand's comptime value, or the runtime need when it has none
    [[nodiscard]] auto require_comptime(const operand& op, const source& src) -> result<ip::index>;
    // `r` evaluated at compile time, running its instructions again when they were runtime
    [[nodiscard]] auto reevaluate(ref r, const source& src) -> result<operand>;

    // Declarations (decls.cc)
    // Whether the declaration's value is a type it declares: `const P = struct { ... }`
    [[nodiscard]] auto declares_type(const decl& d) const -> bool;
    // A module-level or member declaration's GIR symbol
    [[nodiscard]] auto global_symbol(decl_id decl, const source& src) -> result<std::string>;
    [[nodiscard]] auto analyze_global(decl_id                        decl,
                                      const operand&                 value,
                                      stdx::option<gs::type&>        declared,
                                      const gs::resolved_attributes& attrs,
                                      const source&                  src) -> result<operand>;
    // Folds a module-level declaration's own attributes once its value is known
    [[nodiscard]] auto decl_attributes(const decl& d, const operand& value)
        -> result<gs::resolved_attributes>;

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
    std::vector<target>                               targets_;
    std::vector<pending_merge>                        merges_;
    std::vector<std::vector<operand>>                 packs_;
    // Nonzero inside a comptime body of a runtime function, where nothing may emit GIR
    usize comptime_depth_{0};
    // The values `capture` instructions read, and the type whose namespace holds the body
    std::vector<ip::index>  captures_;
    stdx::option<nominal&>  scope_;
    stdx::option<gs::type&> self_type_;
    std::string             symbol_prefix_;
    u32                     frame_;
    u32                     runtime_depth_{0};
    u32                     alloc_base_;
    stdx::option<jump>      jump_;
};

// Diagnostic messages shared across checks (messages.cc)
namespace messages {

constexpr std::string_view MUTABLE_TYPE_BINDING{
    "a 'type' value cannot be stored in a 'let' or 'let mut' binding; use 'const' instead"};
constexpr std::string_view MUTABLE_ADDRESS_OF_IMMUTABLE{
    "Cannot take a mutable address of an immutable value"};
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

// What an array or slice holds
[[nodiscard]] auto element_type_of(gs::type& t) -> stdx::option<gs::type&>;

// Whether `d`'s value is the function literal `func` itself
[[nodiscard]] auto declares_literal(const module_t& m, const decl& d, inst_id func) -> bool;

// An element as a container of `constant` mutability holds it. A pointer, reference or slice keeps
// its own mutability, which is its pointee's
[[nodiscard]] auto element_view(gs::context& ctx, gs::type& elem, bool constant) -> gs::type&;

// Whether an array of references `from` is the array of pointers `to` with no conversion
[[nodiscard]] auto decays_to(const gs::type& from, const gs::type& to) -> bool;

// Whether a value of `type` holds a `type` somewhere, which only exists at compile time
[[nodiscard]] auto holds_types(const gs::type& type) -> bool;

// The `param` instructions of a function's parameter body, which also holds what its `impl`
// bounds name
[[nodiscard]] auto params_of(const module_t& m, gsl::span<const inst_id> body)
    -> std::vector<inst_id>;

// Whether a binding's value lives on past one use, so its spill is reused
[[nodiscard]] constexpr auto is_binding(tag t) noexcept -> bool {
    return t == tag::param || t == tag::let_value || t == tag::bind;
}
// An integer constant's value, none for any other kind of constant
[[nodiscard]] auto int_of(const ip::value& v) -> stdx::option<big_int>;

} // namespace ghoti::uir::sema
