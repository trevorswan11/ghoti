#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <gsl/pointers>
#include <gsl/span>
#include <stdx/function.hh>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/id.hh"
#include "compiler/gir/const_value.hh"
#include "compiler/sema/error.hh"
#include "compiler/sema/type.hh"
#include "support/big_int.hh"

namespace ghoti::mod { struct module_t; } // namespace ghoti::mod

namespace ghoti::sema {

struct context;

namespace pattern {

enum class node_kind : u8 {
    WILDCARD,
    VARIANT,    // a tagged union's field, with one child unless the field is void
    RECORD,     // a struct's fields, or a slice's `len` / `ptr` / bytes
    ARRAY,      // a fixed array's elements
    DEREF,      // looks through a `&T`
    ENUM_VALUE, // a named enum variant
    BOOL_VALUE,
    INT_RANGE, // closed on both ends
    OPAQUE,    // compared whole, in source order: a runtime value or a pointer or type constant
};

struct node;
using node_ptr = stdx::nullable_box<node>;

// Where a sub-pattern looks within its parent's value
struct child {
    usize            index{0};
    std::string_view name{};
    type&            value_type;
    bool             bit_packed{false};
    node_ptr         pattern{};
};

// A name a pattern binds, and what it would bind before the capture's modifier applies
struct capture_site {
    ast::capture_t capture;
    type&          value_type;
    bool           place_is_const{false};
    bool           place_is_bit_packed{false};
};

struct node {
    node_kind                  kind{node_kind::WILDCARD};
    stdx::option<ast::node_id> source{};
    gsl::not_null<type*>       value_type;
    usize                      index{0};
    std::string_view           name{};
    std::vector<child>         children{};
    big_int                    lo{};
    big_int                    hi{};
    bool                       boolean{false};
    std::vector<capture_site>  captures{};
};

// What elaboration needs from the pass running it
struct host {
    // Types a value leaf against its column, reporting why it can't match. Absent once resolved
    stdx::function<stdx::option<diagnostic>(ast::node_id, type&)> resolve_leaf;
    stdx::function<stdx::option<gir::const_value>(ast::node_id)>  fold;
    // Resolves a qualifier such as `Shape` in `Shape.circle(...)`, and gives the type it names
    stdx::function<stdx::option<type&>(ast::node_id)> denoted_type_of;
    // Sizes an array whose length was deferred until now
    stdx::function<type&(type&)> concrete;
};

struct elaboration_context {
    mod::module_t& mod;
    context&       ctx;
    const host&    callbacks;
    u32            target_ptr_bits;
};

// Gives `pattern` its meaning against a scrutinee of `scrutinee_type`
[[nodiscard]] auto elaborate(const elaboration_context& cx,
                             ast::node_id               pattern,
                             type&                      scrutinee_type,
                             bool place_is_const) -> stdx::result<node_ptr, diagnostic>;

// One alternative of one arm, tried in the order given
struct row_input {
    usize       arm{0};
    const node* root;
    bool        guarded{false};
};

// One row per alternative of every elaborated arm, in the order they're tried: `_` always last
[[nodiscard]] auto rows_of(const ast::match_expr&                 match,
                           gsl::span<const std::vector<node_ptr>> arm_patterns)
    -> std::vector<row_input>;

// Attaches a post-arrow `=> |v|`: a variant's payload, or the whole value of anything else
auto attach_arm_capture(const ast::capture_t& capture, bool place_is_const, node& root) -> void;

// Whether a tree holds a name bound through a `|...|` in the pattern itself
[[nodiscard]] auto binds_any_name(const node& root) noexcept -> bool;

// Every capture a tree binds, outermost first
[[nodiscard]] auto captures_of(const node& root) -> std::vector<capture_site>;

enum class step_kind : u8 {
    ROOT,
    PAYLOAD,
    FIELD,
    ELEMENT,
    DEREF,
    SLICE_BYTES,
};

// A location inside the scrutinee, reached from its parent by one step
struct occurrence {
    stdx::opt_size       parent{};
    step_kind            step{step_kind::ROOT};
    usize                index{0};
    std::string_view     name{};
    gsl::not_null<type*> value_type;
    bool                 bit_packed{false};
};

struct binding {
    ast::capture_t capture;
    usize          occ{0};
};

enum class test_kind : u8 {
    TAG,
    ENUM,
    BOOL,
    INTERVAL,
};

struct switch_case {
    test_kind                  kind{test_kind::TAG};
    usize                      index{0}; // TAG's field
    bool                       boolean{false};
    big_int                    lo{};
    big_int                    hi{};
    stdx::option<ast::node_id> source{}; // ENUM's variant, as written
    usize                      target{0};
};

enum class decision_kind : u8 {
    FAIL,     // no arm matches: reached at runtime only through a checked-out match
    LEAF,     // binds and takes an arm, or falls through if its guard fails
    SWITCH,   // tests one occurrence against several constructors
    SEQUENCE, // tests one opaque pattern, then either continues or moves on
};

struct decision {
    decision_kind              kind{decision_kind::FAIL};
    usize                      occ{0};
    usize                      arm{0};
    std::vector<binding>       bindings{};
    stdx::opt_size             fallthrough{};
    std::vector<switch_case>   cases{};
    stdx::opt_size             otherwise{}; // none when `cases` cover the occurrence's whole type
    stdx::option<ast::node_id> test{};
    usize                      on_true{0};
    usize                      on_false{0};
};

struct decision_tree {
    std::vector<occurrence> occurrences{};
    std::vector<decision>   nodes{};
    usize                   root{0};
    // Witnesses to what no arm covers: `.ok(.none)`, `0..=9`. `flat_missing` holds only names
    std::vector<std::string> missing;
    bool                     missing_is_flat{true};
    std::vector<bool>        row_reached{};
    bool                     too_complex{false};
};

// Compiles first-match-wins rows into a tree. A row no path reaches can never match, and a `FAIL`
// node a path reaches is a value no row covers.
[[nodiscard]] auto compile(gsl::span<const row_input> rows,
                           type&                      scrutinee_type,
                           u32                        target_ptr_bits) -> decision_tree;

} // namespace pattern

} // namespace ghoti::sema
