#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <fmt/format.h>
#include <gsl/pointers>
#include <gsl/util>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>
#include <stdx/variant.hh>

#include "compiler/ast/ast.hh"
#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/primitive.hh"
#include "compiler/ast/statement.hh"
#include "compiler/ast/type.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/cfg_evaluator.hh"
#include "compiler/sema/error.hh"
#include "compiler/uir/astgen.hh"
#include "compiler/uir/uir.hh"
#include "support/diagnostic.hh"

namespace ghoti::uir::detail {

using decl_info = uir::decl;

enum class binding_kind : u8 {
    VALUE, // read as is: a `let`, local `const`, parameter, or capture
    PLACE, // a `let mut`, read through a load and written through a store
    DECL,  // a module or namespace declaration
    FIELD, // an aggregate field, reachable only through an instance
    LABEL,
};

struct binding {
    std::string_view name{};
    binding_kind     kind{binding_kind::VALUE};
    ref              value{};
    u32              decl{NO_INDEX};
    bool             mutable_decl{false}; // a `let mut` declaration, a place when written
    source_location  at{};
    u64              seq{0}; // declaration order, which a defer copy's view is cut at
};

enum class scope_kind : u8 {
    MODULE,
    NAMESPACE,
    FUNCTION,
    BLOCK,
    TEST,
};

// A name a function or aggregate reads from an enclosing function
struct capture_slot {
    std::string_view name{};
    bool             by_ref{false};
};

struct scope {
    scope_kind       kind{scope_kind::BLOCK};
    bool             is_boundary{false};  // a function, or an aggregate's namespace
    bool             is_aggregate{false}; // members reached through a type, exempt from shadowing
    std::string_view type_name{};         // the aggregate's declared name, which members can't take

    std::vector<binding>                                            bindings;
    ankerl::unordered_dense::map<std::string_view, u32>             index;
    ankerl::unordered_dense::map<std::string_view, source_location> later; // declared further on

    std::vector<capture_slot>                           captures;
    ankerl::unordered_dense::map<std::string_view, u32> capture_index;
    bool                                                moves{false};
    ankerl::unordered_dense::set<std::string_view>      mutated; // assigned or `&mut`-ed inside
};

// A name's binding, and the capture boundaries between it and the use, innermost first
struct lookup_hit {
    binding                            found;
    std::vector<gsl::not_null<scope*>> crossed;
};

struct used_before_declaration {
    source_location declared_at;
};

using lookup_result = stdx::variant<stdx::monostate, lookup_hit, used_before_declaration>;

struct defer_entry {
    ast::node_id stmt;
    bool         is_errdefer{false};
    u64          visible_seq{0}; // bindings declared after the defer stay out of its copies
};

// Where a `break` or `continue` can land
struct jump_target {
    u32                            inst{NO_INDEX};
    stdx::option<std::string_view> label;
    bool                           is_loop{false};
    usize                          defer_height{0};
};

// What a function body tracks while it's lowered
struct function_state {
    bool                     allows_return{false}; // a function body, not a test or comptime decl
    bool                     allows_defer{false};
    bool                     testing{false}; // a test, or a `@[testing]` function
    std::vector<defer_entry> defers{};
    std::vector<jump_target> targets{};
    usize                    target_floor{0}; // targets below it are outside the defer body
    bool                     in_defer{false};
};

// How an expression's result is wanted
enum class use : u8 {
    VALUE,
    PLACE,  // a pointer to it, for a store, `&`, or a method's receiver
    MUTATE, // a place the code writes through, which makes a closure capture it by reference
};

class astgen {
  public:
    astgen(const mod::module_t& mod, const astgen_context& cx);

    [[nodiscard]] auto run() -> lowered;

  private:
    struct place_result {
        ref  value;
        bool is_place{false};
    };

    // A module-level item waiting for its bodies once every name is declared
    struct pending_decl {
        u32          decl;
        ast::node_id node;
    };

