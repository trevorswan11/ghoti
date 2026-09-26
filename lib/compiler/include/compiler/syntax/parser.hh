#pragma once

#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gsl/span>
#include <stdx/option.hh>
#include <stdx/result.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/arena.hh"
#include "compiler/ast/ast.hh"
#include "compiler/ast/handle.hh"
#include "compiler/ast/id.hh"
#include "compiler/ast/kind.hh"
#include "compiler/syntax/error.hh"
#include "compiler/syntax/lexer.hh"
#include "compiler/syntax/precedence.hh"
#include "compiler/syntax/token.hh"
#include "compiler/syntax/token_type.hh"
#include "support/counter.hh"
#include "support/diagnostic.hh"

namespace ghoti::ast { class AST; } // namespace ghoti::ast

namespace ghoti::syntax {

class parser {
  public:
    using prefix_fn = stdx::result<ast::expr_handle, diagnostic> (*)(parser&);
    using infix_fn  = stdx::result<ast::expr_handle, diagnostic> (*)(parser&, ast::expr_handle);

    class checkpoint {
      public:
        explicit checkpoint(const parser& parser) noexcept
            : snapshot_{parser.lexer_}, current_{parser.current_token_}, peek_{parser.peek_token_},
              pending_docs_len_{parser.pending_docs_.size()},
              pending_module_docs_len_{parser.pending_module_docs_.size()} {}

      private:
        lexer::snapshot snapshot_;
        token_t         current_;
        token_t         peek_;
        usize           pending_docs_len_;
        usize           pending_module_docs_len_;

        friend class parser;
    };

    // An RAII checkpoint-rollback transaction
    class transaction {
      public:
        explicit transaction(parser& parser) noexcept : parser_{parser}, checkpoint_{parser} {}
        ~transaction() {
            if (!committed_) { parser_.rollback(checkpoint_); }
        }

        // Prevent a rollback from happening at the end of the transaction
        void commit() noexcept { committed_ = true; }

      private:
        parser&    parser_;
        checkpoint checkpoint_;
        bool       committed_{false};
    };

    // Per function literal: the names its compile-time positions read, for implicit `constexpr`
    // params in a `constexpr`-declared function (a nested literal gets its own frame)
    struct constexpr_param_frame {
        bool                          infer{false};
        u32                           compile_time_depth{0};
        std::vector<std::string_view> compile_time_names;
    };

    class function_scope {
      public:
        explicit function_scope(parser& p) : parser_{p} {
            p.constexpr_param_frames_.emplace_back(
                constexpr_param_frame{.infer = std::exchange(p.infer_next_fn_params_, false),
                                      .compile_time_depth = 0,
                                      .compile_time_names = {}});
        }
        ~function_scope() { parser_.constexpr_param_frames_.pop_back(); }
        MAKE_PINNED(function_scope);

        [[nodiscard]] auto frame() const noexcept -> const constexpr_param_frame& {
            return parser_.constexpr_param_frames_.back();
        }

      private:
        parser& parser_;
    };

    // Marks a position evaluated at compile time: an `if`/`match`/`for`/`while constexpr` header,
    // a `constexpr` block or label, or a `constexpr` declaration's initializer
    class compile_time_scope {
      public:
        compile_time_scope(parser& p, bool enabled) noexcept : parser_{p} {
            if (enabled && !p.constexpr_param_frames_.empty()) {
                frame_ = p.constexpr_param_frames_.size() - 1;
                ++p.constexpr_param_frames_[*frame_].compile_time_depth;
            }
        }
        ~compile_time_scope() {
            if (frame_) { --parser_.constexpr_param_frames_[*frame_].compile_time_depth; }
        }
        MAKE_PINNED(compile_time_scope);

      private:
        parser&        parser_;
        stdx::opt_size frame_;
    };

  public:
    parser() noexcept = default;
    explicit parser(std::string_view input) noexcept { reset(input); }

    auto reset(std::string_view input = {}) noexcept -> void;

    // Advances the parser, returning the resulting current token.
    // This is a no-op at end of stream.
    auto advance(u8 times = 1) noexcept -> const token_t&;

    // Fills the AST with the parser's output, clearing it before use
    auto consume(ast::AST& ast, ghoti::arena& arena) -> diagnostics;

    [[nodiscard]] auto get_current_token() const noexcept -> const token_t& {
        return current_token_;
    }
    [[nodiscard]] auto get_peek_token() const noexcept -> const token_t& { return peek_token_; }

