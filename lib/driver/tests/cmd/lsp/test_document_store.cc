#include <filesystem>
#include <iostream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <stdx/option.hh>

#include "driver/cmd/lsp/document_store.hh"
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

} // namespace ghoti::tests
