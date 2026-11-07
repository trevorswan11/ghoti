#include <filesystem>
#include <sstream>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "compiler/module/error.hh"
#include "compiler/module/memory_loader.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/error.hh"
#include "ghoti/config.h"
#include "helpers/common.hh"

namespace ghoti::tests {

TEST_CASE("Fetching absolute file modules") {
    mod::memory_loader  loader;
    mod::module_manager manager{loader};

#if GHOTI_WINDOWS
    const std::filesystem::path root{"C:/fake"};
    const std::filesystem::path elsewhere{"D:/other"};
#else
    const std::filesystem::path root{"/fake"};
    const std::filesystem::path elsewhere{"/other"};
#endif
    loader.add(root / "lib" / "foo.gh", "pub const x = 1;");

    // An absolute import ignores its importer's directory and dedupes with the relative spelling
    const auto absolute{UNWRAP(manager.try_get_file_module(root / "lib" / "foo.gh", elsewhere))};
    const auto relative{UNWRAP(manager.try_get_file_module("lib/foo.gh", root))};
    CHECK(absolute.get() == relative.get());

    const auto missing{UNWRAP_ERR(manager.try_get_file_module(root / "missing.gh", elsewhere))};
    CHECK(missing.get_error() == mod::error::PATH_DOES_NOT_EXIST);
}

TEST_CASE("Fetching missing library modules") {
    mod::memory_loader  loader;
    mod::module_manager manager{loader};
    const auto          actual{UNWRAP_ERR(manager.try_get_library_module("foo"))};

    const mod::diagnostic expected{
        "Unknown module 'foo'",
        mod::error::MODULE_DOES_NOT_EXIST,
    };
    CHECK(actual == expected);
}

TEST_CASE("Adding duplicate library module") {
    mod::memory_loader  loader;
    mod::module_manager manager{loader};
    REQUIRE(manager.add_library_module("foo", "foo.gh"));
    const auto actual{UNWRAP_ERR(manager.add_library_module("foo", "src/foo.gh"))};

    const mod::diagnostic expected{
        "Attempt to add duplicate module 'foo' from path 'src/foo.gh' which already exists at "
        "path 'foo.gh'",
        mod::error::MODULE_ALREADY_EXISTS,
    };
    CHECK(actual == expected);
}

namespace {

#if GHOTI_WINDOWS
const std::filesystem::path FAKE_ROOT{"C:/fake"};
#else
const std::filesystem::path FAKE_ROOT{"/fake"};
#endif

[[nodiscard]] auto count_of(std::string_view text, std::string_view needle) -> usize {
    usize count{0};
    for (auto at{text.find(needle)}; at != std::string_view::npos;
         at = text.find(needle, at + needle.size())) {
        count += 1;
    }
    return count;
}

} // namespace

TEST_CASE("Diagnostics print sorted by module path whatever the load order") {
    mod::memory_loader  loader;
    mod::module_manager manager{loader};
    loader.add(FAKE_ROOT / "b.gh", "const x = else;\n");
    loader.add(FAKE_ROOT / "a.gh", "const y = else;\n");
    DISCARD(UNWRAP(manager.try_get_file_module(FAKE_ROOT / "b.gh")));
    DISCARD(UNWRAP(manager.try_get_file_module(FAKE_ROOT / "a.gh")));

    std::ostringstream os;
    manager.print_all_diagnostics(os);
    const auto printed{os.view()};
    const auto a_at{printed.find("a.gh:1:11")};
    const auto b_at{printed.find("b.gh:1:11")};
    REQUIRE(a_at != std::string_view::npos);
    REQUIRE(b_at != std::string_view::npos);
    CHECK(a_at < b_at);
}

TEST_CASE("A module's diagnostics print sorted by position") {
    mod::memory_loader  loader;
    mod::module_manager manager{loader};
    loader.add(FAKE_ROOT / "c.gh", "const a = 1;\nconst b = 2;\n");
    auto module{UNWRAP(manager.try_get_file_module(FAKE_ROOT / "c.gh"))};

    sema::diagnostics list;
    list.emplace_back("second", sema::error::TYPE_MISMATCH, source_location{1, 0});
    list.emplace_back("first", sema::error::TYPE_MISMATCH, source_location{0, 0});
    module->error_out(std::move(list), mod::module_state::POISONED_TYPE_RESOLVED);

    std::ostringstream os;
    module->print_diagnostics(os);
    const auto printed{os.view()};
    CHECK(printed.find("first") < printed.find("second"));
}

TEST_CASE("The error limit stops printing and counts the rest") {
    mod::memory_loader  loader;
    mod::module_manager manager{loader};
    loader.add(FAKE_ROOT / "d.gh", "const x = else;\nconst y = else;\nconst z = else;\n");
    DISCARD(UNWRAP(manager.try_get_file_module(FAKE_ROOT / "d.gh")));

    manager.set_error_limit(2);
    std::ostringstream os;
    manager.print_all_diagnostics(os);
    CHECK(count_of(os.view(), "error[") == 2);
    CHECK(os.view().contains("d.gh:2:11"));
    CHECK_FALSE(os.view().contains("d.gh:3:11"));
    CHECK(os.view().ends_with("1 more error not shown\n"));
}

TEST_CASE("Without an error limit every error prints") {
    mod::memory_loader  loader;
    mod::module_manager manager{loader};
    loader.add(FAKE_ROOT / "e.gh", "const x = else;\nconst y = else;\nconst z = else;\n");
    DISCARD(UNWRAP(manager.try_get_file_module(FAKE_ROOT / "e.gh")));

    std::ostringstream os;
    manager.print_all_diagnostics(os);
    CHECK(count_of(os.view(), "error[") == 3);
    CHECK_FALSE(os.view().contains("not shown"));
}

} // namespace ghoti::tests