    // A name a match arm's patterns bind, in the order the arm first binds it
    struct named_capture {
        std::string_view name{};
        ast::capture_t   capture;
        source_location  at{};
    };

    // The names one arm binds: every alternative shares the slots, but none binds a name twice
    struct pattern_slots {
        ankerl::unordered_dense::set<std::string_view>      alternative;
        ankerl::unordered_dense::map<std::string_view, u32> by_name;
        std::vector<named_capture>                          order;
    };

    // Switches the body new instructions are appended to, restoring it on scope exit
    class body_guard {
      public:
        body_guard(astgen& gen, std::vector<u32>& body) noexcept
            : gen_{gen}, previous_{std::exchange(gen.body_, &body)} {}
        ~body_guard() noexcept { gen_.body_ = previous_; }
        MAKE_PINNED(body_guard)

      private:
        astgen&                          gen_;
        gsl::not_null<std::vector<u32>*> previous_;
    };

    // Marks the arms `comptime` may prune, where an undeclared name is Sema's to report
    class prunable_guard {
      public:
        prunable_guard(astgen& gen, bool active) noexcept : gen_{gen}, active_{active} {
            if (active_) { ++gen_.prunable_depth_; }
        }
        ~prunable_guard() noexcept {
            if (active_) { --gen_.prunable_depth_; }
        }
        MAKE_PINNED(prunable_guard)

      private:
        astgen& gen_;
        bool    active_;
    };

    // Pushes a scope for the guard's lifetime
    class scope_guard {
      public:
        scope_guard(astgen& gen, scope_kind kind) : gen_{gen} { gen.push_scope(kind); }
        ~scope_guard() noexcept { gen_.pop_scope(); }
        MAKE_PINNED(scope_guard)

        [[nodiscard]] auto get() const noexcept -> scope& { return *gen_.scopes_.back(); }

      private:
        astgen& gen_;
    };

  private:
    // astgen.cc: modules, declarations, namespaces
    auto lower_roots() -> void;
    auto declare_module_items(const std::vector<ast::stmt_handle>& items,
                              std::vector<pending_decl>&           pending) -> void;
    auto declare_module_item(ast::stmt_handle item, std::vector<pending_decl>& pending) -> void;
    auto lower_pending(const pending_decl& item) -> void;
    auto lower_decl_bodies(u32 index, const ast::decl_stmt& decl) -> void;
    auto add_root_decl(decl_info d, ast::node_id node, std::vector<pending_decl>& pending) -> u32;
    auto add_value_decl(const ast::decl_stmt& decl, ast::node_id node, u32 owner) -> u32;
    auto add_import_decl(ast::node_id node, const ast::import_stmt& import, u32 owner) -> u32;
    // Loads an imported module, reporting a path or name that doesn't load, and records it
    auto add_import(ast::node_id node, const ast::import_stmt& import) -> u32;
    auto lower_test(u32 index, const ast::test_stmt& test) -> void;
    auto lower_impl(u32 index, const ast::impl_stmt& impl) -> void;
    auto check_top_level_statement(ast::node_id id) -> bool;
    auto check_decl_modifiers(ast::node_id id, const ast::decl_stmt& decl, bool function_local)
        -> void;
    [[nodiscard]] auto decl_flags_of(const ast::decl_stmt& decl) const -> decl_flags;
    // The selected items of a `@cfg` chain, chosen once per node
    [[nodiscard]] auto selected_items(ast::node_id id, const ast::cfg_stmt& cfg)
        -> const std::vector<ast::stmt_handle>&;
    // Statements with each `@cfg` chain replaced by its selected items
    [[nodiscard]] auto spliced(const std::vector<ast::stmt_handle>& items)
        -> std::vector<ast::stmt_handle>;
    template <typename Group, typename Item>
    [[nodiscard]] auto spliced_group(const std::vector<Item>&  items,
                                     const std::vector<Group>& groups) -> std::vector<Item>;
    auto               lower_attributes(const stdx::option<ast::attribute_list>& attributes,
                                        stdx::option<bool>                       routed,
                                        std::vector<u32>&                        into) -> void;
    // Every assignment and `&mut` / `^mut` in the module, by where it is and the name it writes
    auto collect_mutation_sites() -> void;
    // The names a function literal writes, which its closure captures by reference
    [[nodiscard]] auto mutated_within(ast::node_id fn) const
        -> ankerl::unordered_dense::set<std::string_view>;

