#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/ast/expression.hh"
#include "compiler/module/memory_loader.hh"
#include "compiler/module/module.hh"
#include "helpers/uir_diff.hh"

namespace ghoti::tests {

namespace {

// UIR Sema's run of `files` with the LSP index on
[[nodiscard]] auto indexed(mod::memory_loader& loader) -> stdx::box<helpers::uir_sema_run> {
    auto run{helpers::run_uir_sema_files(loader, "main.gh", {.index = true})};
    REQUIRE(run->out->succeeded());
    return run;
}

// The `nth` identifier spelled `name` that the index found, in source order
[[nodiscard]] auto identifier(const mod::module_t& m, std::string_view name, usize nth = 0)
    -> ast::node_id {
    std::vector<ast::node_id> found;
    for (const auto id : m.identifier_positions) {
        const auto ident{m.ast.get_as_opt<ast::identifier_expr>(id)};
        if (!ident || ident->name != name) { continue; }
        // A name lowered twice is indexed twice
        const bool seen{std::ranges::any_of(found, [&](ast::node_id other) {
            return m.ast.location_of(other) == m.ast.location_of(id);
        })};
        if (!seen) { found.emplace_back(id); }
    }

    std::ranges::sort(found, {}, [&](ast::node_id id) {
        const auto at{m.ast.location_of(id)};
        return std::pair{at.line, at.column};
    });
    INFO("identifier '" << name << "'");
    REQUIRE(nth < found.size());
    return found[nth];
}

[[nodiscard]] auto type_name(const mod::module_t& m, ast::node_id id) -> std::string {
    const auto type{m.get_sema_type_opt(id)};
    return type ? type->to_string() : std::string{"<none>"};
}

} // namespace

TEST_CASE("The LSP index types declared names and their uses", "[uir]") {
    mod::memory_loader loader;
    loader.add("main.gh", R"(
        let mut small: i8 = 1;
        let mut wide: i64 = 2;
        const twice = fn(w: i32): i32 { return w * 2; };
        pub const main = fn(): i32 {
            let sum = small + wide;
            return twice(@intCast(i32, sum));
        };
    )");
    const auto  run{indexed(loader)};
    const auto& m{*run->modules->find_file_module("main.gh")};
    CHECK(type_name(m, identifier(m, "sum")) == "i64");
    CHECK(type_name(m, identifier(m, "sum", 1)) == "i64");
    CHECK(type_name(m, identifier(m, "w")) == "i32");
    CHECK(type_name(m, identifier(m, "w", 1)) == "i32");
    CHECK(type_name(m, identifier(m, "small", 1)) == "i8");
}

TEST_CASE("The LSP index resolves fields and module members to their declarations", "[uir]") {
    mod::memory_loader loader;
    loader.add("main.gh", R"(
        import "lib.gh" as lib;
        const P = struct { x: i32, y: i32 };
        pub const main = fn(): i32 {
            let p: P = .{ .x = 1, .y = 2 };
            return p.y + lib.get();
        };
    )");
    loader.add("lib.gh", "pub const get = fn(): i32 { return 3; };");
    const auto  run{indexed(loader)};
    const auto& m{*run->modules->find_file_module("main.gh")};
    const auto& lib{*run->modules->find_file_module("lib.gh")};

    // `p.y` is defined at the field `y`, on the line that declares `P`
    const auto declared{identifier(m, "y")};
    const auto field{m.get_identifier_definition(identifier(m, "y", 1))};
    REQUIRE(field);
    CHECK(field->path == m.path);
    CHECK(field->span.start == m.ast.location_of(declared));
    CHECK(field->span.start.line == m.ast.location_of(identifier(m, "P")).line);
    CHECK(type_name(m, identifier(m, "y", 1)) == "i32");
    CHECK(type_name(m, declared) == "i32");

    const auto member{m.get_identifier_definition(identifier(m, "get"))};
    REQUIRE(member);
    CHECK(member->path == lib.path);
    const auto declaration{m.get_identifier_declaration(identifier(m, "get"))};
    REQUIRE(declaration);
    CHECK(&*declaration->owner == &lib);
}

} // namespace ghoti::tests
