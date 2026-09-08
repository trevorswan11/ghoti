#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <stdx/memory.hh>
#include <stdx/utility.hh>

#include "compiler/module/overlay_loader.hh"
#include "driver/cmd/lsp/diagnostics.hh"
#include "driver/cmd/lsp/session.hh"
#include "support/test.hh"

namespace ghoti::tests {

TEST_CASE("to_lsp_diagnostics is empty for a clean module") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_diagnostics_clean.gh"};
    CHECK(loader.add(path, "pub const x := 5;\n"));

    auto       session{stdx::make_box<lsp::analysis_session>(loader, std::cerr)};
    const auto module{UNWRAP(session->analyze(path))};
    CHECK(lsp::to_lsp_diagnostics(*module).empty());
}

TEST_CASE("to_lsp_diagnostics reports a syntax error with a code, message, and range") {
    mod::overlay_loader         loader;
    const std::filesystem::path path{"test_diagnostics_broken.gh"};
    CHECK(loader.add(path, "pub const x := ;\n"));

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

TEST_CASE("a shared session that analyzes two impls of one interface keeps inherited defaults") {
    mod::overlay_loader loader;
    CHECK(loader.add(std::filesystem::path{"reader.gh"}, R"(
        pub const Result := fn(T: type, E: type): type { return union { ok: T, err: E }; };
        pub const Reader := interface {
            Error: type;
            pub const read := fn(&mut self, buf: []mut u8): Result(usize, Error);
            pub const readAll := fn(&mut self, buf: []mut u8): Result(usize, Error) {
                var i: usize = 0;
                loop {
                    if (i == buf.len) { break; }
                    const n := self.read(buf[i..])?;
                    if (n == 0) { break; }
                    i += n;
                };
                return .{ .ok = i };
            };
        };
    )"));
    constexpr std::string_view impl_body{R"(
        import "reader.gh" as reader;
        pub const {0} := struct {{ pub data: []mut u8, pub pos: usize = 0 }};
        impl reader::Reader for {0} {{
            using Error = u8;
            pub const read := fn(&mut self, buf: []mut u8): reader::Result(usize, Error) {{
                const rem := self.data.len - self.pos;
                const n := if (buf.len < rem) buf.len else rem;
                self.pos += n;
                return .{{ .ok = n }};
            }};
        }}
        pub const drain := fn(s: &mut {0}, out: []mut u8): usize {{
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

} // namespace ghoti::tests