    // scopes.cc
    auto push_scope(scope_kind kind) -> scope&;
    auto pop_scope() noexcept -> void;
    // `may_shadow` is for names nothing reads, like a bodyless signature's parameters
    auto declare(std::string_view name, source_location at, binding b, bool may_shadow = false)
        -> bool;
    auto               declare_member(std::string_view name, source_location at, binding b) -> bool;
    auto               announce_later(const std::vector<ast::stmt_handle>& statements) -> void;
    [[nodiscard]] auto lookup(std::string_view name) -> lookup_result;
    [[nodiscard]] auto shadowed(std::string_view name) const -> stdx::option<source_location>;
    [[nodiscard]] auto next_seq() noexcept -> u64 { return ++seq_; }
    [[nodiscard]] auto visible(const binding& b) const noexcept -> bool;
    // A captured name as the innermost boundary sees it, adding the slots it needs on the way
    [[nodiscard]] auto capture_ref(const lookup_hit& hit, use u, source src) -> ref;
    [[nodiscard]] auto capture_slot_of(scope& boundary, std::string_view name, bool by_ref) -> u32;
    [[nodiscard]] auto in_function() const noexcept -> bool;
    [[nodiscard]] auto in_test() const noexcept -> bool;
    // The captures `boundary` collected, as its enclosing scope sees each one
    [[nodiscard]] auto outer_captures(const scope& boundary, source src) -> std::vector<u32>;

    // astgen_expr.cc
    [[nodiscard]] auto expr(ast::expr_handle h) -> ref;
    [[nodiscard]] auto place(ast::expr_handle h, use u) -> place_result;
    [[nodiscard]] auto identifier(source src, source_location at, std::string_view name, use u)
        -> place_result;
    [[nodiscard]] auto identifier_node(ast::node_id id, use u) -> place_result;
    [[nodiscard]] auto primitive_named(std::string_view name, syntax::token_type_t type, source src)
        -> stdx::option<ref>;
    [[nodiscard]] auto int_literal(ast::node_id id) -> ref;
    [[nodiscard]] auto call(source src, source_location at, const ast::call_expr& node) -> ref;
    [[nodiscard]] auto lower_node(ast::node_id id, const ast::binary_expr& node) -> ref;
    [[nodiscard]] auto lower_node(ast::node_id id, const ast::assignment_expr& node) -> ref;
    [[nodiscard]] auto lower_node(ast::node_id id, const ast::call_expr& node) -> ref;
    [[nodiscard]] auto lower_node(ast::node_id id, const ast::dot_expr& node) -> ref;
    [[nodiscard]] auto lower_node(ast::node_id id, const ast::index_expr& node) -> ref;
    [[nodiscard]] auto lower_node(ast::node_id id, const ast::range_expr& node) -> ref;
    [[nodiscard]] auto lower_node(ast::node_id id, const ast::initializer_expr& node) -> ref;
    [[nodiscard]] auto lower_node(ast::node_id id, const ast::array_expr& node) -> ref;
    [[nodiscard]] auto lower_node(ast::node_id id, const ast::if_expr& node) -> ref;
    [[nodiscard]] auto lower_node(ast::node_id id, const ast::match_expr& node) -> ref;
    [[nodiscard]] auto lower_node(ast::node_id id, const ast::label_expr& node) -> ref;
    [[nodiscard]] auto lower_node(ast::node_id id, const ast::while_loop_expr& node) -> ref;
    [[nodiscard]] auto lower_node(ast::node_id id, const ast::do_while_loop_expr& node) -> ref;
    [[nodiscard]] auto lower_node(ast::node_id id, const ast::infinite_loop_expr& node) -> ref;
    [[nodiscard]] auto lower_node(ast::node_id id, const ast::for_loop_expr& node) -> ref;
    [[nodiscard]] auto lower_node(ast::node_id id, const ast::unwrap_expr& node) -> ref;
    [[nodiscard]] auto lower_node(ast::node_id id, const ast::asm_expr& node) -> ref;
    [[nodiscard]] auto lower_node(ast::node_id id, const ast::cfg_value_expr& node) -> ref;
    [[nodiscard]] auto lower_node(ast::node_id id, const ast::function_expr& node) -> ref;
    [[nodiscard]] auto call_args(const ast::call_expr& call)
        -> std::pair<std::vector<ref>, std::vector<u32>>;
    [[nodiscard]] auto builtin_call(source                src,
                                    ast::node_id          callee,
                                    syntax::builtin_t     builtin,
                                    const ast::call_expr& node) -> ref;
    // A body yielding what an `if` / `match` / loop `else` arm evaluates to
    [[nodiscard]] auto arm_value(ast::stmt_handle arm) -> std::vector<u32>;
    auto               bind_capture(const ast::capture_t& capture, u32 slot, source src) -> void;
    [[nodiscard]] auto pattern(ast::node_id id, pattern_slots& slots) -> ref;
    // A body yielding the expression's value
    [[nodiscard]] auto value_body(ast::expr_handle h) -> std::vector<u32>;
    // Reserves a loop and makes it the innermost jump target, taking any pending label
    [[nodiscard]] auto begin_loop(ast::node_id id, bool is_loop) -> u32;
    [[nodiscard]] auto loop_label() const -> std::string_view;

