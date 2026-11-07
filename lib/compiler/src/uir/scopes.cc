#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "astgen_internal.hh"
#include "compiler/ast/statement.hh"
#include "compiler/sema/error.hh"
#include "compiler/uir/uir.hh"
#include "support/diagnostic.hh"

namespace ghoti::uir::detail {

namespace {

// The prelude's one bindable name; primitive types and builtins can't be spelled as identifiers
constexpr std::string_view PRELUDE_MODULE{"builtin"};

[[nodiscard]] auto orders_bindings(scope_kind kind) noexcept -> bool {
    return kind == scope_kind::BLOCK || kind == scope_kind::FUNCTION || kind == scope_kind::TEST;
}

} // namespace

auto astgen::push_scope(scope_kind kind) -> scope& {
    auto& added{*scopes_.emplace_back(stdx::make_box<scope>())};
    added.kind        = kind;
    added.is_boundary = kind == scope_kind::FUNCTION;
    return added;
}

auto astgen::pop_scope() noexcept -> void { scopes_.pop_back(); }

auto astgen::visible(const binding& b) const noexcept -> bool {
    if (b.seq == 0) { return true; }
    return std::ranges::none_of(
        hidden_, [&b](const auto& range) { return b.seq > range.first && b.seq <= range.second; });
}

auto astgen::shadowed(std::string_view name) const -> stdx::option<source_location> {
    for (const auto& s : scopes_ | std::views::reverse | std::views::drop(1)) {
        if (const auto found{s->index.find(name)}; found != s->index.end()) {
            return s->bindings[found->second].at;
        }
        if (const auto later{s->later.find(name)}; later != s->later.end()) {
            return later->second;
        }
    }
    return stdx::none;
}

auto astgen::declare(std::string_view name, source_location at, binding b, bool may_shadow)
    -> bool {
    auto& current{*scopes_.back()};
    if (const auto found{current.index.find(name)}; found != current.index.end()) {
        report(at,
               sema::error::IDENTIFIER_REDECLARATION,
               "Redeclaration of symbol '{}'; previous declaration here: {}",
               name,
               current.bindings[found->second].at);
        return false;
    }
    // A shadowing name is still bound, so its uses don't each report a second error
    bool ok{true};
    if (const auto previous{may_shadow ? stdx::none : shadowed(name)}) {
        report(at,
               sema::error::SHADOWING_DECLARATION,
               "Attempt to shadow identifier '{}'; previous declaration here: {}",
               name,
               *previous);
        ok = false;
    } else if (!may_shadow && name == PRELUDE_MODULE) {
        report(at,
               sema::error::SHADOWING_DECLARATION,
               "Attempt to shadow identifier '{}', which the prelude declares",
               name);
        ok = false;
    }

    b.name = name;
    b.at   = at;
    b.seq  = next_seq();
    current.later.erase(name);
    current.index.emplace(name, static_cast<u32>(current.bindings.size()));
    current.bindings.push_back(b);
    return ok;
}

auto astgen::declare_member(std::string_view name, source_location at, binding b) -> bool {
    auto& current{*scopes_.back()};
    if (const auto found{current.index.find(name)}; found != current.index.end()) {
        report(at,
               sema::error::IDENTIFIER_REDECLARATION,
               "Redeclaration of symbol '{}'; previous declaration here: {}",
               name,
               current.bindings[found->second].at);
        return false;
    }
    // A member may share an outer name, but not its own type's: `S.S` would be ambiguous
    if (current.is_aggregate && !current.type_name.empty() && name == current.type_name) {
        if (const auto previous{shadowed(name)}) {
            report(at,
                   sema::error::SHADOWING_DECLARATION,
                   "Attempt to shadow identifier '{}'; previous declaration here: {}",
                   name,
                   *previous);
            return false;
        }
    }

    b.name = name;
    b.at   = at;
    b.seq  = 0;
    current.index.emplace(name, static_cast<u32>(current.bindings.size()));
    current.bindings.push_back(b);
    return true;
}

auto astgen::announce_later(const std::vector<ast::stmt_handle>& statements) -> void {
    auto& current{*scopes_.back()};
    for (const auto stmt : statements) {
        if (const auto decl{ast_.get_as_opt<ast::decl_stmt>(stmt)}) {
            current.later.try_emplace(name_of(decl->name), location_of(decl->name));
        } else if (const auto import{ast_.get_as_opt<ast::import_stmt>(stmt)}) {
            if (const auto named{import->get_name(ast_)}) {
                current.later.try_emplace(named->second, import_location(stmt, *import));
            }
        }
    }
}

auto astgen::lookup(std::string_view name) -> lookup_result {
    std::vector<gsl::not_null<scope*>> crossed;
    for (auto& s : scopes_ | std::views::reverse) {
        if (const auto found{s->index.find(name)}; found != s->index.end()) {
            const auto& b{s->bindings[found->second]};
            if (visible(b)) { return lookup_hit{b, std::move(crossed)}; }
            return used_before_declaration{b.at};
        }
        if (orders_bindings(s->kind)) {
            if (const auto later{s->later.find(name)}; later != s->later.end()) {
                return used_before_declaration{later->second};
            }
        }
        if (s->is_boundary) { crossed.emplace_back(s.get()); }
    }
    return stdx::monostate{};
}

auto astgen::capture_slot_of(scope& boundary, std::string_view name, bool by_ref) -> u32 {
    if (const auto found{boundary.capture_index.find(name)};
        found != boundary.capture_index.end()) {
        auto& slot{boundary.captures[found->second]};
        slot.by_ref = slot.by_ref || by_ref;
        return found->second;
    }
    const auto index{static_cast<u32>(boundary.captures.size())};
    boundary.captures.push_back({name, by_ref});
    boundary.capture_index.emplace(name, index);
    return index;
}

auto astgen::capture_ref(const lookup_hit& hit, use u, source src) -> ref {
    // Every boundary between the use and the binding passes the name inward through a slot. A
    // function that writes a mutable binding gets a pointer to it, and so does every boundary
    // outside it, which is why the mutation scan covers nested function bodies
    const bool place{hit.found.kind == binding_kind::PLACE};
    u32        innermost{0};
    bool       innermost_by_ref{false};
    for (const auto boundary : hit.crossed | std::views::reverse) {
        const bool by_ref{place && !boundary->is_aggregate && !boundary->moves &&
                          boundary->mutated.contains(hit.found.name)};
        innermost        = capture_slot_of(*boundary, hit.found.name, by_ref);
        innermost_by_ref = boundary->captures[innermost].by_ref;
    }

    const auto captured{add_un(tag::capture, {}, innermost, src)};
    if (innermost_by_ref && u == use::VALUE) { return add_un(tag::load, captured, 0, src); }
    return captured;
}

auto astgen::outer_captures(const scope& boundary, source src) -> std::vector<u32> {
    std::vector<u32> refs;
    refs.reserve(boundary.captures.size());
    for (const auto& slot : boundary.captures) {
        const auto found{lookup(slot.name)};
        const auto hit{found.as_opt<lookup_hit>()};
        if (!hit) {
            refs.push_back(static_cast<u32>(to_ref(well_known::undefined)));
            continue;
        }
        const auto wanted{slot.by_ref ? use::PLACE : use::VALUE};
        if (!hit->crossed.empty()) {
            refs.push_back(static_cast<u32>(capture_ref(*hit, wanted, src)));
            continue;
        }
        const auto& b{hit->found};
        const bool  load{b.kind == binding_kind::PLACE && !slot.by_ref};
        refs.push_back(static_cast<u32>(load ? add_un(tag::load, b.value, 0, src) : b.value));
    }
    return refs;
}

auto astgen::in_function() const noexcept -> bool {
    return !functions_.empty() && functions_.back().allows_return;
}

auto astgen::in_test() const noexcept -> bool {
    return std::ranges::any_of(scopes_, [](const auto& s) { return s->kind == scope_kind::TEST; });
}

} // namespace ghoti::uir::detail
