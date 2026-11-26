#pragma once

#include <algorithm>
#include <array>
#include <functional>
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
#include <fmt/format.h>

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
#include "compiler/syntax/builtins.hh"
#include "compiler/uir/astgen.hh"
#include "compiler/uir/sema/sema.hh"
#include "compiler/uir/uir.hh"
#include "support/big_int.hh"
#include "support/diagnostic.hh"
#include "support/float128.hh"
#include "support/float_math.hh"
#include "support/int128.hh"

namespace ghoti::uir::sema {

namespace gs = ::ghoti::sema;

// Why an analysis step stopped
enum class stop : u8 {
    POISON,      // A reported error
    UNSUPPORTED, // A construct this phase doesn't handle
    RUNTIME,     // A comptime evaluation needed a runtime value, which its boundary reports
    SKIP,        // A comptime `@skip`, which its boundary turns into the test's skip
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

// Where a bit-packed field sits in its aggregate's `n`-bit backing integer
struct packed_bits {
    u32 n;
    u32 offset;
    u32 fbits;
    // A field of a packed field, which reads and writes go through, else the backing's address
    stdx::option<u32> parent{};
};

// The known value a temporary of one function was built from
struct built_temp {
    const gir::function* fn;
    ip::index            value;
};

// A compile-time `MemoryOrder` or `AtomicRmwOp` argument, by name and value
struct atomic_choice {
    std::string name;
    u8          value;
    source      at;
};

// A vector's lane, read from and written into the vector loaded where the lane was named
struct vector_lane_at {
    gir::value index;
    gir::value vector;
};

// An addressable slot that loads read and stores write
struct place {
    gir::value                address;
    gsl::not_null<gs::type*>  pointee;
    bool                      is_mutable;
    stdx::option<union_field> pending{};
    // A by-value parameter's own copy, whose array fields read with mutable elements
    bool owned_copy{false};
    // A bit-packed field, read and written as bits of its aggregate
    stdx::option<packed_bits> packed{};
    // A vector's lane, read and written through its whole vector
    stdx::option<vector_lane_at> lane{};
};

// The index of the field `name` among `names`, none when there is no such field
[[nodiscard]] inline auto index_of(gsl::span<const std::string_view> names, std::string_view name)
    -> stdx::option<usize> {
    const auto found{std::ranges::find(names, name)};
    if (found == names.end()) { return stdx::none; }
    return static_cast<usize>(found - names.begin());
}

// A typed or untyped integer
[[nodiscard]] inline auto is_int_like(const gs::type& t) noexcept -> bool {
    return gs::is_integer(t.get_kind()) || t.get_kind() == gs::type_kind::COMPTIME_INT;
}

[[nodiscard]] inline auto is_bool(const gs::type& t) noexcept -> bool {
    return t.get_kind() == gs::type_kind::BOOL;
}

// A `packed struct` or `packed union` laid out as one backing integer, not a C one
[[nodiscard]] inline auto is_bit_packed(const gs::type& t) -> bool {
    if (const auto st{t.get_data().as_opt<gs::types::struct_t>()}) { return st->is_bit_packed(); }
    if (const auto ut{t.get_data().as_opt<gs::types::union_t>()}) { return ut->is_bit_packed(); }
    return false;
}

// The values a `for` range's counter takes, which let it convert to any integer holding them all
struct value_range {
    big_int lo;
    big_int hi;
};

// A `dyn` call's receiver data and the vtable entry it calls
struct dyn_entry {
    gir::value data;
    gir::value entry;
};

// A runtime merge whose arms are all untyped constants, given a type where it is used
struct merge_ref {
    u32 index;
};

// A module-level `let mut`, which each use reaches through its GIR global's address
struct global_ref {
    std::string_view         symbol;
    gsl::not_null<gs::type*> type;
};

// One step from a comptime value into one of its elements or fields
struct comptime_step {
    u32                      index;
    gsl::not_null<gs::type*> type;
};

// A comptime allocation's slot, a `comptime let mut` or a `let mut` in a comptime body, or an
// element or field within it
struct comptime_place {
    u32                        alloc;
    std::vector<comptime_step> path{};
    // Reached through a compile-time reference, which any analysis may write through
    bool borrowed{false};
};

// A run of a compile-time array's elements, which a slice of it views
struct memory_view {
    comptime_place           array;
    u64                      offset;
    u64                      len;
    gsl::not_null<gs::type*> elem;
    bool                     is_mutable;
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
    // An immutable binding of a constant global's value
    bool global_value{false};
    // The known value of a binding built at runtime, which only vector conversions read
    stdx::option<ip::index> built{};
};

// An untyped `.{ ... }` literal, which takes the type it is coerced to
struct anon_init {
    inst_id inst;
};

// An untyped `.name`, which names a variant of the enum it is coerced to
struct enum_literal {
    inst_id inst;
    // The `ip::enum_value` holding its name, when it was bound to an untyped comptime parameter
    stdx::option<ip::index> named{};
};

// `.name(args)`, a call of the member of the type it is coerced to
struct implicit_call {
    inst_id inst;
};

// A module-level aggregate `const`, which runtime code reads through its GIR global
struct global_const {
    std::string_view         symbol;
    gsl::not_null<gs::type*> type;
    ip::index                value;
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

// `^.{ ... }`, an array literal whose element type comes from the slice it is used as
struct slice_literal {
    inst_id init;
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
                              pack_ref,       // a pack parameter's values
                              implicit_call,  // a `.name(args)` waiting for its type
                              slice_literal>; // a `^.{ ... }` waiting for its slice type

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
    // A module-level `comptime let mut`, kept when the analysis that made it ends
    bool lasting{false};
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
    decl_status           status{decl_status::UNRESOLVED};
    stdx::option<operand> value{};
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

// A closure's environment, and the field each capture slot reads
struct closure_env {
    gsl::not_null<gs::type*>       type;
    std::vector<stdx::option<u32>> fields;
    // A captured pack's slot and its length, its elements the fields from the slot's on
    std::vector<std::pair<u32, u32>> pack_slots{};
    // A `move fn` owns its copies
    bool moves{false};
    // The environment parameter's name, and the capture slot naming the function itself
    std::string_view  self_name{"self"};
    stdx::option<u32> self_slot{};
    // A local generic's binding type, a function type holding this environment
    stdx::option<gs::type&> value_type{};
    // The number a generic value's type is spelled with in a monomorph's symbol
    u32 id{0};
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
    // A closure's body takes its environment first
    stdx::option<closure_env&> env{};
    // A compile-time closure's borrowed places by capture slot, and the frame that made them
    std::vector<stdx::option<operand>> comptime_env{};
    stdx::option<u32>                  env_frame{};
    // The capture slot of a local function's own name
    stdx::option<u32> self_capture{};
    // Packs captured only for their length, by slot, as `undefined` values of their types
    std::vector<std::pair<u32, std::vector<ip::index>>> known_packs{};
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
    // A local generic's captures, which every instance takes first
    stdx::option<closure_env&> env{};
    // What its instances' symbols end with, for a local generic of a generic's instance
    std::string suffix{};
    // A compile-time closure's borrowed places, which its instances share
    std::vector<stdx::option<operand>> comptime_env{};
    stdx::option<u32>                  env_frame{};
    // The type its value has, holding its environment when it captures, and its number there
    stdx::option<gs::type&> value_type{};
    u32                     value_id{0};
    // The attributes no parameter decides, once `@typeInfo` asks
    stdx::option<gs::resolved_attributes> attributes{};
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

// A capture a compile-time function value folds in
struct named_capture {
    std::string_view name;
    ip::index        value;
};

// A function literal a runtime body declares
struct local_decl {
    inst_id                                 inst;
    inst_id                                 func;
    member_context                          member;
    std::string_view                        name{};
    std::vector<gs::types::closure_capture> env_captures{};
    std::vector<stdx::option<u32>>          fields{};
    std::vector<std::pair<u32, u32>>        pack_slots{};
    stdx::option<u32>                       self_slot{};
    std::vector<std::pair<u32, std::vector<ip::index>>> known_packs{};
    // A capturing generic's type as written, for its environment
    stdx::option<gs::type&> signature{};
};

// A function literal a runtime body declares, by its instruction, enclosing function and captures
struct local_fn {
    u32                    unit_index;
    inst_id                inst;
    const instance*        enclosing;
    std::vector<ip::index> captures;
    operand                fn;
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
    // Built by `@Struct`, `@Union`, `@Enum` or `||`, so `inst` is no aggregate declaration
    bool synthesized{false};
    // A built struct's field defaults, by field
    std::vector<stdx::option<ip::index>> synthetic_defaults{};
};

// One field of a struct or union a builtin builds
struct synthetic_field {
    std::string              name;
    gsl::not_null<gs::type*> type;
    u64                      alignment{0};
    bool                     is_public{true};
    // The value a `@Struct` descriptor's `default_value` points at
    stdx::option<ip::index> default_value{};
};

// How a `@Union` descriptor lays its union out
struct synthetic_union {
    bool tagged;
    bool is_extern;
    bool is_packed;
    // The enum a union tagged by its own enum takes its tag from
    stdx::option<gs::type&> tag_enum;
};

// Where a struct field's default comes from: a body its declaration gives, or a known value
struct default_source {
    gsl::span<const inst_id> body{};
    stdx::option<ip::index>  known{};
    [[nodiscard]] auto       present() const noexcept -> bool { return !body.empty() || known; }
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
    // The closure over the interface's associated items a method that reads them is lowered to
    stdx::option<inst_id> closure{};
    gsl::span<const inst_id> attrs{};
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

// The two enums an `A || B` merges, in order
struct merge_key {
    gsl::not_null<const gs::type*> lhs;
    gsl::not_null<const gs::type*> rhs;

    [[nodiscard]] auto operator==(const merge_key&) const noexcept -> bool = default;
};

struct merge_key_hash {
    using is_avalanching = void;
    [[nodiscard]] auto operator()(const merge_key& key) const noexcept -> u64;
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

// A function `@export` defines under another symbol
struct function_export {
    gsl::not_null<instance*> fn;
    std::string              name;
    bool                     weak{false};
    ast::symbol_visibility   visibility{ast::symbol_visibility::DEFAULT};
    source_location          site;
    // Exported by the program's entry code, whose C `main` stands in for the program's
    bool from_start{false};
};

class compilation {
  public:
    // What a speculative compile-time evaluation could leave behind, so it can be undone
    struct speculation {
        usize        diagnostics;
        usize        memo;
        bool         stopped;
        bool         runtime_site;
        compilation& owner;

        speculation(compilation& comp) noexcept;
        ~speculation();
        MAKE_PINNED(speculation);
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

