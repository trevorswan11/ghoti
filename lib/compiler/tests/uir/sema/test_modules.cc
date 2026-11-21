#include <filesystem>
#include <initializer_list>
#include <regex>
#include <sstream>
#include <string>
#include <utility>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include "compiler/gir/dumper.hh"
#include "compiler/module/memory_loader.hh"
#include "helpers/uir_diff.hh"
#include "helpers/uir_run.hh"

namespace ghoti::tests {

namespace {

using file = std::pair<std::string, std::string>;

// Both pipelines' outcome for a program of in-memory files, the first being the root
[[nodiscard]] auto diff_files(std::initializer_list<file> files) -> helpers::uir_diff {
    mod::memory_loader loader;
    for (const auto& [path, text] : files) { loader.add(path, text); }
    return helpers::diff_pipelines(loader, files.begin()->first, {}, {});
}

// UIR Sema's raw GIR for a program of in-memory files
[[nodiscard]] auto uir_files_gir(std::initializer_list<file> files) -> std::string {
    mod::memory_loader loader;
    for (const auto& [path, text] : files) { loader.add(path, text); }
    const auto  run{helpers::run_uir_sema_files(loader, files.begin()->first)};
    const auto& out{*run->out};
    if (out.stopped_at) { FAIL("UIR Sema stopped at " << out.stopped_at->what); }
    REQUIRE(out.succeeded());
    std::ostringstream text;
    gir::dumper{text}.dump(*out.gir);
    return text.str();
}

} // namespace

TEST_CASE("An import's declarations are reached through its module", "[uir]") {
    const auto diff{diff_files({
        {"main.gh", R"(
            import "lib.gh" as lib;
            pub const main = fn(): i32 {
                lib.counter += 1;
                return lib.twice(lib.K) + lib.Point{ .x = 2 }.get() + lib.counter;
            };
        )"},
        {"lib.gh", R"(
            pub const K: i32 = 3;
            pub let mut counter: i32 = 0;
            pub const twice = fn(x: i32): i32 { return x * 2; };
            pub const Point = struct { x: i32, pub const get = fn(self): i32 { return self.x; }; };
        )"},
    })};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A name declared with storage in two scopes is qualified by each", "[uir]") {
    const std::initializer_list<file> files{
        {"main.gh", R"(
            import "lib.gh" as lib;
            const helper = fn(x: i32): i32 { return x + 1; };
            const K: i32 = 4;
            const T = i32;
            pub const main = fn(): i32 { return helper(K) + lib.helper(lib.K); };
        )"},
        {"lib.gh", R"(
            pub const helper = fn(x: i32): i32 { return x * 2; };
            pub const K: i32 = 5;
            pub const T = u8;
        )"},
    };
    const auto diff{diff_files(files)};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);

    // Two scopes, two qualified symbols each, while the type aliases claim none
    const auto       gir{uir_files_gir(files)};
    const std::regex helper{R"(fn m\d+\.helper\()"};
    const std::regex constant{R"(const m\d+\.K: i32)"};
    const auto       count{[&](const std::regex& re) {
        return std::distance(std::sregex_iterator{gir.begin(), gir.end(), re}, {});
    }};
    CHECK(count(helper) == 2);
    CHECK(count(constant) == 2);
    CHECK(gir.contains("fn main()"));
}

TEST_CASE("Same-named generics of two modules number their instances apart", "[uir]") {
    const std::initializer_list<file> files{
        {"main.gh", R"(
            import "lib.gh" as lib;
            const id = fn(x: auto): auto { return x; };
            pub const main = fn(): i32 { return id(1i32) + lib.id(2i32); };
        )"},
        {"lib.gh", "pub const id = fn(x: auto): auto { return x + 1; };"},
    };
    const auto diff{diff_files(files)};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
    const auto gir{uir_files_gir(files)};
    CHECK(gir.contains("fn id__i32("));
    CHECK(gir.contains("fn id_v2__i32("));
}

TEST_CASE("Each impl's inherited default methods are qualified by the impl", "[uir]") {
    const auto diff{diff_files({
        {"main.gh", R"(
            import "a.gh" as a;
            import "b.gh" as b;
            pub const main = fn(): i32 {
                let x = a.A{ .v = 1 };
                return x.add_three();
            };
        )"},
        {"iface.gh", R"(
            pub const Base = interface {
                pub const base = fn(&self): i32;
                pub const add_three = fn(&self): i32 { return self.base() + 3; };
            };
        )"},
        {"a.gh", R"(
            import "iface.gh" as iface;
            pub const A = struct { pub v: i32 };
            impl iface.Base for A { pub const base = fn(&self): i32 { return self.v; }; }
        )"},
        {"b.gh", R"(
            import "iface.gh" as iface;
            pub const B = struct { pub v: i32 };
            impl iface.Base for B { pub const base = fn(&self): i32 { return self.v * 2; }; }
        )"},
    })};
    INFO(diff.detail);
    CHECK(diff.status == helpers::uir_diff_status::MATCH);
}

TEST_CASE("A private declaration of another module is reported where it is used", "[uir]") {
    mod::memory_loader loader;
    loader.add("main.gh", R"(
        import "lib.gh" as lib;
        pub const main = fn(): i32 { return lib.hidden; };
    )");
    loader.add("lib.gh", "const hidden: i32 = 1;");
    const auto run{helpers::run_uir_sema_files(loader, "main.gh")};
    REQUIRE_FALSE(run->out->stopped_at);
    REQUIRE(run->out->diagnostics.size() == 1);
    for (const auto& d : run->out->diagnostics) {
        CHECK(d.get_message() == "Symbol 'hidden' is private to module 'lib'");
    }
}

} // namespace ghoti::tests
