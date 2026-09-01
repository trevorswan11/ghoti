#pragma once

#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>
#include <gsl/pointers>
#include <gsl/span>
#include <stdx/assert.hh>
#include <stdx/iterator.hh>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/type_traits.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>
#include <stdx/variant.hh>

#include "compiler/ast/expression.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/error.hh"
#include "compiler/syntax/token.hh"
#include "compiler/syntax/token_type.hh"
#include "support/diagnostic.hh"
#include "support/scope_guard.hh"

namespace ghoti::sema {

class type;
class symbol;

enum class symbol_kind : u8 {
    TYPE,
    VALUE,
    CALLABLE,
    MODULE,
    LABEL,
    POISONED,
};

enum class symbol_status : u8 {
    UNRESOLVED,
    RESOLVING,
    RESOLVED,
};

namespace symbols {

// A non-AST-based symbol for builtin types and functions
//
// Holds its own semantic type since it cannot be stored in an AST side table
class builtin {
  public:
    explicit builtin(const syntax::typed_identifier& tok, type& type) noexcept
        : token_{tok}, type_{type} {}

    MAKE_GETTER(token, const syntax::token_t&)
    MAKE_GETTER(type, type&)

  private:
    syntax::token_t token_;
    type&           type_;
};

using node_t = ast::handle<ast::node_kind::DECL_STATEMENT,
                           ast::node_kind::USING_STATEMENT,
                           ast::node_kind::IMPORT_STATEMENT>;

class label {
  public:
    using handle_t = ast::handle<ast::node_kind::LABEL_EXPRESSION>;

  public:
    explicit label(handle_t label) noexcept : definition_{label} {}

    MAKE_GETTER(definition, handle_t)
    MAKE_GETTER(yield_types, gsl::span<const gsl::not_null<type*>>)

    [[nodiscard]] auto has_yield_types() const noexcept -> bool { return !yield_types_.empty(); }
    auto               add_yield_type(type& type) -> void { yield_types_.emplace_back(&type); }

    // Gets the Label data from the symbol, asserting the underlying data is a label
    [[nodiscard]] static auto from(symbol& symbol) -> label&;

  private:
    handle_t                          definition_;
    std::vector<gsl::not_null<type*>> yield_types_;
};

using match_capture    = ast::identifier_handle;
using struct_field     = ast::struct_expr::field;
using union_field      = ast::union_expr::field;
using enumeration      = ast::enum_expr::enumeration;
using self_parameter   = ast::self_parameter;
using parameter        = ast::function_expr::parameter;
using for_loop_capture = ast::for_loop_expr::capture;

} // namespace symbols

class symbol {
  public:
    using data_t = stdx::variant<symbols::builtin,
                                 symbols::node_t,
                                 symbols::label,
                                 symbols::match_capture,
                                 symbols::struct_field,
                                 symbols::union_field,
                                 symbols::enumeration,
                                 symbols::self_parameter,
                                 symbols::parameter,
                                 symbols::for_loop_capture>;

  public:
    symbol(std::string_view name, data_t data) noexcept : name_{name}, data_{std::move(data)} {}
    ~symbol() = default;
    MAKE_MOVE_CONSTRUCTABLE_ONLY(symbol);

    MAKE_GETTER(name, std::string_view)
    MAKE_DEDUCING_GETTER(data)

    [[nodiscard]] auto get_symbol_location(const mod::module& module) const noexcept
        -> source_location;

    // Like `get_symbol_location`, but a decl_stmt resolves to its declared name's span rather
    // than the whole statement's; used for LSP go-to-definition/rename ranges
    [[nodiscard]] auto get_symbol_span(const mod::module& module) const noexcept -> source_span;

    // Can only be true for decls, imports, and type aliases
    [[nodiscard]] auto is_public(const mod::module& module) const noexcept -> bool;

    [[nodiscard]] auto get_status() const noexcept -> symbol_status { return status_; }
    auto               set_status(symbol_status status) noexcept -> void { status_ = status; }

    [[nodiscard]] auto has_kind() const noexcept -> bool { return kind_.has_value(); }
    [[nodiscard]] auto get_kind_opt() const noexcept -> stdx::option<symbol_kind> { return kind_; }
    [[nodiscard]] auto get_kind() const noexcept -> symbol_kind { return *get_kind_opt(); }
    auto               set_kind(symbol_kind kind) noexcept -> void { kind_ = kind; }