    [[nodiscard]] auto current_token_is(token_type_t t) const noexcept -> bool {
        return current_token_.type == t;
    }
    [[nodiscard]] auto peek_token_is(token_type_t t) const noexcept -> bool {
        return peek_token_.type == t;
    }

    // Advances the cursor tokens only if the expected token type matches the actual peek token.
    [[nodiscard]] auto expect_peek(token_type_t expected) -> stdx::result<void, diagnostic>;

    // Checks for a semicolon in either the current or peak token and advances state accordingly
    //
    // Only use this over `expect_peek` when a potentially-block expr has just been parsed
    [[nodiscard]] auto expect_semicolon() -> stdx::result<void, diagnostic>;

    // Indiscriminately returns an error citing the peek token.
    [[nodiscard]] auto peek_error(token_type_t expected) -> diagnostic;

    [[nodiscard]] auto get_current_precedence() const noexcept
        -> std::pair<bind_precedence, stdx::option<binding>>;
    [[nodiscard]] auto get_peek_precedence() const noexcept
        -> std::pair<bind_precedence, stdx::option<binding>>;

    [[nodiscard]] auto parse_statement(semicolon_behavior behavior = semicolon_behavior::REQUIRE)
        -> stdx::result<ast::stmt_handle, diagnostic>;
    [[nodiscard]] auto parse_expression(bind_precedence precedence = bind_precedence::LOWEST)
        -> stdx::result<ast::expr_handle, diagnostic>;

    // Assumes that the current token is looking at the start of the expression.
    // The resulting statement can only be a jump, block, or expression statement.
    [[nodiscard]] auto
    parse_restricted_statement(error              error,
                               semicolon_behavior behavior = semicolon_behavior::REQUIRE)
        -> stdx::result<ast::stmt_handle, diagnostic>;

    // Parses a restricted statement only if an else token is currently looked at.
    [[nodiscard]] auto
    try_parse_restricted_alternate(error              error,
                                   semicolon_behavior behavior = semicolon_behavior::REQUIRE)
        -> stdx::result<stdx::option<ast::stmt_handle>, diagnostic>;

    // Parses the `( <predicate> )` of a `@cfg` arm. Assumes the current token is `@cfg`
    // and leaves the current token on the `)`
    [[nodiscard]] auto parse_cfg_predicate() -> stdx::result<ast::expr_handle, diagnostic>;

    static auto get_prefix_fn_opt(token_type_t tt) noexcept -> stdx::option<prefix_fn>;
    static auto get_poll_infix_fn_opt(token_type_t tt) noexcept -> stdx::option<infix_fn>;

    [[nodiscard]] auto get_ast() noexcept -> ast::AST& { return *ast_; }

    // Attaches any pending `///` lines sitting directly above `name` and below `floor_line`
    auto attach_member_doc(ast::identifier_handle name, usize floor_line) -> void;

    template <ast::NodeData N> [[nodiscard]] constexpr auto get_node(ast::node_id id) -> const N& {
        return ast_->get_as<N>(id);
    }

    [[nodiscard]] auto get_location_of(ast::node_id id) -> source_location;
    [[nodiscard]] auto get_location_of(ast::explicit_type_id id) -> source_location;

    // Adds an expression to the ast and returns its handle
    template <ast::NodeData Data, typename... Args>
    [[nodiscard]] constexpr auto add_expr(const syntax::token_t& start_token, Args&&... args) {
        return add_node<ast::expr_handle, Data>(start_token, std::forward<Args>(args)...);
    }

    // For infix expressions whose span starts before the operator token that tags their node_id
    template <ast::NodeData Data, typename... Args>
    [[nodiscard]] constexpr auto
    add_expr(const source_location& span_start, const syntax::token_t& tag_token, Args&&... args) {
        return add_node<ast::expr_handle, Data>(span_start, tag_token, std::forward<Args>(args)...);
    }

    // Adds a statement to the ast and returns its handle
    template <ast::NodeData Data, typename... Args>
    [[nodiscard]] constexpr auto add_stmt(const syntax::token_t& start_token, Args&&... args) {
        return add_node<ast::stmt_handle, Data>(start_token, std::forward<Args>(args)...);
    }

    // Adds a node to the ast and casts the result to the requested handle. `current_token_` is
    // the last token consumed for this node, since callers always add a node as their final step
    template <typename Handle, ast::NodeData Data, typename... Args>
    [[nodiscard]] constexpr auto add_node(const syntax::token_t& start_token, Args&&... args) {
        return Handle{
            ast_->add_node(start_token, current_token_, Data{std::forward<Args>(args)...})};
    }

