#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <stdx/option.hh>

#include "driver/cmd/lsp/document_store.hh"
#include "driver/cmd/lsp/rpc.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

auto find(const lsp::document_store::touched_modules& results, const std::filesystem::path& path)
    -> stdx::option<const nlohmann::json&> {
    for (const auto& [touched, diagnostics] : results) {
        if (touched == std::filesystem::weakly_canonical(path)) { return diagnostics; }
    }
    return stdx::none;
}

} // namespace

TEST_CASE("document_store publishes and then clears diagnostics across an update") {
    lsp::document_store         store{std::cerr};
    const std::filesystem::path path{"test_document_store_scratch.gh"};

    const auto  broken{store.update(path, "pub const x := ;\n")};
    const auto& broken_diags{UNWRAP(find(broken, path))};
    CHECK(broken_diags.size() == 1);

    const auto  fixed{store.update(path, "pub const x := 5;\n")};
    const auto& fixed_diags{UNWRAP(find(fixed, path))};
    CHECK(fixed_diags.empty());
}

TEST_CASE("document_store close does not break a later update for the same path") {
    lsp::document_store         store{std::cerr};
    const std::filesystem::path path{"test_document_store_close.gh"};

    const auto first{store.update(path, "pub const x := 5;\n")};
    CHECK(find(first, path));
    store.close(path);

    const auto reopened{store.update(path, "pub const y := 5;\n")};
    CHECK(find(reopened, path));
}

TEST_CASE("document_store workspace_symbols indexes every top-level declaration touched") {
    lsp::document_store         store{std::cerr};
    const std::filesystem::path path{"test_document_store_workspace.gh"};
    CHECK(!store.update(path, "pub const alpha := 1;\npub const beta := 2;\n").empty());

    const auto all = store.workspace_symbols("");
    CHECK(lsp::has_field(all, "name", "alpha"));
    CHECK(lsp::has_field(all, "name", "beta"));
}

TEST_CASE("document_store workspace_symbols filters case-insensitively by substring") {
    lsp::document_store         store{std::cerr};
    const std::filesystem::path path{"test_document_store_workspace_filter.gh"};
    CHECK(!store.update(path, "pub const FooBar := 1;\npub const other := 2;\n").empty());

    const auto matched = store.workspace_symbols("foob");
    CHECK(lsp::has_field(matched, "name", "FooBar"));
    CHECK_FALSE(lsp::has_field(matched, "name", "other"));
}

TEST_CASE("document_store workspace_symbols keeps entries for a closed document") {
    lsp::document_store         store{std::cerr};
    const std::filesystem::path path{"test_document_store_workspace_closed.gh"};
    CHECK(!store.update(path, "pub const still_here := 1;\n").empty());
    store.close(path);

    CHECK(lsp::has_field(store.workspace_symbols(""), "name", "still_here"));
}

} // namespace ghoti::tests