    // Keeps an aggregate's AST items with its `@cfg` groups spliced in, for the type viewing them
    template <typename Item>
    [[nodiscard]] auto keep_items(std::vector<Item> items) -> gsl::span<const Item> {
        return *arena_.make<std::vector<Item>>(std::move(items));
    }

    // The declaration's value, analyzing it on first use.
    // A use during its own analysis is a cycle reported at `use`
    [[nodiscard]] auto analyze_decl(const unit& u, decl_id decl, const source& use)
        -> result<operand>;
    // Puts the function's body on the work queue
    auto request_body(instance& fn) -> void;
    // The function a runtime body declared for `inst`, if it already did
    [[nodiscard]] auto local_function_of(const unit&                u,
                                         inst_id                    inst,
                                         const instance*            enclosing,
                                         gsl::span<const ip::index> captures)
        -> stdx::option<operand>;
    // Declares a function literal of a runtime body, with an environment when it captures
    [[nodiscard]] auto declare_local(const unit& u, const instance& enclosing, local_decl decl)
        -> result<operand>;
    // A compile-time closure, which borrows the places in `env` from the analysis `frame`
    [[nodiscard]] auto declare_comptime_closure(const unit&                        u,
                                                const instance&                    enclosing,
                                                inst_id                            func,
                                                member_context                     member,
                                                std::vector<stdx::option<operand>> env,
                                                u32 frame) -> result<operand>;
    // The body of the closure `type` is, if it is one
    [[nodiscard]] auto closure_body(const gs::type& type) -> stdx::option<instance&>;
    // A function's value at compile time, a copy of it with `captures` folded in
    [[nodiscard]] auto comptime_function(const instance&                fn,
                                         gsl::span<const named_capture> captures)
        -> result<ip::index>;
    // Leaves a just-declared function's body off the queue until a runtime use needs it
    auto defer_body(instance& fn) -> void;
    using function_names = ankerl::unordered_dense::map<std::string, gsl::not_null<instance*>>;
    // The function a compile-time value of a symbol in `names` names
    [[nodiscard]] auto named_function(const function_names& names, ip::index value)
        -> stdx::option<instance&>;
    // The function a compile-time function value calls, a copy or the function itself
    [[nodiscard]] auto const_function(ip::index value) -> stdx::option<instance&>;
    // A function's own value at compile time, its symbol
    [[nodiscard]] auto function_value(instance& fn) -> ip::index;
    // The function a compile-time value of its own symbol names
    [[nodiscard]] auto plain_function(ip::index value) -> stdx::option<instance&>;
    // How a monomorph's symbol spells a compile-time argument
    [[nodiscard]] auto mangle_value(ip::index value) -> std::string;
    // The generic local function whose captures `type` holds
    [[nodiscard]] auto closure_generic(const gs::type& type) -> stdx::option<generic_fn&>;
    // The type a generic function's value has, a function type holding what it captures
    [[nodiscard]] auto generic_value_type(generic_fn& g) -> result<gsl::not_null<gs::type*>>;
    // A fresh function type of `signature` that `g`'s values have
    [[nodiscard]] auto new_generic_value(generic_fn& g, gs::type& signature) -> gs::type&;
    // `signature` taking an environment of `env_type` by `&mut` first
    [[nodiscard]] auto with_environment(gs::type& signature, gs::type& env_type) -> gs::type&;

    // An untyped math result's function and input, which a concrete float computes again
    auto note_math_origin(ip::index value, math_origin origin) -> void {
        math_origins_.insert_or_assign(std::to_underlying(value), origin);
    }
    [[nodiscard]] auto math_origin_of(ip::index value) const -> stdx::option<math_origin> {
        const auto found{math_origins_.find(std::to_underlying(value))};
        if (found == math_origins_.end()) { return stdx::none; }
        return found->second;
    }

    auto note_literal_digits(ip::index value, stdx::option<std::array<f128, 4>> narrowed) -> void {
        if (!narrowed) {
            literal_digits_.erase(std::to_underlying(value));
            return;
        }
        literal_digits_.insert_or_assign(std::to_underlying(value), *narrowed);
    }

    [[nodiscard]] auto literal_digits_in(ip::index value, float_format format) const
        -> stdx::option<f128> {
        const auto found{literal_digits_.find(std::to_underlying(value))};
        if (found == literal_digits_.end() || format == float_format::QUAD) { return stdx::none; }
        return found->second.at(std::to_underlying(format));
    }

    // A compile-time `@skip` unwinds to its boundary, which takes its message
    auto skip(std::string message) -> stop {
        pending_skip_ = std::move(message);
        return stop::SKIP;
    }
    [[nodiscard]] auto take_skip() -> std::string { return std::exchange(pending_skip_, {}); }

    // An inferred return type that an untyped `return` leaves to the peer of every `return`
    [[nodiscard]] auto infer_peer_return(instance& fn) -> result<void>;
    // Analyzes a body whose return type is inferred now, since a caller needs that type
    [[nodiscard]] auto infer_return(instance& fn, const unit& caller, const source& use)
        -> result<void>;
    // The function type a bodyless literal written as a value spells
    [[nodiscard]] auto function_type_of(const unit& u, inst_id func, const member_context& member)
        -> result<operand>;
    // Declares the function `decl`'s `func` value creates, leaving its body for the queue
    [[nodiscard]] auto
    declare_function(const unit& u, decl_id decl, inst_id func, const member_context& member = {})
        -> result<operand>;
    // Entry (entry.cc)
    // Records an `@export`, reporting a symbol already exported by another function
    [[nodiscard]] auto add_export(const unit& u, function_export exported, const source& src)
        -> result<void>;
    // Gives each exported function its symbol and aliases
    auto apply_exports() -> void;
    // The program's root module and its entry code, when it is attached
    [[nodiscard]] auto root_unit() const -> const unit&;
    [[nodiscard]] auto start_unit() const -> stdx::option<const unit&>;
    [[nodiscard]] auto units() const noexcept -> gsl::span<const unit> { return units_; }
    // A module-level declaration of `u` named `name`, analyzed
    [[nodiscard]] auto named_decl(const unit& u, std::string_view name, const source& use)
        -> stdx::option<result<operand>>;
    // Whether a call to `fn` may drop its result
    [[nodiscard]] auto is_discardable(instance& fn) -> bool;
    // Per unit, whether the test build runs its tests
    [[nodiscard]] auto test_units() const -> std::vector<bool>;
    // Analyzes a `test` block into a test function
    auto analyze_test(const unit& u, decl_id decl) -> void;

    // Modules (modules.cc)
    // The module an import of `u` loads, reached through the name `name`
    [[nodiscard]] auto
    import_target(const unit& u, import_id import, std::string_view name, const source& src)
        -> result<operand>;
    // The module-level declaration `name` of `m`
    [[nodiscard]] auto
    module_member(const module_ref& m, std::string_view name, const unit& user, const source& src)
        -> result<operand>;
    [[nodiscard]] auto prelude_module() -> stdx::option<module_ref>;
    // Whether a compile-time call is running
    [[nodiscard]] auto in_comptime_call() const noexcept -> bool { return !calls_.empty(); }
    [[nodiscard]] auto is_prelude(const unit& u) const noexcept -> bool {
        return prelude_unit_ && u.index == *prelude_unit_;
    }

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
    // The interface's wrapper a call of `name` with `written` arguments goes to, else `name`
    [[nodiscard]] auto
    defaulted_method(const nominal& iface, std::string_view name, usize written) -> std::string;
    // The vtable of an impl's interface for its target, made and its methods requested on first use
    [[nodiscard]] auto vtable_for(impl_instance& applied) -> result<std::string_view>;
    // Whether an interface declares associated types or constants
    [[nodiscard]] static auto declares_associated_items(const nominal& iface) -> bool;
    // Whether every associated item of an interface has a default
    [[nodiscard]] static auto defaults_every_associated_item(const nominal& iface) -> bool;
    // An associated item's value: the impl's own, else the interface's default
    [[nodiscard]] auto associated_value(const nominal&               iface,
                                        stdx::option<impl_instance&> applied,
                                        std::string_view             name,
                                        const source&                use) -> result<ip::index>;
    // The values a method's closure over the interface's associated items captures
    [[nodiscard]] auto associated_captures(const nominal&               iface,
                                           inst_id                      closure,
                                           stdx::option<impl_instance&> applied,
                                           const source& use) -> result<std::vector<ip::index>>;
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

    // The GIR symbol of a module-level declaration, a function's or a global's
    [[nodiscard]] auto symbol_for(const unit& u, decl_id decl, const source& src, bool function)
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
    // A fresh name for a hidden global holding a literal's folded value
    [[nodiscard]] auto literal_global_name(std::string_view kind) -> std::string {
        return fmt::format(".{}.{}", kind, literal_globals_++);
    }
    // Records that a declaration's value was given a GIR global
    auto note_storage(const unit& u, decl_id decl) -> void {
        decls_[u.index][std::to_underlying(decl)].has_storage = true;
    }
    [[nodiscard]] auto has_storage(const unit& u, decl_id decl) const -> bool {
        return decls_[u.index][std::to_underlying(decl)].has_storage;
    }

    // Runs `fn` at compile time on interned arguments, memoized when the call is pure
    [[nodiscard]] auto
    call_comptime(const unit& caller, const source& src, instance& fn, std::vector<ip::index> args)
        -> result<operand>;
    [[nodiscard]] auto limits() noexcept -> eval_limits& { return limits_; }

    // Comptime memory
    [[nodiscard]] auto new_alloc(const comptime_alloc& alloc) -> comptime_place;
    [[nodiscard]] auto alloc_at(const comptime_place& p) -> comptime_alloc& {
        return allocs_[p.alloc];
    }
    [[nodiscard]] auto alloc_count() const noexcept -> u32 {
        return static_cast<u32>(allocs_.size());
    }
    // Drops the allocations from `base` on, once the analysis that made them ends
    auto release_allocs(u32 base) -> void {
        while (allocs_.size() > base && !allocs_.back().lasting) { allocs_.pop_back(); }
    }
    // A store makes impure every call in progress that didn't make the allocation
    auto               note_store(const comptime_place& p) -> void;
    [[nodiscard]] auto next_frame() noexcept -> u32 { return next_frame_++; }