    // astgen_stmt.cc
    // Returns true when the statement never falls through
    auto               stmt(ast::stmt_handle h) -> bool;
    auto               block_statements(const std::vector<ast::stmt_handle>& statements) -> bool;
    auto               local_decl(ast::node_id id, const ast::decl_stmt& decl) -> void;
    auto               break_stmt(ast::node_id id, const ast::break_stmt& node) -> void;
    auto               continue_stmt(ast::node_id id, const ast::continue_stmt& node) -> void;
    auto               return_stmt(ast::node_id id, const ast::return_stmt& node) -> void;
    [[nodiscard]] auto find_target(ast::node_id                                id,
                                   const stdx::option<ast::identifier_handle>& label,
                                   bool                                        loops_only,
                                   std::string_view stmt_name) -> stdx::option<jump_target>;
    // A body of statements in its own scope, as a block construct runs it
    [[nodiscard]] auto scoped_block(ast::block_handle h, scope_kind kind = scope_kind::BLOCK)
        -> std::vector<u32>;
    // A block's statements declared into the innermost scope, which the caller owns
    [[nodiscard]] auto block_in_current_scope(ast::block_handle h) -> std::vector<u32>;

    // astgen_defer.cc
    auto push_defer(ast::node_id id, bool is_errdefer) -> void;
    // Copies every defer above `height`, innermost first; failure paths take errdefers too
    auto               emit_defers(usize height, bool on_failure, ref residual) -> void;
    auto               emit_defer(const defer_entry& entry, bool on_failure, ref residual) -> void;
    auto               emit_return(ast::node_id id, ref value) -> void;
    [[nodiscard]] auto has_errdefers(usize height) const -> bool;

