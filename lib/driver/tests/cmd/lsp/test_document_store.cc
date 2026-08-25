#include <chrono> // IWYU pragma: keep
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <nlohmann/json.hpp>
#include <stdx/option.hh>

#include "driver/cmd/lsp/document_store.hh"
#include "driver/cmd/lsp/references.hh"
#include "driver/cmd/lsp/rpc.hh"
#include "support/tempfile.hh"
#include "support/test.hh"

using namespace std::chrono_literals;

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

TEST_CASE("document_store finds references in a file that imports the queried definition") {
    lsp::document_store         store{std::cerr};
    const std::filesystem::path helper_path{"test_ds_xmod_helper.gh"};
    const std::filesystem::path main_path{"test_ds_xmod_main.gh"};

    CHECK(!store.update(helper_path, "pub const value := 42;\n").empty());
    CHECK(!store
               .update(main_path,
                       "import \"test_ds_xmod_helper.gh\" as helper;\n"
                       "pub const x := helper::value;\n")
               .empty());

    const auto helper_module{UNWRAP(store.analyze(helper_path))};
    const auto definition{UNWRAP(lsp::definition_location_at(*helper_module, {0, 10}))};

    const auto refs{lsp::find_references(store.manager(), definition)};
    REQUIRE(refs.size() == 1);
    CHECK(refs[0].path == std::filesystem::weakly_canonical(main_path));
    CHECK(refs[0].span.start.line == 1);
    CHECK(refs[0].span.start.column == 23);
}

TEST_CASE("document_store finds the upstream reference regardless of which file was opened first") {
    lsp::document_store         store{std::cerr};
    const std::filesystem::path helper_path{"test_ds_xmod_order_helper.gh"};
    const std::filesystem::path main_path{"test_ds_xmod_order_main.gh"};

    // main opened before helper, the reverse of the previous test
    CHECK(!store
               .update(main_path,
                       "import \"test_ds_xmod_order_helper.gh\" as helper;\n"
                       "pub const x := helper::value;\n")
               .empty());
    CHECK(!store.update(helper_path, "pub const value := 42;\n").empty());

    const auto helper_module{UNWRAP(store.analyze(helper_path))};
    const auto definition{UNWRAP(lsp::definition_location_at(*helper_module, {0, 10}))};

    const auto refs{lsp::find_references(store.manager(), definition)};
    REQUIRE(refs.size() == 1);
    CHECK(refs[0].path == std::filesystem::weakly_canonical(main_path));
}

TEST_CASE(
    "document_store reflects a downstream edit in a later upstream-relative references query") {
    lsp::document_store         store{std::cerr};
    const std::filesystem::path helper_path{"test_ds_xmod_edit_helper.gh"};
    const std::filesystem::path main_path{"test_ds_xmod_edit_main.gh"};

    CHECK(!store.update(helper_path, "pub const value := 42;\n").empty());
    CHECK(!store
               .update(main_path,
                       "import \"test_ds_xmod_edit_helper.gh\" as helper;\n"
                       "pub const x := helper::value;\n")
               .empty());

    const auto definition_of = [&] {
        const auto helper_module{UNWRAP(store.analyze(helper_path))};
        return UNWRAP(lsp::definition_location_at(*helper_module, {0, 10}));
    };
    REQUIRE(lsp::find_references(store.manager(), definition_of()).size() == 1);

    // A second didChange adding a second usage of helper::value
    CHECK(!store
               .update(main_path,
                       "import \"test_ds_xmod_edit_helper.gh\" as helper;\n"
                       "pub const x := helper::value;\n"
                       "pub const y := helper::value;\n")
               .empty());

    CHECK(lsp::find_references(store.manager(), definition_of()).size() == 2);
}

TEST_CASE("document_store only finds references in files it has actually opened or queried") {
    lsp::document_store         store{std::cerr};
    const std::filesystem::path helper_path{"test_ds_xmod_unopened_helper.gh"};
    CHECK(!store.update(helper_path, "pub const value := 42;\n").empty());

    // No importer of `value` was ever opened via update() or queried via analyze()
    const auto helper_module{UNWRAP(store.analyze(helper_path))};
    const auto definition{UNWRAP(lsp::definition_location_at(*helper_module, {0, 10}))};
    CHECK(lsp::find_references(store.manager(), definition).empty());
}

TEST_CASE("update_throttled skips the rebuild when called again inside the throttle window") {
    lsp::document_store         store{std::cerr, 1'000ms};
    const std::filesystem::path path{"test_ds_throttle_skip.gh"};

    CHECK(!store.update_throttled(path, "pub const x := 5;\n").empty()); // first call rebuilds
    CHECK(store.update_throttled(path, "pub const x := 6;\n").empty());  // second is throttled
}

TEST_CASE("update_throttled rebuilds again once the throttle interval has elapsed") {
    lsp::document_store         store{std::cerr, 1ms};
    const std::filesystem::path path{"test_ds_throttle_elapsed.gh"};

    CHECK(!store.update_throttled(path, "pub const x := 5;\n").empty());
    std::this_thread::sleep_for(10ms); // 10x margin over the 1ms interval
    CHECK(!store.update_throttled(path, "pub const x := 6;\n").empty());
}

TEST_CASE("a throttled-and-skipped update_throttled call still leaves reads fresh") {
    lsp::document_store         store{std::cerr, 1'000ms};
    const std::filesystem::path path{"test_ds_throttle_dirty_read.gh"};

    CHECK(!store.update_throttled(path, "pub const x := 5;\n").empty());
    CHECK(store.update_throttled(path, "pub const x := 6; pub const y := 1;\n").empty());

    // The rebuild was skipped, but analyze()/workspace_symbols() must still see the latest text
    const auto module{UNWRAP(store.analyze(path))};
    CHECK(module->identifier_positions.size() > 0);
    CHECK(lsp::has_field(store.workspace_symbols(""), "name", "y"));
}

TEST_CASE("seed_known_roots discovers cross-file references without ever opening either file") {
    const tempfile helper_file{"seed_helper"};
    {
        std::ofstream helper_out{helper_file.path};
        fmt::println(helper_out, "pub const value := 42;");
    }
    const tempfile main_file{"seed_main"};
    {
        std::ofstream main_out{main_file.path};
        fmt::println(main_out,
                     "import \"{}\" as helper;\npub const x := helper::value;",
                     helper_file.path.filename().string());
    }

    lsp::document_store store{std::cerr};
    const auto          seeded{store.seed_known_roots({helper_file.path, main_file.path})};
    CHECK(!seeded.empty());

    const auto helper_module{UNWRAP(store.analyze(helper_file.path))};
    const auto definition{UNWRAP(lsp::definition_location_at(*helper_module, {0, 10}))};
    const auto refs{lsp::find_references(store.manager(), definition)};
    REQUIRE(refs.size() == 1);
    CHECK(refs[0].path == std::filesystem::weakly_canonical(main_file.path));
}

} // namespace ghoti::tests