    // The struct, union or enum `inst` declares, created the first time the current comptime call
    // reaches it
    [[nodiscard]] auto declare_nominal(const unit&            u,
                                       inst_id                inst,
                                       gs::type_kind          kind,
                                       std::vector<ip::index> identity = {}) -> nominal&;
    // An enum with no declaration in any AST, its variants given here
    auto synthesize_enum(nominal&                                  n,
                         gsl::span<const std::string>              names,
                         std::vector<i64>                          values,
                         gs::type&                                 underlying,
                         bool                                      non_exhaustive,
                         gsl::span<const gsl::not_null<gs::type*>> merged_from) -> void;
    // An `extern` function's declaration, called by its link name
    [[nodiscard]] auto declare_extern(const unit& u, decl_id decl, gs::type& type)
        -> result<operand>;
    auto synthesize_struct(nominal&                         n,
                           gsl::span<const synthetic_field> fields,
                           bool                             is_extern,
                           bool                             is_packed) -> void;
    auto synthesize_union(nominal& n, gsl::span<const synthetic_field> fields, synthetic_union shape)
        -> void;
    // `A || B`, one enum per pair of enums however many merges name it
    [[nodiscard]] auto merged_enum(const merge_key& key) -> stdx::option<gs::type&>;
    auto               remember_merge(const merge_key& key, gs::type& merged) -> void;
    [[nodiscard]] auto nominal_of(const gs::type& t) -> stdx::option<nominal&>;
    // A nominal's number, unique across modules, for its mangled name
    [[nodiscard]] static auto nominal_number(const gs::type& t) -> usize;
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

    [[nodiscard]] auto speculate() -> speculation;
    // Whether a speculative attempt is in progress, which never builds a compile-time closure
    [[nodiscard]] auto speculating() const noexcept -> bool { return speculating_ > 0; }
    // Drops the value errors reported since `from`, which an arm that never runs can't have
    [[nodiscard]] auto diagnostic_count() const noexcept -> usize { return diagnostics_.size(); }
    auto drop_value_errors(usize from) -> bool;
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
    [[nodiscard]] auto
    collides(const std::string& name, const unit& user, const source& use, bool function)
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
    ghoti::arena                          arena_;
    std::vector<gsl::not_null<instance*>> instances_;
    std::vector<local_fn>                 local_fns_;
    ankerl::unordered_dense::map<gsl::not_null<const gs::type*>, gsl::not_null<instance*>>
        closure_bodies_;
    ankerl::unordered_dense::map<gsl::not_null<const gs::type*>, gsl::not_null<generic_fn*>>
                                            closure_generics_;
    function_names                          const_functions_;
    function_names                          plain_functions_;
    u32                                     local_count_{0};
    u32                                     speculating_{0};
    ankerl::unordered_dense::map<u32, math_origin> math_origins_;
    ankerl::unordered_dense::map<u32, std::array<f128, 4>> literal_digits_;
    std::string                                            pending_skip_;
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
    u32                                                                 literal_globals_{0};
    std::vector<gsl::not_null<nominal*>>                                nominals_;
    ankerl::unordered_dense::map<merge_key, gsl::not_null<gs::type*>, merge_key_hash> merges_;
    ankerl::unordered_dense::map<nominal_key, u32, nominal_key_hash>                  nominal_ids_;
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
    std::vector<function_export> exports_;
    u32                          tests_{0};
    u32                          anonymous_tests_{0};
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
struct unrolled_loop {
    inst_id inst;
    u32     runtime_depth;
    usize   targets;
};

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
    // `body` analyzed into a function no one emits, so nothing in it runs
    [[nodiscard]] auto analyze_unemitted(gsl::span<const inst_id> body) -> result<flow>;
    // The top-level statement this analysis checks, which a function literal in it is declared by
    auto checking_statement(decl_id decl) -> void { statement_.emplace(decl); }
    // A `test` block's body, which passes when it reaches its end
    [[nodiscard]] auto analyze_test(gsl::span<const inst_id> body) -> result<void>;
    // Runs the function's body at compile time on arguments of its parameter types
    [[nodiscard]] auto eval_call(gsl::span<const ip::index> args) -> result<operand>;
    // A type body's denoted type
    // A local generic's type as written, whatever a call binds left `auto`
    [[nodiscard]] auto local_generic_signature(inst_id func, gsl::span<const ref> captured)
        -> result<gsl::not_null<gs::type*>>;
    [[nodiscard]] auto eval_type(gsl::span<const inst_id> body, const source& src)
        -> result<gsl::not_null<gs::type*>>;
    // Analyzes the function's parameters and body into its GIR function
    [[nodiscard]] auto analyze_function() -> result<void>;
    // Analyzes a body only to collect the types its `return`s give, for an inferred return type
    [[nodiscard]] auto collect_returns() -> std::vector<gsl::not_null<gs::type*>>;
    // Binds a comptime parameter of a runtime body, giving an aggregate a slot runtime reads use
    [[nodiscard]] auto bind_comptime_param(inst_id param, ip::index value) -> result<void>;
    // A module-level declaration's value, emitting its GIR global when it has one
    [[nodiscard]] auto analyze_decl(decl_id decl) -> result<operand>;
    // A module-level `comptime let mut`, a compile-time allocation of its own
    [[nodiscard]] auto comptime_global(const decl& d, const source& src) -> result<operand>;
    // The alignment a local's attributes give its slot
    [[nodiscard]] auto local_alignment(gsl::span<const inst_id> attrs)
        -> result<stdx::option<u64>>;
    // A global the linker provides, declared with its type alone
    [[nodiscard]] auto declare_extern_global(decl_id decl, gs::type& type) -> result<operand>;
    // Resolves a nominal type's fields, or an enum's values
    [[nodiscard]] auto resolve_fields(nominal& n) -> result<void>;
    // Folds an attribute list into what codegen reads, checking each against `site`
    [[nodiscard]] auto fold_attributes(gsl::span<const inst_id> body, const attribute_site& site)
        -> result<gs::resolved_attributes>;
    // The `@branchHint` a body starts with, looking into a leading block
    [[nodiscard]] auto hint_inst_of(gsl::span<const inst_id> body) const -> stdx::option<inst_id>;
    [[nodiscard]] auto branch_hint_of(gsl::span<const inst_id> body) const
        -> stdx::option<ast::branch_hint>;

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

    // An operator applied to an untyped merge, run once the merge takes its type
    struct merge_op {
        binary_info info;
        operand     other;
        bool        merge_left;
    };

    // Arms that all carry untyped constants, waiting for a use to type them
    struct pending_merge {
        gir::segment_id          seg;
        std::vector<incoming>    arrivals;
        stdx::option<gir::value> materialized;
        std::vector<merge_op>    ops{};
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
        gsl::span<const ref> patterns;
        // Holds the `bind` of each name the patterns capture, then the arm's own capture
        gsl::span<const inst_id> pattern_body;
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
        // A `&v` or `^v` capture holds the address of what it names
        bool is_alias;
    };

    // Where a `&v` or `^v` capture's value is reached
    struct capture_access {
        bool is_const;
        // A temporary's copy can't be captured by mutable reference
        bool is_addressable;
        // Reached through a `mut?` reference
        bool is_poly;
    };

    // Where a runtime `match` takes its scrutinee apart
    struct match_root {
        gir::value     address;
        capture_access access;
    };

    // Compile-time memory a pattern takes apart, which its `&v` and `^v` captures alias
    struct comptime_alias {
        comptime_place place;
        capture_access access;
    };

    // The names a `for` body binds and how, by the iterable each comes from
    struct for_captures {
        gsl::span<const stdx::option<inst_id>> binds;
        gsl::span<const ast::type_modifier>    modifiers;
    };

    // What a type's `builtin.Unwrappable` impl gives `?` and `!`
    struct unwrap_shape {
        gsl::not_null<impl_instance*> applied;
        gsl::not_null<gs::type*>      output;
        gsl::not_null<gs::type*>      residual;
    };

    // A `Flow` known at compile time: whether it breaks, and the reference or residual it holds
    struct known_flow {
        bool                    is_break;
        stdx::option<ip::index> payload;
    };

    // The `Flow` that `branch` returned, spilled, and whether it breaks
    struct flow_result {
        gir::value slot;
        gir::value is_break;
        // Where the operand lives, which a capture by reference is checked against
        bool is_const;
        bool is_poly;
        // Set at compile time, where nothing is spilled
        stdx::option<known_flow> known{};
    };

    // A returned value's `Flow`, none when its type can't fail
    struct returned_failure {
        flow_result  flow;
        unwrap_shape shape;
    };

    // A `?` whose failure body is being analyzed
    struct propagation {
        flow_result                   flow;
        unwrap_shape                  shape;
        gsl::not_null<impl_instance*> rewrap;
        gsl::not_null<gs::type*>      from;
        stdx::option<operand>         rewrapped{};
    };

    // A runtime `match` while its decision tree is emitted
    struct match_lowering {
        const gs::pattern::decision_tree&      tree;
        const std::vector<match_arm>&          arms;
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
        // A `let` or `return` does but an operand of another operator doesn't
        bool computes_in_target{false};
    };

  private:
    // Instructions
    [[nodiscard]] auto analyze_body(gsl::span<const inst_id> body) -> result<flow>;
    [[nodiscard]] auto tested_call(inst_id inst) -> result<operand>;
    // Whether every operand of `inst` was already known when it was analyzed
    [[nodiscard]] auto operands_known(inst_id inst) const -> bool;
    // Whether running `r` again would run a call, or read an address's number
    [[nodiscard]] auto reaches_call(ref r) const -> bool;
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
    // The name the `let` right after a function literal binds it to
    [[nodiscard]] auto binding_name_of(inst_id value) const -> std::string_view;
    // Whether a closure right after the function literal `func` wraps it
    [[nodiscard]] auto wrapping_closure(inst_id func) const -> bool;
    // A function literal of a runtime body, with the compile-time values its captures read
    [[nodiscard]] auto local_function(
        inst_id                                             func,
        std::vector<ip::index>                              captures,
        stdx::option<u32>                                   self_slot   = stdx::none,
        std::vector<std::pair<u32, std::vector<ip::index>>> known_packs = {}) -> result<operand>;
    // Whether a capture's source is the binding the literal initializes, not analyzed yet
    [[nodiscard]] auto names_itself(stdx::option<inst_id> from) const -> bool;
    // A closure that only runs at compile time
    [[nodiscard]] auto comptime_closure(inst_id inst) -> result<operand>;
    // A closure of a runtime body, its environment built here
    [[nodiscard]] auto local_closure(inst_id inst) -> result<operand>;
    // The value a capture slot the body reads elsewhere holds in `captures`
    [[nodiscard]] auto capture_placeholder() -> ip::index;
    // A closure's environment built from what it stores
    [[nodiscard]] auto build_environment(const closure_env& env, gsl::span<const gir::value> stored)
        -> gir::value;
    // Reads a closure body's captures from its environment parameter
    auto bind_environment(const closure_env& env, const gir::value& self) -> void;
    // Whether a binding declared `declared` may name the function `fn` itself
    [[nodiscard]] auto names_function(const function_ref& fn, gs::type& declared) -> bool;
    [[nodiscard]] auto builtin_fn_ctx(const source& src) -> result<operand>;
    // Runs `call` the way a `@call` modifier asks
    [[nodiscard]] auto with_modifier(stdx::option<ref>                       modifier,
                                     const source&                           src,
                                     const std::function<result<operand>()>& call)
        -> result<operand>;
    [[nodiscard]] auto field_call_unmodified(inst_id              inst,
                                             ref                  object_ref,
                                             std::string_view     name,
                                             gsl::span<const ref> args,
                                             gsl::span<const u32> pack_mask) -> result<operand>;
    // A call of a closure's `.thunk`, its environment the first argument
    [[nodiscard]] auto call_thunk(instance& body, gsl::span<const ref> args, const source& src)
        -> result<operand>;
    // The address of the closure `op` is, spilled when it has none
    [[nodiscard]] auto closure_address(const operand& op, const source& src) -> result<gir::value>;
    [[nodiscard]] auto call_closure(const operand&       callee,
                                    instance&            body,
                                    gsl::span<const ref> args,
                                    const source&        src) -> result<operand>;