    template <typename Handle, ast::NodeData Data, typename... Args>
    [[nodiscard]] constexpr auto
    add_node(const source_location& span_start, const syntax::token_t& tag_token, Args&&... args) {
        return Handle{ast_->add_node(
            span_start, tag_token, current_token_, Data{std::forward<Args>(args)...})};
    }

    // Helper for type-ast insertion, reducing a layer of call-site indirection
    template <ast::ExplicitTypeData Data, typename... Args>
    [[nodiscard]] constexpr auto
    add_type(const syntax::token_t& start_token, ast::type_modifier mod, Args&&... args) {
        return ast_->add_type(start_token, current_token_, mod, Data{std::forward<Args>(args)...});
    }

    // Records that `id` was wrapped in a `( )` pair the parser is about to discard.
    auto mark_parenthesized(ast::node_id id) -> void { ast_->add_parenthesization(id); }

    // `explicit_type::parse` stashes the interface list of an `impl I` / `impl (A + B)` parameter
    // type here; `function_expr::parse` picks it up right after and associates it with the param.
    auto set_pending_impl_bound(std::vector<ast::explicit_type_id> interfaces) -> void {
        pending_impl_bound_.emplace(std::move(interfaces));
    }

    [[nodiscard]] auto take_pending_impl_bound()
        -> stdx::option<std::vector<ast::explicit_type_id>> {
        auto out{std::move(pending_impl_bound_)};
        pending_impl_bound_.reset();
        return out;
    }

    // The next function literal belongs to a `constexpr` declaration
    auto arm_constexpr_param_inference() noexcept -> void { infer_next_fn_params_ = true; }

    // Inside `@TypeOf(x)` and friends only `x`'s type is read, never its value
    [[nodiscard]] auto enter_type_only_operand() noexcept -> counter<u32>::guard {
        return counter<u32>::guard{type_only_depth_};
    }

    auto note_identifier_reference(std::string_view name) -> void {
        if (constexpr_param_frames_.empty() || type_only_depth_) { return; }
        auto& frame{constexpr_param_frames_.back()};
        if (frame.infer && frame.compile_time_depth > 0) {
            frame.compile_time_names.emplace_back(name);
        }
    }

    [[nodiscard]] auto in_test_block() const noexcept -> bool { return test_block_depth_; }
    [[nodiscard]] auto enter_test_block() noexcept -> counter<u32>::guard {
        return counter<u32>::guard{test_block_depth_};
    }

  private:
    // Bounds recursive-descent depth so deep nesting reports a diagnostic, not a stack overflow.
    static constexpr u32 MAX_EXPRESSION_DEPTH{512};
    using depth_counter = counter<u32>;
    using depth_guard   = depth_counter::guard;

    // A `///` doc line awaiting attachment to the next declaration.
    struct pending_doc {
        std::string_view text;
        usize            line;
    };

  private:
    // Reverts the parser to the state from the checkpoint.
    auto rollback(const checkpoint& checkpoint) noexcept -> void {
        lexer_.restore(checkpoint.snapshot_);
        current_token_ = checkpoint.current_;
        peek_token_    = checkpoint.peek_;
        pending_docs_.resize(checkpoint.pending_docs_len_);
        pending_module_docs_.resize(checkpoint.pending_module_docs_len_);
    }

    // Like `lexer_.advance()` but siphons off `///` / `//!` comments into the pending-doc
    // buffers and skips plain comments, so callers only see significant tokens.
    [[nodiscard]] auto next_significant_token() noexcept -> token_t;

    // Joins pending doc lines into one `\n`-separated block.
    [[nodiscard]] static auto join_docs(gsl::span<const pending_doc> lines) -> std::string {
        return fmt::to_string(fmt::join(lines | std::views::transform(&pending_doc::text), "\n"));
    }

  private:
    std::string_view        input_;
    lexer                   lexer_;
    token_t                 current_token_;
    token_t                 peek_token_;
    stdx::option<ast::AST&> ast_;
    depth_counter           expr_depth_;

    stdx::option<std::vector<ast::explicit_type_id>> pending_impl_bound_;
    depth_counter                                    test_block_depth_;
    depth_counter                                    type_only_depth_;

    std::vector<constexpr_param_frame> constexpr_param_frames_;

    std::vector<pending_doc> pending_docs_;
    std::vector<pending_doc> pending_module_docs_;
    bool                     module_doc_locked_{false};
    bool                     infer_next_fn_params_{false};
};

} // namespace ghoti::syntax
