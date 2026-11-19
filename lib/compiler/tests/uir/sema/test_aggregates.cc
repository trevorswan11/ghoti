#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/types.hh>

#include "compiler/gir/dumper.hh"
#include "compiler/ip/index.hh"
#include "compiler/ip/pool.hh"
#include "compiler/sema/error.hh"
#include "compiler/uir/sema/sema.hh"
#include "helpers/uir_diff.hh"
#include "support/big_int.hh"
#include "support/int128.hh"
#include "support/test.hh"

namespace ghoti::tests {

namespace {

using sema::error;

// A UIR Sema run of one program, read back by declaration name
class aggregate_run {
  public:
    explicit aggregate_run(std::string_view source, const helpers::pipeline_settings& settings = {})
        : run_{helpers::run_uir_sema(source, settings)} {}

    [[nodiscard]] auto out() const -> const uir::sema::outcome& { return *run_->out; }

    [[nodiscard]] auto messages() const -> std::vector<std::string> {
        std::vector<std::string> found;
        for (const auto& d : out().diagnostics) {
            found.emplace_back(d.get_message().value_or(std::string{}));
        }
        return found;
    }

    [[nodiscard]] auto codes() const -> std::vector<error> {
        std::vector<error> found;
        for (const auto& d : out().diagnostics) { found.emplace_back(d.get_error()); }
        return found;
    }

    [[nodiscard]] auto int_value(std::string_view name) const -> i128 {
        const auto& v{pool().value_at(root_index(name))};
        const auto  exact{
            v.data.visit([](i64 x) -> stdx::option<i128> { return i128{x}; },
                         [](u64 x) -> stdx::option<i128> { return static_cast<i128>(x); },
                         [](const auto&) -> stdx::option<i128> { return stdx::none; })};
        return UNWRAP(exact);
    }

    [[nodiscard]] auto type_name(std::string_view name) const -> std::string {
        const auto index{root_index(name)};
        REQUIRE(pool().is_type(index));
        return run_->analyzer->get_ctx().type_display_name(pool().type_at(index));
    }

    [[nodiscard]] auto gir() const -> std::string {
        REQUIRE(out().succeeded());
        std::ostringstream text;
        gir::dumper{text}.dump(*out().gir);
        return text.str();
    }

  private:
    [[nodiscard]] auto pool() const -> ip::pool& { return run_->analyzer->get_ctx().pool.intern(); }

    [[nodiscard]] auto root_index(std::string_view name) const -> ip::index {
        const auto found{out().root_values.find(std::string{name})};
        if (found == out().root_values.end()) { FAIL(fmt::format("'{}' has no value", name)); }
        return found->second;
    }

