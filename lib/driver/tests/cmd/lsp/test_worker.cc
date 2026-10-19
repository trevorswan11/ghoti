#include <atomic>
#include <chrono>
#include <iostream>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <stdx/memory.hh>

#include "compiler/module/overlay_loader.hh"
#include "compiler/module/parse_cache.hh"
#include "driver/cmd/lsp/document_store.hh"
#include "driver/cmd/lsp/event_queue.hh"
#include "driver/cmd/lsp/rpc.hh"
#include "driver/cmd/lsp/snapshot.hh"
#include "driver/cmd/lsp/worker.hh"
#include "support/tempfile.hh"
#include "support/test.hh"

using namespace std::chrono_literals;

namespace ghoti::tests {

namespace {

// Inputs for one root, `root.gh` in `dir`, holding `text`
auto inputs_for(const tempdir& dir, u64 revision, const std::string& text) -> lsp::analysis_inputs {
    lsp::analysis_inputs inputs;
    inputs.revision = revision;
    const auto root{dir.path / "root.gh"};
    REQUIRE(inputs.files.add(root, text));
    inputs.roots.emplace_back(std::filesystem::weakly_canonical(root));
    return inputs;
}

} // namespace

TEST_CASE("A snapshot cancelled before it is built is never produced") {
    const tempdir           dir{"snapshot_cancelled"};
    mod::parse_cache        cache;
    const std::atomic<bool> cancelled{true};
    CHECK_FALSE(lsp::analysis_snapshot::build(
        inputs_for(dir, 1, "pub const x = 1;\n"), cache, std::cerr, &cancelled));
}

TEST_CASE("A snapshot answers from the text of its own revision") {
    const tempdir    dir{"snapshot_text"};
    mod::parse_cache cache;
    auto             snapshot{UNWRAP(lsp::analysis_snapshot::build(
        inputs_for(dir, 3, "pub const alpha = 1;\n"), cache, std::cerr))};

    CHECK(snapshot->revision() == 3);
    CHECK(snapshot->module_of(dir.path / "root.gh"));
    CHECK_FALSE(snapshot->module_of(dir.path / "never_reached.gh"));
    CHECK(lsp::has_field(snapshot->workspace_symbols("alp"), "name", "alpha"));
}

TEST_CASE("The worker delivers an analysis of the newest inputs it was given") {
    const tempdir                                       dir{"worker_newest"};
    lsp::event_queue<stdx::box<lsp::analysis_snapshot>> finished;
    lsp::analysis_worker worker{std::cerr, [&](stdx::box<lsp::analysis_snapshot> snapshot) {
                                    finished.push(std::move(snapshot));
                                }};

    // Older inputs are superseded before or while they run, so only the newest must arrive
    worker.schedule(inputs_for(dir, 1, "pub const first = 1;\n"));
    worker.schedule(inputs_for(dir, 2, "pub const second = 2;\n"));
    while (true) {
        auto snapshot{UNWRAP(finished.pop_until(std::chrono::steady_clock::now() + 60s))};
        if (snapshot->revision() < 2) { continue; }
        CHECK(snapshot->revision() == 2);
        CHECK(lsp::has_field(snapshot->workspace_symbols(""), "name", "second"));
        break;
    }
}

TEST_CASE("A store installs only analyses newer than the one it has") {
    const tempdir       dir{"store_install"};
    lsp::document_store store{std::cerr};
    store.edit(dir.path / "root.gh", "pub const x = 1;\n");
    CHECK_FALSE(store.is_current());

    mod::parse_cache cache;
    store.install(UNWRAP(lsp::analysis_snapshot::build(store.inputs(), cache, std::cerr)));
    CHECK(store.is_current());
    CHECK_FALSE(store.take_pending_diagnostics().empty());

    // A late arrival from an older revision can't replace what a newer one installed
    store.install(UNWRAP(lsp::analysis_snapshot::build(
        inputs_for(dir, 0, "pub const stale = 1;\n"), cache, std::cerr)));
    CHECK(store.is_current());
    CHECK(store.take_pending_diagnostics().empty());

    store.edit(dir.path / "root.gh", "pub const x = 2;\n");
    CHECK_FALSE(store.is_current());
    CHECK(UNWRAP(store.snapshot()).revision() == 1);
}

} // namespace ghoti::tests