    // astgen_types.cc
    [[nodiscard]] auto type(ast::explicit_type_id id) -> ref;
    [[nodiscard]] auto type_inner(ast::explicit_type_id id) -> ref;
    [[nodiscard]] auto type_body(ast::explicit_type_id id) -> std::vector<u32>;
    [[nodiscard]] auto fn_type(ast::explicit_type_id id, const ast::explicit_function_type& fn)
        -> ref;
    // Pushes an aggregate's namespace scope and reserves its instruction
    [[nodiscard]] auto begin_aggregate(source src) -> u32;
    // Pops the namespace scope, returning its captures and what each is outside it
    [[nodiscard]] auto finish_aggregate(source src)
        -> std::pair<std::vector<capture_slot>, std::vector<u32>>;
    auto               write_captures(record_writer&                   record,
                                      const std::vector<capture_slot>& captures,
                                      const std::vector<u32>&          outer) -> void;
    [[nodiscard]] auto struct_decl(source src, const ast::struct_expr& node) -> ref;
    [[nodiscard]] auto union_decl(source src, const ast::union_expr& node) -> ref;
    [[nodiscard]] auto enum_decl(source src, const ast::enum_expr& node) -> ref;
    [[nodiscard]] auto interface_decl(source src, const ast::interface_expr& node) -> ref;
    // Declares and lowers an aggregate's members into the namespace owned by `inst`
    [[nodiscard]] auto namespace_members(u32 inst, const ast::member_list& members)
        -> std::vector<u32>;
    auto lower_function(ast::node_id                        id,
                        const ast::function_expr&           fn,
                        stdx::option<const ast::decl_stmt&> decl = stdx::none) -> ref;
    auto lower_param(const ast::function_expr::parameter& param,
                     u32                                  extra_flags,
                     bool                                 bodyless = false) -> ref;

    // Instruction emission
    auto add(tag t, data d, source src) -> ref;
    auto add_un(tag t, ref operand, u32 aux, source src) -> ref;
    auto add_bin(tag t, ref lhs, ref rhs, source src) -> ref;
    auto add_str(tag t, std::string_view text, source src) -> ref;
    auto add_pl(tag t, record_writer& record, source src) -> ref;
    auto add_pl(tag t, record_writer&& record, source src) -> ref { return add_pl(t, record, src); }
    // A placeholder bodies can refer to before its record is known; `fill_pl` adds it to the
    // current body, after the operands it was waiting on
    [[nodiscard]] auto reserve(source src) -> u32;
    auto               fill_pl(u32 inst, tag t, record_writer& record) -> void;
    auto fill_pl(u32 inst, tag t, record_writer&& record) -> void { fill_pl(inst, t, record); }
    [[nodiscard]] auto writer() -> record_writer { return record_writer{out_}; }
    auto               yields(ref value, source src) -> void;

    template <typename... Args>
    auto
    report(source_location at, sema::error code, fmt::format_string<Args...> spec, Args&&... args)
        -> void {
        diags_.emplace_back(fmt::format(spec, std::forward<Args>(args)...), code, at);
    }
    [[nodiscard]] auto location_of(ast::node_id id) const -> source_location {
        return ast_.location_of(id);
    }
    [[nodiscard]] auto name_of(ast::identifier_handle h) const -> std::string_view;
    // Where an import's name is declared: its alias, or the statement when it has none
    [[nodiscard]] auto import_location(ast::node_id id, const ast::import_stmt& import) const
        -> source_location;

  private:
    const mod::module_t&  mod_;
    const ast::AST&       ast_;
    const astgen_context& cx_;
    module                out_;
    sema::diagnostics     diags_;
    sema::cfg_evaluator   cfg_;

    std::vector<u32>                 root_body_; // instructions outside any body, never read
    gsl::not_null<std::vector<u32>*> body_{&root_body_};
    std::vector<stdx::box<scope>>    scopes_;
    std::vector<function_state>      functions_;
    u64                              seq_{0};
    std::vector<std::pair<u64, u64>> hidden_; // seq ranges a defer copy can't see
    ankerl::unordered_dense::map<usize, stdx::option<usize>>  cfg_arms_;
    std::vector<std::pair<source_location, std::string_view>> mutation_sites_;
    std::string_view pending_type_name_;           // the declaration an aggregate value initializes
    stdx::option<std::string_view> pending_label_; // a label waiting for the loop it names
    usize                          prunable_depth_{0}; // inside arms `comptime` may prune
};

} // namespace ghoti::uir::detail
