#pragma once

#include <filesystem>
#include <iterator>
#include <string>

#include <ankerl/unordered_dense.h>
#include <stdx/memory.hh>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/arena.hh"
#include "compiler/ast/ast.hh"
#include "compiler/syntax/error.hh"
#include "support/source_file.hh"

namespace ghoti::mod {

// Parses `source` into `ast`, allocating its payloads in `arena`
[[nodiscard]] auto parse_source(const source_file& source, ast::AST& ast, ghoti::arena& arena)
    -> syntax::diagnostics;

// Parsed files kept across analyses, so a file whose text is unchanged is not parsed again
class parse_cache {
  public:
    // One file's text and the tree parsed from it. Trees borrowed from `ast` keep it alive.
    struct entry {
        explicit entry(std::string text) : source{std::move(text)} {}
        ~entry() = default;
        MAKE_PINNED(entry);

        source_file         source;
        ghoti::arena        arena;
        ast::AST            ast;
        syntax::diagnostics diagnostics;
    };

  public:
    parse_cache() noexcept = default;
    ~parse_cache()         = default;
    MAKE_MOVE_ONLY(parse_cache);

    // The parse of `path` holding exactly `text`, parsing it on a miss
    [[nodiscard]] auto get(const std::filesystem::path& path, std::string text)
        -> stdx::rc<const entry>;

    // Drops every entry whose path `keep` rejects
    template <typename Keep> auto retain(Keep&& keep) -> void {
        for (auto it{entries_.begin()}; it != entries_.end();) {
            it = keep(it->first) ? std::next(it) : entries_.erase(it);
        }
    }

    [[nodiscard]] auto size() const noexcept -> usize { return entries_.size(); }

  private:
    ankerl::unordered_dense::map<std::filesystem::path, stdx::rc<const entry>> entries_;
};

} // namespace ghoti::mod
