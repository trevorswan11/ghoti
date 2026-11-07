#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <stdx/memory.hh>
#include <stdx/utility.hh>

#include "compiler/module/module.hh"
#include "compiler/module/overlay_loader.hh"
#include "compiler/sema/error.hh"
#include "driver/cmd/lsp/diagnostics.hh"
#include "driver/cmd/lsp/session.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("to_lsp_diagnostics is empty for a clean module") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_diagnostics_clean.gh"};
    CHECK(loader.add(path, "pub const x = 5;\n"));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};
    CHECK(lsp::to_lsp_diagnostics(*module).empty());
}

TEST_CASE("to_lsp_diagnostics reports a syntax error with a code, message, and range") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_diagnostics_broken.gh"};
    CHECK(loader.add(path, "pub const x = ;\n"));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};

    const auto diagnostics = lsp::to_lsp_diagnostics(*module);
    REQUIRE(diagnostics.size() == 1);

    const auto& diag{diagnostics.at(0)};
    CHECK(diag.at("severity") == 1);
    CHECK(diag.at("source") == "ghoti");
    CHECK(diag.at("code") == "MISSING_PREFIX_PARSER");
    CHECK_FALSE(diag.at("message").get<std::string>().empty());
    CHECK(diag.at("range").at("start").at("line") == 0);
}

TEST_CASE("to_lsp_diagnostics reports a deprecated use as a tagged warning") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_diagnostics_deprecated.gh"};
    CHECK(loader.add(path, R"(
@[deprecated("use `newer`")]
const older = fn(): i32 { return 1; };
pub const x = older();
)"));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};
    CHECK(module->is_ok());

    const auto diagnostics = lsp::to_lsp_diagnostics(*module);
    REQUIRE(diagnostics.size() == 1);
    const auto& diag{diagnostics.at(0)};
    CHECK(diag.at("severity") == 2);
    CHECK(diag.at("code") == "DEPRECATED_USE");
    CHECK(diag.at("tags") == nlohmann::json::array({2}));
    CHECK(diag.at("message") == "'older' is deprecated: use `newer`");
    CHECK(diag.at("range").at("start").at("line") == 3);
}

TEST_CASE("a shared session that analyzes two impls of one interface keeps inherited defaults") {
    mod::overlay_loader loader;
    CHECK(loader.add(std::filesystem::path{"reader.gh"}, R"(
        pub const Result = fn(comptime T: type, comptime E: type): type { return union(enum) { ok: T, err: E }; };
        impl(T: type, E: type) builtin.Unwrappable for Result(T, E) {
            const Output = T;
            const Residual = E;
            pub const branch = fn(&mut? self): builtin.Flow(&mut? T, E) {
                return match (self) {
                    .ok => |&mut? v| .{ .@"continue" = v },
                    .err => |e| .{ .@"break" = e },
                };
            };
        }
        impl(T: type, E: type) builtin.Rewrappable for Result(T, E) {
            const From = E;
            pub const from_residual = fn(r: E): @This() { return .{ .err = r }; };
        }
        pub const Reader = interface {
            Error: type;
            pub const read = fn(&mut self, buf: []mut u8): Result(usize, Error);
            pub const readAll = fn(&mut self, buf: []mut u8): Result(usize, Error) {
                let mut i: usize = 0;
                loop {
                    if (i == buf.len) { break; }
                    let n = self.read(buf[i..])?;
                    if (n == 0) { break; }
                    i += n;
                };
                return .{ .ok = i };
            };
        };
    )"));
    constexpr std::string_view impl_body{R"(
        import "reader.gh" as reader;
        pub const {0} = struct {{ pub data: []mut u8, pub pos: usize = 0 }};
        impl reader.Reader for {0} {{
            const Error = u8;
            pub const read = fn(&mut self, buf: []mut u8): reader.Result(usize, Error) {{
                let rem = self.data.len - self.pos;
                let n = if (buf.len < rem) buf.len else rem;
                self.pos += n;
                return .{{ .ok = n }};
            }};
        }}
        pub const drain = fn(s: &mut {0}, out: []mut u8): usize {{
            return match (s.readAll(out)) {{ .ok => |n| n, .err => 0uz }};
        }};
    )"};

    // `early.gh` resolves `reader::Reader` first
    CHECK(loader.add(std::filesystem::path{"early.gh"}, fmt::format(impl_body, "Early")));
    CHECK(loader.add(std::filesystem::path{"late.gh"}, fmt::format(impl_body, "Late")));

    auto session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    DISCARD(session->analyze(std::filesystem::path{"early.gh"}));

    // `late.gh` then implements the cached interface.
    const auto late{UNWRAP(session->analyze(std::filesystem::path{"late.gh"}))};
    CHECK(lsp::to_lsp_diagnostics(*late).empty());
}

TEST_CASE("to_lsp_diagnostics spans a parse error's whole token") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_diagnostics_token_span.gh"};
    CHECK(loader.add(path, "pub const x = else;\n"));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};
    const auto diagnostics = lsp::to_lsp_diagnostics(*module);
    REQUIRE_FALSE(diagnostics.empty());

    const auto& range{diagnostics.at(0).at("range")};
    CHECK(range.at("start").at("character") == 14);
    CHECK(range.at("end").at("character") == 18);
}

TEST_CASE("to_lsp_diagnostics sorts and carries labels and notes") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_diagnostics_related.gh"};
    const std::filesystem::path other{"test_diagnostics_related_other.gh"};
    CHECK(loader.add(path, "const a = 1;\nconst b = 2;\n"));

    auto session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    auto module{UNWRAP(session->analyze(path))};

    sema::diagnostics list;
    sema::diagnostic  late{"late", sema::error::TYPE_MISMATCH, source_span{{1, 6}, {1, 7}}};
    late.with_primary_label("this one")
        .with_label({other, source_span{{0, 2}, {0, 4}}}, "declared here")
        .with_note("unlocated context")
        .with_note("located context", located_span{path, source_span{{0, 0}, {0, 5}}});
    list.push_back(late);
    list.emplace_back("early", sema::error::TYPE_MISMATCH, source_location{0, 6});
    module->diagnostics.emplace<sema::diagnostics>(std::move(list));

    const auto diagnostics = lsp::to_lsp_diagnostics(*module);
    REQUIRE(diagnostics.size() == 2);
    CHECK(diagnostics.at(0).at("message") == "early");
    CHECK_FALSE(diagnostics.at(0).contains("relatedInformation"));

    const auto& diag{diagnostics.at(1)};
    CHECK(diag.at("message") == "late\nnote: unlocated context");
    CHECK(diag.at("range").at("start").at("character") == 6);
    CHECK(diag.at("range").at("end").at("character") == 7);

    const auto& related{diag.at("relatedInformation")};
    REQUIRE(related.size() == 3);
    CHECK(related.at(0).at("message") == "this one");
    CHECK(related.at(1).at("message") == "declared here");
    CHECK(related.at(1)
              .at("location")
              .at("uri")
              .get<std::string>()
              .ends_with("test_diagnostics_related_other.gh"));
    CHECK(related.at(1).at("location").at("range").at("end").at("character") == 4);
    CHECK(related.at(2).at("message") == "located context");
}

} // namespace ghoti::tests
