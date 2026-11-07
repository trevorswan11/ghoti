#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include "compiler/sema/error.hh"
#include "compiler/uir/dumper.hh"
#include "helpers/uir.hh"

namespace ghoti::tests {

namespace {

[[nodiscard]] auto decl_names(const uir::lowered& lowered) -> std::vector<std::string> {
    std::vector<std::string> names;
    for (const auto& d : lowered.uir.decls()) { names.emplace_back(lowered.uir.string(d.name)); }
    return names;
}

[[nodiscard]] auto has_code(const uir::lowered& lowered, sema::error code) -> bool {
    return std::ranges::any_of(lowered.diagnostics,
                               [code](const auto& d) { return d.get_error() == code; });
}

} // namespace

TEST_CASE("A gated-off declaration doesn't exist in UIR") {
    const auto lowered{helpers::lower_uir(
        "@cfg (ptr_bits == 1) { const gone = 1; } else { const kept = 2; }\nconst user = kept;")};
    CHECK(lowered.diagnostics.empty());
    CHECK(decl_names(lowered) == std::vector<std::string>{"kept", "user"});
}

TEST_CASE("A name only a gated-off arm declares is undeclared") {
    const auto lowered{
        helpers::lower_uir("@cfg (ptr_bits == 1) { const gone = 1; }\nconst user = gone;")};
    CHECK(has_code(lowered, sema::error::UNDECLARED_IDENTIFIER));
}

TEST_CASE("Gated fields, members, and statements follow their selected arm") {
    const auto lowered{helpers::lower_uir(R"(
const S = struct {
    @cfg (ptr_bits == 1) { a: i32, } else { b: i32, }
    @cfg (ptr_bits == 1) { pub const k = 1; } else { pub const j = 2; }
};
const f = fn(): i32 {
    @cfg (ptr_bits == 1) { let x = 1; return x; } else { let y = 2; return y; }
};
)")};
    CHECK(lowered.diagnostics.empty());
    const auto text{uir::dump(lowered.uir)};
    CHECK(text.contains("name=\"b\""));
    CHECK_FALSE(text.contains("name=\"a\""));
    CHECK(text.contains("decl#2 VALUE j"));
    CHECK(text.contains("name=\"y\""));
    CHECK_FALSE(text.contains("name=\"x\""));
}

TEST_CASE("A @compileError fires only in the selected arm") {
    const auto unselected{helpers::lower_uir(
        "@cfg (ptr_bits == 1) { @compileError(\"never\"); } else { const ok = 1; }")};
    CHECK(unselected.diagnostics.empty());

    const auto selected{helpers::lower_uir("@cfg (ptr_bits != 1) { @compileError(\"always\"); }")};
    REQUIRE(selected.diagnostics.size() == 1);
    CHECK(selected.diagnostics[0].get_error() == sema::error::COMPILE_ERROR_REACHED);
    CHECK(selected.diagnostics[0].get_message() == "always");
}

TEST_CASE("@cfgValue lowers to its selected expression") {
    const auto lowered{helpers::lower_uir("const wide = @cfgValue(ptr_bits == 64);\n"
                                          "const width = @cfgValue(ptr_bits == 1 => 1, _ => 64);")};
    CHECK(lowered.diagnostics.empty());
    const auto text{uir::dump(lowered.uir)};
    CHECK(text.contains("yield @bool_true"));
    CHECK(text.contains("int_value 64"));
    CHECK_FALSE(text.contains("int_value 1\n"));
}

TEST_CASE("Tests exist only when building tests") {
    constexpr std::string_view source{"test \"t\" { const x = only_for_tests; }\n"
                                      "@cfg (testing) { const only_for_tests = 1; }"};
    const auto                 build{helpers::lower_uir(source)};
    CHECK(build.diagnostics.empty());
    CHECK(decl_names(build).empty());

    const auto test{helpers::lower_uir(source, {}, true)};
    CHECK(test.diagnostics.empty());
    CHECK(decl_names(test) == std::vector<std::string>{"t", "only_for_tests"});
}

} // namespace ghoti::tests