    // Methods (members.cc)
    // The `self` argument a method takes from its object
    [[nodiscard]] auto receiver(ast::type_modifier::modifier how,
                                gs::type&                    self_type,
                                ref                          object_ref,
                                const source&                src) -> result<receiver_arg>;
    // The compile-time memory an object lies in, or that its compile-time address names
    [[nodiscard]] auto memory_receiver(ref object_ref, const source& src)
        -> result<stdx::option<comptime_place>>;
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
    // The interface wrapper an impl inherits for a call of `name` leaving defaults off
    [[nodiscard]] auto interface_wrapper(gs::type&        owner,
                                         std::string_view name,
                                         usize            written,
                                         const source&    src) -> result<stdx::option<instance&>>;
    // `^T`, `&T` and `[]T`
    [[nodiscard]] auto analyze_elem_type(inst_id inst, tag t) -> result<operand>;
    [[nodiscard]] auto analyze_array_type(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_fn_type(inst_id inst) -> result<operand>;
    // An array length, which must be known at compile time
    [[nodiscard]] auto array_length(ref len_ref, const source& src) -> result<usize>;

    // Nominal types (aggregates.cc)
    [[nodiscard]] auto analyze_nominal_decl(inst_id inst, gs::type_kind kind) -> result<operand>;
    [[nodiscard]] auto analyze_interface_decl(inst_id inst) -> result<operand>;

    // Vectors (vectors.cc)
    // The vector a value of `type` reads as, directly or through a reference
    [[nodiscard]] auto vector_read(gs::type& type) -> stdx::option<gs::type&>;
    // A known vector's lanes, none when it isn't known
    [[nodiscard]] auto lanes_of(const operand& op, u32 len) -> stdx::option<std::vector<ip::index>>;
    // `scalar` in every lane of `vector`
    [[nodiscard]] auto splat(const operand& scalar, gs::type& vector, const source& src)
        -> result<operand>;
    // An operand of a lane operator, a scalar taking every lane
    [[nodiscard]] auto vector_operand(const operand& op, gs::type& lane, u32 len, const source& src)
        -> result<operand>;
    // A lane operand at runtime, where a known scalar is splat rather than built lane by lane
    [[nodiscard]] auto runtime_lanes(const operand& given, const operand& widened, const source& src)
        -> result<gir::value>;
    // A known vector bound at runtime, built where it is bound and still known to lane folds
    [[nodiscard]] auto bind_known_vector(ip::index known, const source& src) -> result<operand>;
    // Records that a value built at runtime is known, for vector folds
    auto remember_built(const gir::value& built, ip::index known) -> void;
    // A known value, or the known value a runtime aggregate or vector was built from
    [[nodiscard]] auto built_value(const operand& op) const -> stdx::option<ip::index>;
    // A conversion into or out of a vector, none when the types don't pair up
    [[nodiscard]] auto
    coerce_vector(const operand& op, const gs::type& from, gs::type& target, const source& src)
        -> result<stdx::option<operand>>;
    // An operator applied lane by lane
    [[nodiscard]] auto vector_binary(const binary_info& info,
                                     const operand&     lhs,
                                     const operand&     rhs,
                                     const source&      src) -> result<operand>;
    // A read of the lane `index_ref` names
    [[nodiscard]] auto vector_lane(const operand& vector, ref index_ref, const source& src)
        -> result<operand>;
    // A known operand's unary result, none when it doesn't fold
    [[nodiscard]] auto fold_unary(tag t, const operand& value, gs::type& type) -> stdx::option<operand>;
    // A unary operator applied lane by lane
    [[nodiscard]] auto vector_unary(tag t, const operand& value, const source& src)
        -> result<operand>;
    [[nodiscard]] auto builtin_reduce(gsl::span<const ref> args, const source& src)
        -> result<operand>;
    [[nodiscard]] auto builtin_select(gsl::span<const ref> args, const source& src)
        -> result<operand>;
    [[nodiscard]] auto builtin_shuffle(gsl::span<const ref> args, const source& src)
        -> result<operand>;
    // `@maskedLoad`, `@maskedStore`, `@gather` and `@scatter`, which touch only the masked lanes
    [[nodiscard]] auto
    builtin_masked(syntax::builtin_t which, gsl::span<const ref> args, const source& src)
        -> result<operand>;
    // A lane builtin's diagnostic, named for the builtin
    [[nodiscard]] auto lane_error(syntax::builtin_t which, std::string message, const source& src)
        -> stdx::err<stop>;

    // `dyn` (dyn.cc)
    [[nodiscard]] auto analyze_dyn_type(inst_id inst) -> result<operand>;
    // A pointer or reference to a type that implements the interface as a `&dyn I` or `^dyn I`
    [[nodiscard]] auto coerce_to_dyn(const operand& op, gs::type& target, const source& src)
        -> result<stdx::option<operand>>;
    // A method call through a `dyn` object's vtable, none when the object isn't one
    [[nodiscard]] auto call_dyn(inst_id              call,
                                const operand&       object,
                                std::string_view     name,
                                gsl::span<const ref> args,
                                const source&        src) -> result<stdx::option<operand>>;
    // Loads a `dyn` receiver's data and vtable entry before the call's arguments are computed
    void prepare_dyn_call(inst_id call);
    [[nodiscard]] auto
    load_dyn_entry(const operand& object, gs::type& dyn, u64 index, const source& src)
        -> result<dyn_entry>;
    // A `dyn` object's `{ data, vtable }` stored in a slot typed by the bare `dyn`
    [[nodiscard]] auto spill_dyn(const operand& object, gs::type& dyn, const source& src)
        -> result<gir::value>;
    // `@dynCast(^T, w)` reads `w`'s data pointer as a `^T`, unchecked
    [[nodiscard]] auto builtin_dyn_cast(gsl::span<const ref> args, const source& src)
        -> result<operand>;
    // The comptime values a type declaration captures from the body around it
    [[nodiscard]] auto captured_values(inst_id inst, gs::type_kind kind)
        -> result<std::vector<ip::index>>;
    [[nodiscard]] auto resolve_struct(nominal& n) -> result<void>;
    [[nodiscard]] auto resolve_union(nominal& n) -> result<void>;
    [[nodiscard]] auto resolve_enum(nominal& n) -> result<void>;
    // A field's type, an `auto` one taken from its default: `a: auto = 42` is an `i32`
    [[nodiscard]] auto field_type_of(gsl::span<const inst_id>                body,
                                     stdx::option<gsl::span<const inst_id>> default_body,
                                     std::string_view                        name,
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
    // A read of the field `name` of what `object_ref` names
    [[nodiscard]] auto
    field_value(ref object_ref, std::string_view name, const source& src, bool checked = true)
        -> result<operand>;
    [[nodiscard]] auto analyze_field_ptr(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_elem_val(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_elem_ptr(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_ref_of(inst_id inst, bool is_pointer) -> result<operand>;
    // The module-level declaration a `decl_val` names, when it has a GIR global
    [[nodiscard]] auto declared_global(ref r) const -> stdx::option<decl_id>;
    [[nodiscard]] auto analyze_deref(inst_id inst) -> result<operand>;
    // `*s[a..b]`, the array a range of known length spans
    [[nodiscard]] auto
    deref_slice(ref operand_ref, const operand& slice, bool as_place, const source& src)
        -> result<operand>;
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
    [[nodiscard]] auto through_pointer(const place& base, const source& src, bool checked = true)
        -> result<place>;
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

    // Bit-packed aggregates (packed.cc)
    // Rejects a field a bit-packed `kind` can't hold
    [[nodiscard]] auto check_packed_field(gs::type&        field,
                                          std::string_view name,
                                          std::string_view kind,
                                          const source&    src) -> result<void>;
    [[nodiscard]] auto
    reject_packed_alignment(std::string_view name, std::string_view kind, const source& src)
        -> result<void>;
    [[nodiscard]] auto check_packed_width(u64 bits, std::string_view kind, const source& src)
        -> result<void>;
    // The layout of field `index` of the bit-packed `type`
    [[nodiscard]] auto packed_layout(gs::type& type, usize index, gs::type& field) -> packed_bits;
    [[nodiscard]] static auto field_type_at(gs::type& type, usize index) -> gs::type&;
    // Field `index` of a bit-packed aggregate's place, which has no address of its own
    [[nodiscard]] auto packed_place(const place& base, usize index, gs::type& field) -> place;
    // Field `index` read out of a bit-packed aggregate value
    [[nodiscard]] auto
    packed_extract(const gir::value& object, gs::type& type, usize index, gs::type& field)
        -> gir::value;
    [[nodiscard]] auto packed_read(const place& p) -> gir::value;
    void               packed_store(const place& p, const gir::value& value);
    // The `fbits` bits at `offset` of `backing` as `field`
    [[nodiscard]] auto
    bits_extract(const gir::value& backing, const packed_bits& at, gs::type& field) -> gir::value;
    // `old` with the bits at `at` replaced by `value`, a `field`
    [[nodiscard]] auto bits_merge(const gir::value&  old,
                                  const packed_bits& at,
                                  gs::type&          field,
                                  const gir::value&  value) -> gir::value;
    [[nodiscard]] auto backing_int(u32 bits) -> gs::type&;
    // A union's active field as the variant of its tag enum it names
    [[nodiscard]] auto union_tag_as_enum(const operand& op,
                                         gs::type&      union_type,
                                         gs::type&      enum_type,
                                         const source&  src) -> result<operand>;
    // Stores a zero `bits`-wide backing integer to start a bit-packed initializer
    void zero_backing(const place& whole, u32 bits);
    // A known bit-packed value built field by field, each field's bits already known
    [[nodiscard]] auto materialize_packed(const ip::value& value, gs::type& type, const source& src)
        -> result<gir::value>;

    [[nodiscard]] auto index_operand(ref index_ref, const source& src) -> result<gir::value>;
    [[nodiscard]] auto
    bounds_check(const gir::value& index, const gir::value& bound, bool sentinel, const source& src)
        -> result<void>;
    // An element's place, `written` when a store follows, which orders a vector lane's load
    [[nodiscard]] auto elem_place(const place&  object,
                                  ref           index_ref,
                                  const source& src,
                                  bool          written = false) -> result<place>;
    [[nodiscard]] auto comptime_field(ip::index object, std::string_view name, const source& src)
        -> result<operand>;
    // The value of a pack at a compile-time index
    [[nodiscard]] auto pack_elem(pack_ref pack, ref at_ref, const source& src) -> result<operand>;
    // The pack of the elements a pack's compile-time slice bounds select
    [[nodiscard]] auto pack_slice(pack_ref          pack,
                                  stdx::option<ref> lo_ref,
                                  stdx::option<ref> hi_ref,
                                  bool              inclusive,
                                  const source&     src) -> result<operand>;
    [[nodiscard]] auto comptime_elem(ip::index object, ip::index at, const source& src)
        -> result<operand>;

    // Ranges, slicing and `for` (iteration.cc)
    [[nodiscard]] auto analyze_range(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_bind(inst_id inst) -> result<operand>;
    // A slice of a value known at compile time, holding the elements it covers
    [[nodiscard]] auto comptime_slice(ref               object_ref,
                                      stdx::option<ref> lo_ref,
                                      stdx::option<ref> hi_ref,
                                      bool              inclusive,
                                      const source&     src) -> result<operand>;
    [[nodiscard]] auto analyze_slice(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_for(inst_id inst) -> result<operand>;
    [[nodiscard]] auto usize_bound(ref r, const source& src) -> result<gir::value>;
    // The type a `for` range counts in
    [[nodiscard]] auto counter_type(const range_operands& r) -> gs::type&;
    // Every value a range takes, when both its bounds are known
    [[nodiscard]] auto known_range(const range_operands& r, bool inclusive)
        -> stdx::option<value_range>;
    // A `for` capture's type, a `&v` or `^v` one's checked against the iterable it reaches into
    [[nodiscard]] auto for_capture_type(const source&      src,
                                        usize              at,
                                        ast::type_modifier modifier,
                                        gs::type&          elem,
                                        bool               is_const,
                                        bool               is_poly)
        -> result<gsl::not_null<gs::type*>>;
    // A `for` at compile time, binding each iteration's values as constants
    [[nodiscard]] auto run_for(inst_id                  inst,
                               gsl::span<const ref>     iterables,
                               const for_captures&      captures,
                               gsl::span<const inst_id> body,
                               gsl::span<const inst_id> else_body) -> result<operand>;

    // `match` (match.cc)
    [[nodiscard]] auto analyze_match(inst_id inst) -> result<operand>;
    [[nodiscard]] auto match_arms(inst_id inst) -> std::vector<match_arm>;
    [[nodiscard]] auto capture_name(const ast::capture_t& capture) const -> std::string_view;
    // A pattern's value as written, which must be known at compile time
    [[nodiscard]] auto pattern_operand(gsl::span<const inst_id> body, const source& src)
        -> result<operand>;
    // A match on a type takes no guard or capture, and needs a catch-all arm
    [[nodiscard]] auto check_type_match(stdx::option<u32> catch_all, const source& src)
        -> result<void>;
    // A pattern's value, which must be known at compile time
    [[nodiscard]] auto pattern_value(gsl::span<const inst_id> body,
                                     gs::type&                type,
                                     const source&            src) -> result<ip::index>;
    // A nested pattern, unlike a whole one, must be known at compile time
    [[nodiscard]] auto pattern_node(inst_id   pat,
                                    gs::type& type,
                                    bool      place_is_const = false,
                                    bool      top_level      = true) -> result<gs::pattern::node_ptr>;
    // A pattern's value when it is known at compile time, none when it is only known at runtime
    [[nodiscard]] auto known_pattern(gsl::span<const inst_id> body,
                                     gs::type&                type,
                                     const source&            src) -> result<stdx::option<ip::index>>;
    // A pattern compared whole at runtime, tested in source order
    [[nodiscard]] auto opaque_pattern(inst_id pat, gs::type& type, bool top_level)
        -> result<gs::pattern::node_ptr>;
    // `"ab"` against bytes: a slice's length and bytes, or an array's bytes
    [[nodiscard]] auto string_pattern(inst_id pat, ip::index text, gs::type& type)
        -> result<gs::pattern::node_ptr>;
    // Whether the known `value` passes an opaque pattern, known at compile time too
    [[nodiscard]] auto known_opaque(const gs::pattern::node& node,
                                    ip::index                value,
                                    const source&            src) -> result<bool>;
    // Whether `subject` passes the opaque pattern `pat`
    [[nodiscard]] auto opaque_test(const gir::value& subject, inst_id pat) -> result<gir::value>;
    // The `|x|` a `pat_capture` was written as
    [[nodiscard]] auto pattern_capture(inst_id pat) const -> const ast::capture_t&;
    // `.{ ... }` against an array, a struct or a slice
    [[nodiscard]] auto aggregate_pattern(inst_id pat, gs::type& type, bool place_is_const)
        -> result<gs::pattern::node_ptr>;
    // `.name(p)` against a tagged union, the payload matched by `p`
    [[nodiscard]] auto variant_pattern(inst_id pat, gs::type& type, bool place_is_const)
        -> result<gs::pattern::node_ptr>;
    // Whether the known `value` matches `node`, collecting the values its captures bind
    [[nodiscard]] auto
    known_match(const gs::pattern::node&                            node,
                ip::index                                           value,
                std::vector<std::pair<std::string_view, ip::index>>& bound,
                const source&                                        src,
                const stdx::option<comptime_alias>&                  at = stdx::none)
        -> result<bool>;
    // Binds each name an arm's patterns capture to what `lookup` gives for it
    template <typename Lookup> void bind_arm_names(const match_arm& arm, Lookup&& lookup);
    // The value of a runtime match that runs entirely at compile time to a number or `bool`
    [[nodiscard]] auto fold_match(const std::vector<match_arm>&              arms,
                                  const std::vector<gs::pattern::row_input>& rows,
                                  ref                                        operand_ref,
                                  const source& src) -> result<stdx::option<operand>>;
    // A compile-time `match` on a type or untyped number, comparing each pattern's value
    [[nodiscard]] auto match_by_value(const std::vector<match_arm>& arms,
                                      stdx::option<u32>             catch_all,
                                      const operand&                scrutinee,
                                      gs::type&                     type,
                                      const source&                 src) -> result<operand>;
    // A `match` at compile time, which analyzes only the arm it takes
    [[nodiscard]] auto run_match(const std::vector<match_arm>&              arms,
                                 const std::vector<gs::pattern::row_input>& rows,
                                 const operand&                             scrutinee,
                                 const source&                              src,
                                 bool                                       binding = false,
                                 const stdx::option<comptime_alias>& root = stdx::none)
        -> result<operand>;
    [[nodiscard]] auto emit_match(const std::vector<match_arm>&              arms,
                                  const std::vector<gs::pattern::row_input>& rows,
                                  const gs::pattern::decision_tree&          tree,
                                  ref                                        operand_ref,
                                  const operand&                             scrutinee,
                                  gs::type&                                  type,
                                  const source&                              src,
                                  stdx::option<u32> binding_else) -> result<operand>;
    [[nodiscard]] auto scrutinee_address(ref            operand_ref,
                                         const operand& scrutinee,
                                         gs::type&      type,
                                         bool           aggregate,
                                         bool           aliased,
                                         const source&  src) -> result<match_root>;
    // A capture's slot type, a `&v` or `^v` capture's checked against where it's reached
    [[nodiscard]] auto capture_type(const gs::pattern::capture_site& site, capture_access root)
        -> result<gsl::not_null<gs::type*>>;
    // Where a compile-time `match` takes its scrutinee apart, when a capture aliases it
    [[nodiscard]] auto comptime_scrutinee(ref            operand_ref,
                                          const operand& scrutinee,
                                          const source&  src) -> result<stdx::option<comptime_alias>>;
    // Whether `r` names a `mut?` parameter, whose argument has its caller's mutability
    [[nodiscard]] auto names_poly_param(ref r, const source& src) -> result<bool>;
    // A capture's type under its modifier, `what` naming the value in its errors
    [[nodiscard]] auto alias_type(ast::type_modifier modifier,
                                  gs::type&          value_type,
                                  capture_access     access,
                                  std::string_view   what,
                                  const source&      src) -> result<gsl::not_null<gs::type*>>;
    [[nodiscard]] auto occurrence_address(const match_lowering& m, usize occ) -> gir::value;
    [[nodiscard]] auto emit_decision(const match_lowering& m, usize node) -> result<void>;
    [[nodiscard]] auto slot_operand(const capture_slot& slot) -> operand;
    // Binds the names an arm's patterns capture to the arm's slots
    void bind_slots(const match_lowering& m, usize arm);

    // Types with no declaration (synthesized.cc)
    [[nodiscard]] auto analyze_merge_enums(inst_id inst) -> result<operand>;
    // `@Enum(desc)`, one enum per call site and descriptor
    [[nodiscard]] auto builtin_enum(inst_id inst, gsl::span<const ref> args, const source& src)
        -> result<operand>;
    // `@Struct(desc)` and `@Union(desc)`, one type per call site and descriptor
    [[nodiscard]] auto builtin_struct(inst_id inst, gsl::span<const ref> args, const source& src)
        -> result<operand>;
    [[nodiscard]] auto builtin_union(inst_id inst, gsl::span<const ref> args, const source& src)
        -> result<operand>;
    // The fields a `StructInfo` or `UnionInfo` lists
    [[nodiscard]] auto descriptor_fields(ip::index        desc,
                                         std::string_view builtin_name,
                                         bool             is_struct,
                                         const source& src) -> result<std::vector<synthetic_field>>;
    // A field of a compile-time aggregate, none when it isn't known
    [[nodiscard]] auto known_field(ip::index object, std::string_view name, const source& src)
        -> result<stdx::option<ip::index>>;
    // An enum value as the enum merged from it, its tag rewritten where the merge moved it
    [[nodiscard]] auto widen_enum(const operand& op, gs::type& target, const source& src)
        -> result<operand>;

    // `a ++ b` of two compile-time arrays or slices (concat.cc)
    [[nodiscard]] auto analyze_concat(inst_id inst) -> result<operand>;

    // Unwrapping (unwrap.cc)
    [[nodiscard]] auto analyze_unwrap(inst_id inst) -> result<operand>;
    // `if (x) |v| a else |e| b`
    [[nodiscard]] auto analyze_unwrap_branch(inst_id inst) -> result<operand>;
    // The shape an `if` or `while` capture unwraps, its `else` capture checked
    [[nodiscard]] auto capture_shape(const operand&                      op,
                                     const stdx::option<ast::capture_t>& residual,
                                     std::string_view                    construct,
                                     const source&                       cond_src,
                                     const source& src) -> result<unwrap_shape>;
    // Binds an unwrapping capture's `bind` in `body` to `value`
    auto bind_unwrapped(gsl::span<const inst_id> body, u32 slot, const operand& value) -> void;
    // A `|v|` payload capture's slot, a copy or the address the `Flow` holds
    [[nodiscard]] auto payload_capture(const ast::capture_t& capture,
                                       const flow_result&    flow,
                                       const unwrap_shape&   shape) -> result<operand>;
    // A payload capture of a compile-time `Flow`, the payload or its address
    [[nodiscard]] auto known_capture(const ast::capture_t& capture,
                                     const flow_result&    flow,
                                     gs::type&             cap_type) -> result<operand>;
    [[nodiscard]] auto residual_capture(const flow_result& flow, const unwrap_shape& shape)
        -> operand;
    [[nodiscard]] auto analyze_residual(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_ret_residual(inst_id inst) -> result<operand>;
    // A `return` with errdefers pending asks its value's `branch` whether it fails
    [[nodiscard]] auto analyze_ret_failure(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_failure_is_set(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_failure_residual(inst_id inst) -> result<operand>;
    // The impl of the `builtin` interface `name` for `target`, none when it has none
    [[nodiscard]] auto prelude_impl(gs::type& target, std::string_view name, const source& src)
        -> result<stdx::option<impl_instance&>>;
    [[nodiscard]] auto associated_type(impl_instance&   applied,
                                       std::string_view name,
                                       const source&    src) -> result<stdx::option<gs::type&>>;
    [[nodiscard]] auto call_impl_method(impl_instance&       applied,
                                        std::string_view     name,
                                        std::vector<operand> args,
                                        const source&        src) -> result<operand>;
    [[nodiscard]] auto unwrap_shape_of(gs::type& operand_type, const source& src)
        -> result<stdx::option<unwrap_shape>>;
    // Calls `branch` on the operand's address and tests the `Flow` it returns
    [[nodiscard]] auto
    emit_flow(ref operand_ref, const operand& op, const unwrap_shape& shape, const source& src)
        -> result<flow_result>;
    // Reads the `continue` payload through the reference the `Flow` holds
    [[nodiscard]] auto flow_output(const flow_result& flow, const unwrap_shape& shape)
        -> gir::value;
    [[nodiscard]] auto propagation_for(const operand&      op,
                                       const unwrap_shape& shape,
                                       const source&       src) -> result<propagation>;

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
    // A union whose `index` field holds `given`
    [[nodiscard]] auto union_of(gs::type& type, usize index, const operand& given, const source& src)
        -> result<operand>;
    [[nodiscard]] auto init_array(inst_id inst, gs::type& type, const source& src)
        -> result<operand>;
    // Each field's default, none for a field without one
    [[nodiscard]] auto field_defaults(gs::type& type, const source& src)
        -> result<std::vector<default_source>>;
    [[nodiscard]] auto
    field_default(gs::type& type, usize index, const default_source& from, const source& src)
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
    // The binary operator later in `body` whose left side is `call`, read through its reference
    [[nodiscard]] auto reading_binary(gsl::span<const inst_id> body, inst_id call) const
        -> stdx::option<inst_id>;
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
    // `r` computed again in the wider `target`, when it has an untyped side
    [[nodiscard]] auto widened_arithmetic(ref r, gs::type& target, const source& src)
        -> result<stdx::option<gir::value>>;
    [[nodiscard]] auto fold_into_merge(const binary_info& info,
                                       const operand&     lhs,
                                       const operand&     rhs,
                                       const source&      src) -> result<stdx::option<operand>>;
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
    // The bytes a known `u8` aggregate holds, none when one isn't known
    [[nodiscard]] auto known_bytes(const ip::aggregate_value& agg, gs::type& elem)
        -> stdx::option<std::string>;
    // A known array as a compile-time slice of the same elements, none when the types disagree
    [[nodiscard]] auto comptime_slice_of(ip::index array, gs::type& from, gs::type& target)
        -> stdx::option<operand>;
    // The slice viewing all of the array at `address`
    [[nodiscard]] auto slice_of_array(const gir::value& address, gs::type& from)
        -> result<gir::value>;
    [[nodiscard]] auto as_runtime(const operand& op, const source& src) -> result<gir::value>;
    // The runtime address of a compile-time address that names no global
    [[nodiscard]] auto
    stored_pointee(const ip::address_value& address, gs::type& type, const source& src)
        -> result<gir::value>;
    // A function as a compile-time value of the function type `target`, none when it isn't one
    [[nodiscard]] auto comptime_callable(const operand& op, gs::type& target, const source& src)
        -> result<stdx::option<operand>>;
    // A function or callable value as the erased `target`, none when it isn't one
    [[nodiscard]] auto as_erased_callable(const operand& op, gs::type& target, const source& src)
        -> result<stdx::option<operand>>;
    // A generic's instance for the parameters of `signature`
    [[nodiscard]] auto instance_for_signature(generic_fn&                  g,
                                              const gs::types::function& signature,
                                              const source& src)
        -> result<gsl::not_null<instance*>>;
    [[nodiscard]] auto materialize_merge(merge_ref m, gs::type& type, const source& src)
        -> result<gir::value>;
    // A `.{...}` or `.name` that takes its type from where it is used
    // The variant an enum literal names
    [[nodiscard]] auto literal_name(const enum_literal& literal) const -> std::string;
    // An enum literal as a compile-time value of `@EnumLiteral()`
    [[nodiscard]] auto literal_value(const enum_literal& literal) -> ip::index;
    [[nodiscard]] static auto is_untyped_literal(const operand& op) -> bool {
        return op.is<anon_init>() || op.is<enum_literal>() || op.is<cast_ref>() ||
               op.is<implicit_call>() || op.is<slice_literal>();
    }
    [[nodiscard]] auto slice_literal_as(slice_literal lit, gs::type& target, const source& src)
        -> result<operand>;
    [[nodiscard]] auto concrete(const operand& op, const source& src) -> result<operand>;

    // Locals and memory (memory.cc)
    [[nodiscard]] auto analyze_alloc_mut(inst_id inst, bool is_comptime_alloc) -> result<operand>;
    [[nodiscard]] auto store_comptime(const comptime_place& p,
                                      const operand&        value,
                                      const source&         src) -> result<void>;
    [[nodiscard]] auto load_comptime(const comptime_place& p) -> operand;
    // The writable run of compile-time memory a range copy or `@memset` fills
    [[nodiscard]] auto comptime_range(const operand& dest, const source& src)
        -> result<memory_view>;
    // `a[i..j] = v`, `@memcpy` and `@memmove` into compile-time memory
    [[nodiscard]] auto comptime_range_copy(const operand&   dest,
                                           const operand&   from,
                                           std::string_view what,
                                           const source&    src) -> result<void>;
    [[nodiscard]] auto comptime_fill(const operand& dest, const operand& value, const source& src)
        -> result<void>;
    // A runtime slot holding a copy of what comptime memory holds
    [[nodiscard]] auto comptime_copy(const comptime_place& p, const source& src) -> result<place>;
    // The compile-time address of a place in comptime memory as a value of `type`
    [[nodiscard]] auto memory_address(const comptime_place& p, gs::type& type) -> ip::index;
    // The place a compile-time address names
    [[nodiscard]] auto memory_place(const operand& op) -> stdx::option<comptime_place>;
    // The memory an object's compile-time address names, held directly or in comptime memory
    [[nodiscard]] auto memory_target(const operand& object) -> stdx::option<comptime_place>;
    // The address an object holds, read from comptime memory when it is kept there
    [[nodiscard]] auto held_address(const operand& object) -> operand;
    // The elements an array in comptime memory, or a compile-time slice of one, views
    [[nodiscard]] auto view_of(const operand& object) -> stdx::option<memory_view>;
    // A compile-time slice of a view's elements from `lo` up to `hi`
    [[nodiscard]] auto memory_slice(const memory_view& view, u64 lo, u64 hi) -> ip::index;
    // The element a compile-time slice or element pointer reaches at an index
    [[nodiscard]] auto memory_element(const operand& object, ref index_ref, const source& src)
        -> result<stdx::option<comptime_place>>;
    // What a known pointer or slice holds
    [[nodiscard]] auto comptime_deref(const operand& ptr) -> stdx::option<operand>;
    // `.ptr` or `.len` of comptime memory or a compile-time slice
    [[nodiscard]] auto memory_field(const operand& object, std::string_view name)
        -> stdx::option<operand>;
    // Where a store to a comptime place or reference writes, through any reference it holds
    [[nodiscard]] auto comptime_destination(const operand& target) -> stdx::option<comptime_place>;
    // `current` with the element `path` leads to replaced by `value`
    [[nodiscard]] auto with_element(stdx::option<ip::index>        current,
                                    gs::type&                      type,
                                    gsl::span<const comptime_step> path,
                                    ip::index                      value,
                                    const source&                  src) -> result<ip::index>;
    // An element or field of comptime memory
    [[nodiscard]] auto comptime_elem_place(const comptime_place& p,
                                           ref                   index_ref,
                                           const source&         src) -> result<operand>;
    [[nodiscard]] auto comptime_field_place(const comptime_place& p,
                                            std::string_view      name,
                                            const source&         src) -> result<operand>;
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
    // A runtime array arm as a slice of where it is stored, or of a slot holding it
    [[nodiscard]] auto array_slice(const incoming& arm, const source& src) -> result<operand>;
    [[nodiscard]] auto arm_address(const incoming& arm, const source& src) -> result<gir::value>;
    [[nodiscard]] auto analyze_block(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_loop(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_condbr(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_break(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_repeat(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_ret(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_ret_value(inst_id inst) -> result<operand>;
    // What a compile-time reference refers to, none for any other operand
    [[nodiscard]] auto comptime_referent(const operand& op) -> stdx::option<ip::index>;
    // What a reference refers to, loaded, and any other operand as it is
    [[nodiscard]] auto read_through_reference(const operand& op, const source& src)
        -> result<operand>;
    [[nodiscard]] auto analyze_unreachable(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_block_comptime(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_condbr_comptime(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_eval_branch(inst_id inst) -> result<operand>;
    // A block at compile time, taking a `break` that targets it as its value
    [[nodiscard]] auto run_block(inst_id inst, gsl::span<const inst_id> body) -> result<operand>;
    [[nodiscard]] auto run_loop(inst_id inst) -> result<operand>;
    // Records a loop unrolled into a runtime body until the guard ends
    [[nodiscard]] auto unrolling(inst_id inst) {
        unrolling_.emplace_back<unrolled_loop>(
            {.inst = inst, .runtime_depth = runtime_depth_, .targets = targets_.size()});
        return gsl::finally([this] { unrolling_.pop_back(); });
    }
    // Reports a `break` or `continue` an unrolled loop's runtime copies can't take
    [[nodiscard]] auto unrolled_jump(ref to, bool is_break, const source& src) -> result<void>;
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
    [[nodiscard]] auto at_comptime_boundary(result<operand> value, const source& src)
        -> result<operand>;
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
    // Whether `r` is a loop being unrolled at runtime, which its jumps leave per iteration
    [[nodiscard]] auto unrolls(ref r) const -> bool;
    auto               arrive(target& t, const operand& value, const source& src) -> void;
    [[nodiscard]] auto condition(const operand& op, const source& src) -> result<gir::value>;
    [[nodiscard]] auto void_value() -> operand;

    // Calls (calls.cc)
    [[nodiscard]] auto analyze_call(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_field_call(inst_id inst) -> result<operand>;
    // A call whose arguments may expand packs, `f(rest...)`
    [[nodiscard]] auto call_expanding(const operand&       callee,
                                      gsl::span<const ref> args,
                                      gsl::span<const u32> pack_mask,
                                      const source&        src,
                                      const source&        callee_src) -> result<operand>;
    // Calls a function or generic `callee` evaluated to
    [[nodiscard]] auto call_operand(const operand&       callee,
                                    gsl::span<const ref> args,
                                    const source&        src,
                                    const source&        callee_src) -> result<operand>;
    // `s.f()` where `f` is a field holding a plain function
    [[nodiscard]] auto call_through_field(ref                  object_ref,
                                          std::string_view     name,
                                          gsl::span<const ref> args,
                                          const source&        src) -> result<operand>;
    // A call through a function value of type `fn`
    [[nodiscard]] auto call_value(const gir::value&          callee,
                                  const gs::types::function& fn,
                                  gsl::span<const ref>       args,
                                  const source&              src) -> result<operand>;
    // A call of `target`. `through_member` calls a type's function by its name: `T.f(x)`
    [[nodiscard]] auto call_function(instance&            target,
                                     gsl::span<const ref> args,
                                     const source&        src,
                                     const source&        callee_src,
                                     bool                 through_member) -> result<operand>;
    // A call whose `mut?` result takes the mutability its `mut?` argument has
    [[nodiscard]] auto call_following_poly(instance&            target,
                                           std::vector<operand> values,
                                           const source&        src,
                                           bool                 through_member) -> result<operand>;
    // A call of `target` on arguments that match its GIR parameters
    [[nodiscard]] auto
    call_with(instance& target, std::vector<operand> args, const source& src, bool through_member)
        -> result<operand>;

    // Generic calls (generics.cc)
    // A local generic's call passes the address of the environment it was called through
    [[nodiscard]] auto call_generic(generic_fn&              g,
                                    std::vector<call_arg>    args,
                                    const source&            src,
                                    const source&            callee_src,
                                    stdx::option<gir::value> env = stdx::none) -> result<operand>;
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
    // A generic's own attributes, an error when one reads its parameters
    [[nodiscard]] auto generic_attributes(generic_fn& g, const source& src)
        -> result<gs::resolved_attributes>;
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
    // A file's bytes as a constant `[N:0]u8`, read through the context's cache
    [[nodiscard]] auto builtin_embed(gsl::span<const ref> args, const source& src)
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
    [[nodiscard]] auto builtin_export(gsl::span<const ref> args, const source& src)
        -> result<operand>;
    [[nodiscard]] auto builtin_src(const source& src) -> result<operand>;
    [[nodiscard]] auto builtin_entry_test_runner(const source& src) -> result<operand>;
    [[nodiscard]] auto builtin_test_functions(const source& src) -> result<operand>;
    // Reports an entry builtin used outside the program's entry code
    [[nodiscard]] auto require_start(std::string_view name, const source& src) -> result<void>;
    // A statement's value, which must be void or a `@[discardable]` call's
    [[nodiscard]] auto analyze_unused(inst_id inst) -> result<operand>;
    // A failed `@expect` reports and goes on, a failed `@require` stops
    [[nodiscard]] auto comptime_expect(gsl::span<const ref> args, const source& src, bool require)
        -> result<operand>;
    [[nodiscard]] auto builtin_skip(gsl::span<const ref> args, const source& src) -> result<operand>;
    // Calls the skip handler, then leaves the test, or the `@[testing]` helper for good
    [[nodiscard]] auto emit_skip(const gir::value& message, const source& src) -> result<operand>;
    // The function an `@export` argument names, none for anything else
    [[nodiscard]] auto export_target(ref arg, const source& src) -> result<stdx::option<instance&>>;
    // The text a known string constant holds
    [[nodiscard]] auto known_text(ip::index value) -> stdx::option<std::string>;
    [[nodiscard]] auto builtin_limit(gsl::span<const ref> args, const source& src, bool recursion)
        -> result<operand>;
    [[nodiscard]] auto builtin_layout(gsl::span<const ref> args, const source& src, bool size)
        -> result<operand>;
    [[nodiscard]] auto builtin_assert(gsl::span<const ref> args, const source& src, bool verify)
        -> result<operand>;
    // A type builtin's descriptor argument as the prelude's `info_name`, known at compile time
    [[nodiscard]] auto descriptor(gsl::span<const ref> args,
                                  std::string_view     builtin_name,
                                  std::string_view     info_name,
                                  const source&        src) -> result<ip::index>;
    [[nodiscard]] auto builtin_int_type(gsl::span<const ref> args, const source& src)
        -> result<operand>;
    [[nodiscard]] auto builtin_float_type(gsl::span<const ref> args, const source& src)
        -> result<operand>;
    // A memory builtin's buffer taken apart: its first byte, element count and element size
    struct mem_view {
        gir::value data;
        gir::value count;
        // The destination's size in bytes, which the old emitter computes before the source
        stdx::option<gir::value> bytes;
        // The element type, which an untyped `.{...}` source takes
        stdx::option<gs::type&> elem{};
    };

    // Memory builtins (math.cc)
    // Inline assembly (asm.cc)
    [[nodiscard]] auto analyze_asm(inst_id inst) -> result<operand>;
    [[nodiscard]] auto analyze_mem_view(inst_id inst) -> result<operand>;
    // A buffer taken apart into its data, element count and, for a destination, byte count
    [[nodiscard]] auto
    buffer_view(ref arg_ref, const operand& given, bool is_dest, const source& src)
        -> result<mem_view>;
    [[nodiscard]] auto byte_count(const gir::value& count, usize elem_bytes) -> gir::value;
    // `@memcpy`, `@memmove` and `@memset`
    [[nodiscard]] auto builtin_mem(syntax::builtin_t    which,
                                   gsl::span<const ref> args,
                                   const source&        src) -> result<operand>;

    // Math builtins (math.cc)
    [[nodiscard]] auto builtin_bit_count(syntax::builtin_t    which,
                                         gsl::span<const ref> args,
                                         const source&        src) -> result<operand>;
    [[nodiscard]] auto builtin_abs(gsl::span<const ref> args, const source& src) -> result<operand>;
    [[nodiscard]] auto builtin_mul_add(gsl::span<const ref> args, const source& src)
        -> result<operand>;

    // `@addWithOverflow` and the others, which write the wrapped result through a reference
    [[nodiscard]] auto
    builtin_overflow(syntax::builtin_t which, gsl::span<const ref> args, const source& src)
        -> result<operand>;

    // Weights a two-way branch by the hints its sides start with
    auto apply_branch_hints(gir::instruction&        branch,
                            gsl::span<const inst_id> on_true,
                            gsl::span<const inst_id> on_false) const -> void;
    [[nodiscard]] auto builtin_branch_hint(inst_id inst, gsl::span<const ref> args, const source& src)
        -> result<operand>;
    // Whether runtime safety checks are emitted here, which `@setRuntimeSafety` scopes to a body
    [[nodiscard]] auto runtime_safety() const -> bool;
    [[nodiscard]] auto builtin_set_runtime_safety(gsl::span<const ref> args, const source& src)
        -> result<operand>;

    // `@Pointer`, `@Reference`, `@Slice` and `@Array` from their descriptors
    [[nodiscard]] auto
    builtin_shaped_type(syntax::builtin_t which, gsl::span<const ref> args, const source& src)
        -> result<operand>;
    [[nodiscard]] auto builtin_fn_type(gsl::span<const ref> args, const source& src)
        -> result<operand>;

    // A field name argument known at compile time, none when it isn't
    [[nodiscard]] auto field_name_arg(ref arg, const source& src)
        -> result<stdx::option<std::string>>;
    // `@hasField`, `@fieldType` and `@fieldDefault`
    [[nodiscard]] auto
    builtin_field_query(syntax::builtin_t which, gsl::span<const ref> args, const source& src)
        -> result<operand>;
    [[nodiscard]] auto builtin_field_parent_ptr(gsl::span<const ref> args, const source& src)
        -> result<operand>;

    // The wrapper a declaration's parser made for a call writing `written` of its arguments
    [[nodiscard]] auto
    default_wrapper(const unit& owner, decl_id decl, usize written, const source& src)
        -> result<stdx::option<operand>>;

    // The type an argument a pack takes has, an untyped number taking a concrete one
    [[nodiscard]] auto pack_element_type(const operand& arg) -> gs::type&;
    // Why a callable argument doesn't match an `impl Fn(...)` pattern, none when it does
    [[nodiscard]] auto callable_mismatch(const gs::type& pattern, const operand& arg)
        -> stdx::option<std::string>;

    // A range used as a value: the array of the values it takes, of `elem` if given
    [[nodiscard]] auto range_value(inst_id inst, stdx::option<gs::type&> elem, const source& src)
        -> result<operand>;

    // Runs the operand's `branch` at compile time, on compile-time memory holding it
    [[nodiscard]] auto
    comptime_flow(ref operand_ref, const operand& op, const unwrap_shape& shape, const source& src)
        -> result<flow_result>;

    // A comparison of an integer with an untyped number outside its type, known whatever the value
    [[nodiscard]] auto compare_out_of_range(const binary_info& info,
                                            const operand&     lhs,
                                            const operand&     rhs) -> stdx::option<bool>;
    // `+` or a comparison of two known strings, none for anything else
    [[nodiscard]] auto string_binary(const binary_info& info,
                                     const operand&     lhs,
                                     const operand&     rhs) -> stdx::option<ip::index>;
    // A binary operator of an inferred cast and an untyped value, once its type is known
    [[nodiscard]] auto binary_as(inst_id inst, gs::type& target, const source& src)
        -> result<operand>;

    // Atomics (atomics.cc)
    [[nodiscard]] auto
    builtin_atomic(syntax::builtin_t which, gsl::span<const ref> args, const source& src)
        -> result<operand>;
    // The argument at `index`, a variant of the `builtin` module's enum `prelude`
    [[nodiscard]] auto atomic_enum_arg(syntax::builtin_t    which,
                                       gsl::span<const ref> args,
                                       usize                index,
                                       std::string_view     prelude,
                                       std::string_view     what,
                                       const source&        src) -> result<atomic_choice>;
    // The scalar forms of the math builtins, on operands already analyzed
    [[nodiscard]] auto bit_count_of(syntax::builtin_t which, const operand& arg, const source& src)
        -> result<operand>;
    [[nodiscard]] auto abs_of(const operand& arg, const source& src) -> result<operand>;
    [[nodiscard]] auto binary_math_of(syntax::builtin_t which,
                                      const operand&    lhs,
                                      const operand&    rhs,
                                      const source&     src) -> result<operand>;
    [[nodiscard]] auto untyped_division(syntax::builtin_t which,
                                        const big_int&    lhs,
                                        const big_int&    rhs,
                                        gs::type&         type,
                                        const source&     src) -> result<operand>;
    [[nodiscard]] auto float_math_of(syntax::builtin_t which, const operand& arg, const source& src)
        -> result<operand>;
    [[nodiscard]] auto mul_add_of(gs::type& type, gsl::span<const operand> values, const source& src)
        -> result<operand>;
    // One lane of a math builtin applied to a vector
    [[nodiscard]] auto math_lane(syntax::builtin_t        which,
                                 gsl::span<const operand> lanes,
                                 gs::type&                lane,
                                 const source&            src) -> result<operand>;
    // A math builtin applied lane by lane, `declared` naming `@mulAdd`'s type
    [[nodiscard]] auto vector_math(syntax::builtin_t       which,
                                   std::vector<operand>    values,
                                   stdx::option<gs::type&> declared,
                                   const source&           src) -> result<operand>;
    // `@min`, `@max` and the division builtins, whose operands meet at their peer type
    [[nodiscard]] auto builtin_binary_math(syntax::builtin_t    which,
                                           gsl::span<const ref> args,
                                           const source&        src) -> result<operand>;
    [[nodiscard]] auto builtin_float_math(syntax::builtin_t    which,
                                          gsl::span<const ref> args,
                                          const source&        src) -> result<operand>;
    // `@Vector(n, T)` or `@Vector(desc)`
    [[nodiscard]] auto builtin_vector_type(gsl::span<const ref> args, const source& src)
        -> result<operand>;
    // `@targetOs`, `@targetArch`, `@targetAbi` and `@targetFamily` as the prelude's enums
    [[nodiscard]] auto builtin_target(syntax::builtin_t which, const source& src)
        -> result<operand>;
    [[nodiscard]] auto builtin_panic(gsl::span<const ref> args, const source& src)
        -> result<operand>;
    [[nodiscard]] auto builtin_implements(gsl::span<const ref> args, const source& src)
        -> result<operand>;
    // A runtime `@assert` or `@verify`, a call to the handler where the condition fails
    [[nodiscard]] auto builtin_llvm_intrinsic(gsl::span<const ref> args, const source& src)
        -> result<operand>;
    [[nodiscard]] auto builtin_return_address(const source& src) -> result<operand>;
    [[nodiscard]] auto builtin_entry_main(const source& src) -> result<operand>;
    [[nodiscard]] auto builtin_field(gsl::span<const ref> args, const source& src)
        -> result<operand>;
    // `@tagName` of a value known only at runtime, tested against each name in turn
    [[nodiscard]] auto
    runtime_tag_name(const operand& value, gs::type& type, gs::type& name_type, const source& src)
        -> result<operand>;
    [[nodiscard]] auto builtin_name_location(const source& src) -> source_location;
    // `@expect` / `@require`, a test check whose failure calls a weak handler
    [[nodiscard]] auto builtin_expect(gsl::span<const ref> args, const source& src, bool require)
        -> result<operand>;
    [[nodiscard]] auto runtime_assert(gsl::span<const ref> args, const source& src, bool verify)
        -> result<operand>;
    [[nodiscard]] auto builtin_trap(const source& src) -> result<operand>;
    // `@cVaStart`, `@cVaArg`, `@cVaCopy` and `@cVaEnd` on caller-provided `va_list` storage
    [[nodiscard]] auto builtin_c_va(syntax::builtin_t    which,
                                    gsl::span<const ref> args,
                                    const source&        src) -> result<operand>;
    [[nodiscard]] auto builtin_as(inst_id inst, gsl::span<const ref> args, const source& src)
        -> result<operand>;
    // `@as(T, x)` with its target known
    [[nodiscard]] auto as_to(gs::type& target, ref value_ref, const source& src) -> result<operand>;
    // Why `@as` can't narrow a runtime `from` to `target`, naming the builtin that can
    [[nodiscard]] auto narrowing_as(const gs::type& from, const gs::type& target) -> std::string;
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
    // A lane-by-lane conversion between vectors of as many lanes
    [[nodiscard]] auto vector_cast(syntax::builtin_t which,
                                   const operand&    value,
                                   gs::type&         target,
                                   const source&     src,
                                   const source&     operand_src) -> result<operand>;
    [[nodiscard]] auto
    cast_value(gir::instruction_kind kind, const gir::value& from, gs::type& target) -> operand;
    [[nodiscard]] auto
    check_bit_cast(const gs::type& from, const gs::type& target, const source& src) -> result<void>;
    // The bits `@bitCast` reinterprets in a `t`, none for a type with no fixed bit layout
    [[nodiscard]] auto bit_width(const gs::type& t) const -> stdx::option<u64>;
    // A known value's bits, an array's first element lowest, and the value those bits make
    [[nodiscard]] auto bits_of(ip::index value, const gs::type& type, const source& src)
        -> result<u128>;
    [[nodiscard]] auto value_of_bits(u128 bits, gs::type& type, const source& src)
        -> result<ip::index>;
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
    // Where a builtin call's argument is written, none when the call has no such argument
    [[nodiscard]] auto arg_src(const source& call_src, usize index) const -> stdx::option<source>;
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
    stdx::option<decl_id>                          statement_;
    stdx::option<instance&>                        fn_;
    stdx::option<gir::function&>                   gir_fn_;
    gir::builder                                   builder_;
    ankerl::unordered_dense::map<inst_id, operand> values_;
    // Instructions whose analysis failed, so a later read is poisoned rather than unsupported
    ankerl::unordered_dense::set<inst_id> failed_;
    // Result slots an inner `and` / `or` already made for the ones it is the left side of
    ankerl::unordered_dense::map<inst_id, gir::value>                     bool_slots_;
    std::vector<target>                                                   targets_;
    std::vector<pending_merge>                                            merges_;
    std::vector<std::vector<operand>>                                     packs_;
    std::vector<propagation>                                              propagations_;
    ankerl::unordered_dense::map<inst_id, stdx::option<returned_failure>> failures_;
    // The loops being unrolled into a runtime body, innermost last
    std::vector<unrolled_loop>                      unrolling_;
    ankerl::unordered_dense::map<inst_id, mem_view> mem_views_;
    // The types an inferred return type meets, when collecting them
    stdx::option<std::vector<gsl::not_null<gs::type*>>> collected_returns_;
    // The `@branchHint` leading each body being analyzed, innermost last
    std::vector<stdx::option<inst_id>> body_hints_;
    // What `@setRuntimeSafety` set in each body being analyzed, innermost last
    std::vector<stdx::option<bool>> safety_scopes_;
    // The known values of temporaries built at runtime, which only vector folds read
    ankerl::unordered_dense::map<usize, built_temp> built_values_;
    // The segment each call returning a runtime reference ended in
    ankerl::unordered_dense::map<inst_id, gir::segment_id> reference_segments_;
    // Calls an `if` tests, which fold when they can run at compile time
    ankerl::unordered_dense::set<inst_id> tested_calls_;
    ankerl::unordered_dense::set<inst_id> comptime_tests_;
    // The `dyn` call whose arguments start at each instruction, and the entries loaded for them
    ankerl::unordered_dense::map<inst_id, inst_id>   dyn_receivers_;
    ankerl::unordered_dense::map<inst_id, dyn_entry> dyn_entries_;
    // The reference each binary operator read its left side through
    ankerl::unordered_dense::map<inst_id, gir::value> reference_reads_;
    // A binary operator's left side read through a returned reference right after its call
    ankerl::unordered_dense::map<inst_id, std::pair<gir::value, operand>> early_reads_;
    // Compile-time string parameters built on entry, which spilling them reuses
    ankerl::unordered_dense::map<inst_id, gir::value> entry_strings_;
    // The packed fields a nested packed field's place is reached through
    std::vector<place> packed_parents_;
    // The pattern each opaque pattern node was built from, by its AST node
    ankerl::unordered_dense::map<usize, inst_id> opaque_patterns_;
    // Nonzero inside a comptime body of a runtime function, where nothing may emit GIR
    usize comptime_depth_{0};
    // Inside a signature's `@TypeOf`, which reads a runtime parameter's type only
    bool only_types_{false};
    // The values `capture` instructions read, and the type whose namespace holds the body
    std::vector<ip::index> captures_;
    // A closure body's captures read from its environment, by capture slot
    std::vector<stdx::option<operand>> env_values_;
    // The environment the next call of a local generic's instance takes first
    stdx::option<gir::value> call_env_;
    // A closure body's own environment parameter
    stdx::option<gir::value> env_self_;
    // The frame whose compile-time memory a compile-time closure's body may store to
    stdx::option<u32>       env_frame_;
    // The call instruction being analyzed, and the function each call reached
    stdx::option<inst_id>                                           calling_;
    ankerl::unordered_dense::map<inst_id, gsl::not_null<instance*>> callees_;
    // The `dyn` calls whose interface method is `@[discardable]`
    ankerl::unordered_dense::set<inst_id> discardable_calls_;
    stdx::option<nominal&>  scope_;
    stdx::option<gs::type&> self_type_;
    std::string             symbol_prefix_;
    u32                     frame_;
    u32                     runtime_depth_{0};
    // The arms being analyzed that a known condition skips
    u32 untaken_{0};
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
// An integer of any width as the nearest `format` float, rounded once
[[nodiscard]] auto float_of_int(const big_int& value, float_format format) -> f128;
// The low `bits` of an integer's two's complement pattern, read as signed or unsigned
[[nodiscard]] auto wrap_int(const big_int& value, u64 bits, bool is_signed) -> big_int;

// What an array or slice holds
[[nodiscard]] auto element_type_of(gs::type& t) -> stdx::option<gs::type&>;

// Whether `d`'s value is the function literal `func` itself
[[nodiscard]] auto declares_literal(const module_t& m, const decl& d, inst_id func) -> bool;
// Whether an attribute list holds `@[discardable]` with no condition or a `true` one
[[nodiscard]] auto lists_discardable(const module_t& m, gsl::span<const inst_id> attrs) -> bool;

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