  private:
    std::string_view          name_;
    data_t                    data_;
    symbol_status             status_{symbol_status::UNRESOLVED};
    stdx::option<symbol_kind> kind_;
};

class symbol_table {
  public:
    // The managed value type in mapped to by inserted keys
    struct value_proxy {
        symbol symbol;
        usize  idx;
    };

    // Used for returning pseudo reference types from get operations, avoided in iterators
    template <typename Self> struct reference_proxy {
        stdx::const_dispatch_t<Self, symbol>& symbol;
        usize                                 index;
    };

    using table = ankerl::unordered_dense::map<std::string_view, value_proxy>;
    MAKE_UNALIASED_ITERATOR(table, symbols_)
    using kv = table::iterator::value_type;

  public:
    symbol_table() noexcept = default;
    ~symbol_table()         = default;
    MAKE_MOVE_CONSTRUCTABLE_ONLY(symbol_table);

    // Constructs the symbolic node in place with the provided args
    template <typename T, typename... Args>
    auto insert(std::string_view name, const mod::module& module, Args&&... args)
        -> stdx::result<void, diagnostic> {
        return insert(name, module, symbol::data_t{T{std::forward<Args>(args)...}});
    }

    // Checks that the module was inserted without collision
    auto insert(std::string_view name, const mod::module& module, const symbol::data_t& data)
        -> stdx::result<void, diagnostic>;

    // For use of prelude injection only
    auto insert_unchecked(std::string_view name, const symbol::data_t& data) -> void;

    auto reserve(usize cap) -> void { symbols_.reserve(cap); }

    [[nodiscard]] auto has(std::string_view name) const noexcept -> bool {
        return symbols_.contains(name);
    }

    // Differs from `get_proxy_opt` by asserting that the name is present.
    template <typename Self>
    [[nodiscard]] auto get_proxy(this Self&& self, std::string_view name) noexcept {
        auto it{self.symbols_.find(name)};
        ASSERT(it != self.symbols_.end(), "Illegal get on missing key");
        return reference_proxy<Self>{it->second.symbol, it->second.idx};
    }

    // Returns optional referential metadata of the symbol if present
    template <typename Self>
    [[nodiscard]] auto get_proxy_opt(this Self&& self, std::string_view name) noexcept
        -> stdx::option<reference_proxy<Self>> {
        auto it{self.symbols_.find(name)};
        if (it == self.symbols_.end()) { return stdx::none; }
        return reference_proxy<Self>{it->second.symbol, it->second.idx};
    }

    // Differs from `get_opt` by asserting that the name is present.
    [[nodiscard]] auto get(this auto&& self, std::string_view name) noexcept -> auto& {
        return self.get_proxy(name).symbol;
    }

    // Returns an optional containing a mutable or const reference to a symbol depending on context.
    template <typename Self>
    [[nodiscard]] auto get_opt(this Self&& self, std::string_view name) noexcept
        -> stdx::option<stdx::const_dispatch_t<Self, symbol>&> {
        const auto proxy{self.get_proxy_opt(name)};
        if (!proxy) { return stdx::none; }
        return proxy->symbol;
    }

  private:
    table symbols_;
};

class symbol_table_stack {
  public:
    // A basic push/pop RAII guard, see `Scope`
    using guard = scope_guard<symbol_table_stack>;

    // An extension of `Guard` that also resets the old index upon destruction
    class scope {
      public:
        scope(symbol_table_stack& s, usize new_idx, usize& old_idx) noexcept
            : guard_{s, new_idx}, idx_ref_{old_idx}, old_idx_{old_idx} {
            idx_ref_ = new_idx;
        }
        ~scope() { idx_ref_ = old_idx_; }

      private:
        symbol_table_stack::guard guard_;
        usize&                    idx_ref_;
        usize                     old_idx_;
    };

    MAKE_ITERATOR(stack, std::vector<usize>, stack_)

  public:
    symbol_table_stack() noexcept = default;
    ~symbol_table_stack()         = default;
    MAKE_MOVE_CONSTRUCTABLE_ONLY(symbol_table_stack);

    auto push(usize idx) -> void { stack_.emplace_back(idx); }
    auto pop() noexcept -> void {
        if (!stack_.empty()) { stack_.pop_back(); }
    }

