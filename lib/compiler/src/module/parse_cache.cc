#include "compiler/module/parse_cache.hh"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <stdx/memory.hh>
#include <stdx/profiler.hh>

#include "compiler/arena.hh"
#include "compiler/syntax/error.hh"
#include "compiler/syntax/parser.hh"
#include "support/source_file.hh"

namespace ghoti::mod {

auto parse_source(const source_file& source, ast::AST& ast, ghoti::arena& arena)
    -> syntax::diagnostics {
    syntax::parser p{source};
    p.enable_default_wrappers();
    return p.consume(ast, arena);
}

auto parse_cache::get(const std::filesystem::path& path, std::string text)
    -> stdx::rc<const entry> {
    PROFILE_FUNCTION();
    if (const auto it{entries_.find(path)};
        it != entries_.end() && std::string_view{it->second->source} == text) {
        return it->second;
    }

    auto parsed{stdx::make_rc<entry>(std::move(text))};
    parsed->diagnostics = parse_source(parsed->source, parsed->ast, parsed->arena);
    entries_.insert_or_assign(path, parsed);
    return parsed;
}

} // namespace ghoti::mod