  private:
    stdx::box<helpers::uir_sema_run> run_;
};

[[nodiscard]] auto accepted(std::string_view source) -> aggregate_run {
    aggregate_run run{source};
    if (run.out().stopped_at) { FAIL("UIR Sema stopped at " << run.out().stopped_at->what); }
    if (!run.out().succeeded()) {
        FAIL("UIR Sema reported:\n" << fmt::format("{}", run.out().diagnostics));
    }
    return run;
}

[[nodiscard]] auto rejected(std::string_view source) -> aggregate_run {
    aggregate_run run{source};
    if (run.out().stopped_at) { FAIL("UIR Sema stopped at " << run.out().stopped_at->what); }
    REQUIRE_FALSE(run.out().diagnostics.empty());
    return run;
}

} // namespace

TEST_CASE("A by-value field of its own type is an infinite size cycle", "[uir]") {
    const auto run{rejected("const A = struct { a: A };")};
    CHECK(run.codes() == std::vector{error::CYCLIC_DEPENDENCY});
    CHECK(run.messages() ==
          std::vector<std::string>{
              "Field 'a' has an incomplete type; creates an infinite size cycle"});
}

TEST_CASE("A type whose layout reads its own size depends on itself", "[uir]") {
    const auto run{rejected("const A = struct { x: [@sizeOf(A)]u8 };")};
    CHECK(run.codes() == std::vector{error::CYCLIC_DEPENDENCY});
    CHECK(run.messages() == std::vector<std::string>{"the layout of 'struct' depends on itself"});
}

TEST_CASE("Types that point at themselves or each other resolve", "[uir]") {
    const auto run{accepted(R"(
        const N = struct { next: ^N, v: i32 };
        const A = struct {
            b: ^B,
            pub const k = @sizeOf(B);
        };
        const B = struct { a: ^A, x: i64 };
        const size_n = @sizeOf(N);
        const ka = A.k;
    )")};
    CHECK(run.int_value("size_n") == 16);
    CHECK(run.int_value("ka") == 16);
}

TEST_CASE("A declared type is named for its declaration", "[uir]") {
    const auto run{accepted("const P = struct { x: i32 }; const E = enum { a }; const Q = P;")};
    CHECK(run.type_name("P") == "P");
    CHECK(run.type_name("E") == "E");
    CHECK(run.type_name("Q") == "P");
}

TEST_CASE("A field an initializer leaves out takes its default", "[uir]") {
    const auto run{accepted(R"(
        const P = struct { x: i32, y: i32 = 7 };
        const p = P{ .x = 1 };
        const py = p.y;
        const make = fn(a: i32): P { return .{ .x = a }; };
    )")};
    CHECK(run.int_value("py") == 7);
    CHECK(run.gir().contains("store 7,"));
}

TEST_CASE("An enum's values count up from the last one written", "[uir]") {
    const auto run{accepted(R"(
        const E = enum : u8 { a = 3, b, c = 10, d };
        const eb = E.b;
        const ed = E.d;
    )")};
    CHECK(run.int_value("eb") == 4);
    CHECK(run.int_value("ed") == 11);
}

TEST_CASE("A struct initializer names each field once, and every field without a default",
          "[uir]") {
    SECTION("A missing field") {
        const auto run{rejected("const P = struct { x: i32, y: i32 }; const p = P{ .x = 1 };")};
        CHECK(run.codes() == std::vector{error::MISSING_FIELD});
        CHECK(run.messages() ==
              std::vector<std::string>{"Struct initializer missing required field: y"});
    }
    SECTION("An unknown field") {
        const auto run{rejected("const P = struct { x: i32 }; const p = P{ .x = 1, .z = 2 };")};
        CHECK(run.codes() == std::vector{error::UNKNOWN_FIELD});
    }
    SECTION("A field named twice") {
        const auto run{rejected("const P = struct { x: i32 }; const p = P{ .x = 1, .x = 2 };")};
        CHECK(run.codes() == std::vector{error::DUPLICATE_FIELD});
    }
}

TEST_CASE("A runtime index checks its bounds through the prelude's panic handler", "[uir]") {
    const auto run{accepted("const at = fn(a: [4]u8, i: usize): u8 { return a[i]; };")};
    const auto gir{run.gir()};
    CHECK(gir.contains("lt bool param.1, 4"));
    CHECK(gir.contains("call @panic_handler("));
    CHECK(gir.contains("fn panic_handler("));
}

TEST_CASE("A match at compile time binds a union's payload", "[uir]") {
    const auto run{accepted(R"(
        const U = union(enum) { a: i32, b: bool };
        const pick = fn(u: U): i32 {
            return match (u) {
                .a => |v| v,
                .b => 0,
            };
        };
        const five = pick(U{ .a = 5 });
        const zero = pick(.{ .b = true });
    )")};
    CHECK(run.int_value("five") == 5);
    CHECK(run.int_value("zero") == 0);
}

TEST_CASE("A runtime match must cover every value", "[uir]") {
    const auto run{rejected(R"(
        const Color = enum { red, green };
        const code = fn(c: Color): i32 { return match (c) { .red => 1 }; };
    )")};
    CHECK(run.codes() == std::vector{error::MISSING_FIELD});
}

TEST_CASE("An immutable `let` holding a struct is comptime-known on demand", "[uir]") {
    const auto run{accepted(R"(
        const P = struct { x: i32 };
        const f = fn(): i32 {
            let p = P{ .x = 4 };
            if comptime (p.x == 4) { return 1; }
            return 0;
        };
    )")};
    CHECK(run.gir().contains("ret i32 1"));
}

TEST_CASE("A range capture converts to a narrower integer that holds every value", "[uir]") {
    const auto run{accepted(R"(
        const take = fn(x: u8): i32 { return x; };
        const sum = fn(): i32 {
            let mut s: i32 = 0;
            for (200..=198) |i| { s += take(i); }
            return s;
        };
    )")};
    CHECK(run.gir().contains("int_cast u8"));
}

TEST_CASE("A mutable address of an immutable array element is an error", "[uir]") {
    // The old pipeline accepts this while rejecting `buf[0] = 3` (known_legacy_bugs.md)
    const auto run{rejected(R"(
        const touch = fn(p: ^mut u8): void { *p = 3; };
        const f = fn(): void {
            let mut buf: [4]u8 = undefined;
            touch(^mut buf[0]);
        };
    )")};
    CHECK(run.codes() == std::vector{error::ASSIGNMENT_TO_CONST});
    CHECK(run.messages() ==
          std::vector<std::string>{
              "Cannot take a mutable address of an element of a non-mutable array or slice"});
}

TEST_CASE("Assigning a tagged union's field makes it the active one", "[uir]") {
    const auto run{accepted(R"(
        const U = union(enum) { flag: bool, val: i32 };
        const f = fn(): i32 {
            let mut u = U{ .flag = true };
            u.val = 7;
            return u.val;
        };
    )")};
    CHECK(run.gir().contains("store 1, "));
}

TEST_CASE("Untyped initializer arms of an `if` take the type the result needs", "[uir]") {
    const auto run{accepted(R"(
        const P = struct { x: i32, y: i32 };
        const pick = fn(hi: bool): P {
            return if (hi) .{ .x = 30, .y = 12 } else .{ .x = 1, .y = 1 };
        };
    )")};
    CHECK(run.gir().contains("store 30, "));
}

TEST_CASE("A mutable reference to an array views it as a mutable slice", "[uir]") {
    const auto run{accepted(R"(
        const bump = fn(s: []mut i32): void { s[0] = s[0] + 10; };
        const f = fn(): i32 {
            let mut arr = [3]mut i32{1, 2, 3};
            bump(&mut arr);
            return arr[0];
        };
    )")};
    CHECK(run.gir().contains("call @bump("));
}

TEST_CASE("A negative untyped integer wraps into an unsigned `@as`", "[uir]") {
    const auto run{accepted(R"(
        const A: u8 = @as(u8, -1);
        const B: u16 = @as(u16, -1);
    )")};
    CHECK(run.int_value("A") == 255);
    CHECK(run.int_value("B") == 65'535);
}

} // namespace ghoti::tests