  private:
    stack stack_;
};

class symbol_table_registry {
  public:
    MAKE_ITERATOR(tables, std::vector<symbol_table>, tables_)

  public:
    symbol_table_registry() noexcept = default;
    ~symbol_table_registry()         = default;

    MAKE_MOVE_CONSTRUCTABLE_ONLY(symbol_table_registry)

    [[nodiscard]] auto create() -> usize {
        tables_.emplace_back();
        return tables_.size() - 1;
    }

    // Constructs the symbolic node in place with the provided args
    template <typename T, typename... Args>
    [[nodiscard]] auto
    insert_into(usize table_idx, const mod::module& module, std::string_view name, Args&&... args)
        -> stdx::result<void, diagnostic> {
        return insert_into(table_idx, module, name, symbol::data_t{T{std::forward<Args>(args)...}});
    }

    [[nodiscard]] auto insert_into(usize                 table_idx,
                                   const mod::module&    module,
                                   std::string_view      name,
                                   const symbol::data_t& data) -> stdx::result<void, diagnostic>;

    [[nodiscard]] auto get(this auto&& self, usize idx) noexcept -> auto& {
        ASSERT(idx < self.tables_.size(), "Index out of range");
        return self.tables_[idx];
    }

    template <typename Self>
    [[nodiscard]] auto get_opt(this Self&& self, usize idx) noexcept
        -> stdx::option<stdx::const_dispatch_t<Self, symbol_table>&> {
        if (idx >= self.tables_.size()) { return stdx::none; }
        return self.get(idx);
    }

    [[nodiscard]] auto get_from(this auto&& self, usize idx, std::string_view name) -> auto& {
        ASSERT(idx < self.tables_.size(), "Index out of range");
        return self.tables_[idx].get(name);
    }

    template <typename Self>
    [[nodiscard]] auto get_from_opt(this Self&& self, usize idx, std::string_view name) noexcept
        -> stdx::option<stdx::const_dispatch_t<Self, symbol>&> {
        if (idx >= self.tables_.size()) { return stdx::none; }
        return self.tables_[idx].get_opt(name);
    }

    // Looks up all levels of the stack for possible illegal shadowing of the name
    [[nodiscard]] auto is_shadowing(const symbol_table_stack& stack,
                                    const mod::module&        module,
                                    std::string_view          name,
                                    const symbol::data_t&     data) noexcept
        -> stdx::result<void, diagnostic>;

    // Looks up all levels of the stack for the queried name
    template <typename Self>
    [[nodiscard]] auto
    lookup(this Self&& self, const symbol_table_stack& stack, std::string_view name) noexcept
        -> stdx::option<stdx::const_dispatch_t<Self, symbol>&> {
        for (const auto idx : stack | std::views::reverse) {
            if (auto symbol{self.tables_[idx].get_opt(name)}) { return symbol; }
        }
        return stdx::none;
    }

    template <typename Self> struct depth_result {
        stdx::const_dispatch_t<Self, symbol>& symbol;
        usize                                 depth;
    };

    template <typename Self> struct table_result {
        stdx::const_dispatch_t<Self, symbol>& symbol;
        usize                                 table_idx;
    };

    // Like `lookup`, but also reports the absolute registry index of the table it was found in
    template <typename Self>
    [[nodiscard]] auto lookup_with_table(this Self&&               self,
                                         const symbol_table_stack& stack,
                                         std::string_view          name) noexcept
        -> stdx::option<table_result<Self>> {
        for (const auto idx : stack | std::views::reverse) {
            if (auto symbol{self.tables_[idx].get_opt(name)}) {
                return table_result<Self>{*symbol, idx};
            }
        }
        return stdx::none;
    }

    // Identical to `lookup`, but also reports the resolving stack depth
    template <typename Self>
    [[nodiscard]] auto lookup_with_depth(this Self&&               self,
                                         const symbol_table_stack& stack,
                                         std::string_view          name) noexcept
        -> stdx::option<depth_result<Self>> {
        usize depth{stack.size()};
        for (const auto idx : stack | std::views::reverse) {
            depth -= 1;
            if (auto symbol{self.tables_[idx].get_opt(name)}) {
                return depth_result<Self>{*symbol, depth};
            }
        }
        return stdx::none;
    }

  private:
    tables tables_;
};

} // namespace ghoti::sema
