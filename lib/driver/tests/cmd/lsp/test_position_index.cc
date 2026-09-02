#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <stdx/memory.hh>

#include "compiler/module/overlay_loader.hh"
#include "driver/cmd/lsp/position_index.hh"
#include "driver/cmd/lsp/session.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view source{"pub const x := 5;\n"
                                  "pub const y := x + 1;\n"};

} // namespace

TEST_CASE("identifier_at finds a reference and its declaration through the side tables") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_position_index.gh"};
    CHECK(loader.add(path, std::string{source}));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    const auto  id{UNWRAP(lsp::identifier_at(*module, {1, 15}))};
    const auto& type{UNWRAP(module->get_sema_type_opt(id))};
    CHECK(type.to_string() == "i32");

    // get_symbol_span resolves to the declared name itself, not the whole `pub const` statement
    const auto def_loc{UNWRAP(module->get_identifier_definition(id))};
    CHECK(def_loc.path == module->path);
    CHECK(def_loc.span.start.line == 0);
    CHECK(def_loc.span.start.column == 10);
    CHECK(def_loc.span.end.line == 0);
    CHECK(def_loc.span.end.column == 11);
}

TEST_CASE("hover-style type resolution still works around an unrelated syntax error") {
    // The 3rd statement is a dangling dot-expression and fails to parse
    constexpr std::string_view broken_source{"pub const x := 5;\n"
                                             "pub const y := x + 1;\n"
                                             "pub const broken := y.;\n"};

    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_position_index_partial_error.gh"};
    CHECK(loader.add(path, std::string{broken_source}));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};
    CHECK_FALSE(module->is_errored());
    CHECK_FALSE(module->parse_diagnostics.empty());

    const auto  id{UNWRAP(lsp::identifier_at(*module, {1, 15}))};
    const auto& type{UNWRAP(module->get_sema_type_opt(id))};
    CHECK(type.to_string() == "i32");

    const auto def_loc{UNWRAP(module->get_identifier_definition(id))};
    CHECK(def_loc.span.start.line == 0);
    CHECK(def_loc.span.start.column == 10);
}

TEST_CASE("identifier_at returns none off the end of an identifier and off any identifier") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_position_index_none.gh"};
    CHECK(loader.add(path, std::string{source}));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    CHECK_FALSE(lsp::identifier_at(*module, {1, 16})); // just past `x`
    CHECK_FALSE(lsp::identifier_at(*module, {0, 0}));  // `pub`, not an identifier_expr
}

TEST_CASE("hover-style type resolution works on a struct field declaration's own name") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_position_index_struct_field.gh"};
    CHECK(loader.add(path, "pub const S := struct { x: i32 };\n"));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    // Column 24 lands on the `x` field name itself, inside the struct body
    const auto  id{UNWRAP(lsp::identifier_at(*module, {0, 24}))};
    const auto& type{UNWRAP(module->get_sema_type_opt(id))};
    CHECK(type.to_string() == "i32");
}

TEST_CASE("hover-style type resolution works on an enum enumerator's own name") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_position_index_enum_member.gh"};
    CHECK(loader.add(path, "pub const E := enum { RED };\n"));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    const auto  id{UNWRAP(lsp::identifier_at(*module, {0, 22}))};
    const auto& type{UNWRAP(module->get_sema_type_opt(id))};
    CHECK(type.to_string() == "i32");
}

TEST_CASE("hover-style type resolution works on a union field declaration's own name") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_position_index_union_field.gh"};
    CHECK(loader.add(path, "pub const U := union { x: i32, y: i32 };\n"));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    const auto  id{UNWRAP(lsp::identifier_at(*module, {0, 23}))};
    const auto& type{UNWRAP(module->get_sema_type_opt(id))};
    CHECK(type.to_string() == "i32");
}

TEST_CASE("hover-style type resolution works on a slice's .len member access") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_position_index_slice_len.gh"};
    CHECK(loader.add(path, "pub const f := fn(s: []i32): usize { return s.len; };\n"));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    const auto  id{UNWRAP(lsp::identifier_at(*module, {0, 46}))};
    const auto& type{UNWRAP(module->get_sema_type_opt(id))};
    CHECK(type.to_string() == "usize");
}

TEST_CASE("hover-style type resolution names the module on an import alias and its uses") {
    mod::overlay_loader         loader;
    const std::filesystem::path helper_path{"helper.gh"};
    const std::filesystem::path main_path{"main.gh"};
    CHECK(loader.add(helper_path, "pub const value := 42;\n"));
    CHECK(loader.add(main_path,
                     "import \"helper.gh\" as helper;\n"
                     "pub const x := helper::value;\n"));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(main_path))};

    const auto  alias_id{UNWRAP(lsp::identifier_at(*module, {0, 25}))};
    const auto& alias_type{UNWRAP(module->get_sema_type_opt(alias_id))};
    CHECK(alias_type.to_string() == "module helper");

    const auto  use_id{UNWRAP(lsp::identifier_at(*module, {1, 18}))};
    const auto& use_type{UNWRAP(module->get_sema_type_opt(use_id))};
    CHECK(use_type.to_string() == "module helper");
}

TEST_CASE("hover-style type resolution shows the null-terminated sentinel on array/slice types") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_position_index_null_terminated.gh"};
    CHECK(loader.add(path, "pub const f := fn(s: [:0]u8): void { const l := s; };\n"));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    const auto  id{UNWRAP(lsp::identifier_at(*module, {0, 48}))};
    const auto& type{UNWRAP(module->get_sema_type_opt(id))};
    CHECK(type.to_string() == "[:0]u8");
}

} // namespace ghoti::tests
