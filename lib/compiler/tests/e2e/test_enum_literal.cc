#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "compiler/sema/error.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

namespace {

constexpr std::string_view LEVEL{"const Level = enum { fast, small, safe };\n"};

[[nodiscard]] auto with_level(std::string_view body) -> std::string {
    return std::string{LEVEL} + std::string{body};
}

} // namespace

TEST_CASE("An uncoerced `.name` is an @EnumLiteral()") {
    CHECK(helpers::compile_and_run(R"(
        pub const main = fn(): i32 {
            let mut out: i32 = 0;
            if (@TypeOf(.x) == @EnumLiteral()) { out += 1; }
            if (@typeInfo(@EnumLiteral()) == .enum_literal) { out += 2; }
            const m = .safe;
            out += @intCast(i32, @tagName(m).len) * 4; // "safe"
            if (.a == .a and .a != .b) { out += 32; }
            return out;
        };
    )") == 1 + 2 + 16 + 32);
}

TEST_CASE("An enum literal becomes an enum value wherever one is expected") {
    CHECK(helpers::compile_and_run(with_level(R"(
        const fast = .fast;
        const small = .small;
        const as_global: Level = small;
        const takes = fn(l: Level): i32 { return @intCast(i32, @backingInt(l)); };
        const gives = fn(): Level { return small; };
        pub const main = fn(): i32 {
            let local: Level = fast;
            let mut out: i32 = 0;
            if (local == .fast) { out += 1; }
            if (local == fast and local != small) { out += 2; }
            out += takes(.safe) * 4;                       // 2
            out += @intCast(i32, @backingInt(gives())) * 16;  // 1
            out += @intCast(i32, @backingInt(as_global)) * 32; // 1
            return out;
        };
    )")) == 1 + 2 + 8 + 16 + 32);
}

TEST_CASE("An enum literal becomes a tagged union's `void` field") {
    CHECK(helpers::compile_and_run(R"(
        const Opt = fn(T: type): type { return union { some: T, none: void }; };
        const nothing = .none;
        pub const main = fn(): i32 {
            let o: Opt(i32) = nothing;
            return if (o == .none) 42 else 0;
        };
    )") == 42);
    DISCARD(helpers::expect_compile_error(R"(
        const Opt = fn(T: type): type { return union { some: T, none: void }; };
        const some = .some;
        pub const main = fn(): i32 { let o: Opt(i32) = some; _ = o; return 0; };
    )"));
}

TEST_CASE("A comptime `auto` parameter takes an enum literal") {
    CHECK(helpers::compile_and_run(with_level(R"(
        const pick = fn(comptime k: auto): i32 {
            return match comptime (k) { .fast => 1, .small => 2, _ => 3 };
        };
        const name_len = fn(comptime k: auto): usize { return @tagName(k).len; };
        pub const main = fn(): i32 {
            return pick(.small) + pick(.other) * 10 + @intCast(i32, name_len(.abc)) * 50;
        };
    )")) == 2 + 30 + 150);
}

TEST_CASE("Enum literal errors name what went wrong") {
    SECTION("a name the enum lacks") {
        DISCARD(helpers::expect_compile_error(
            with_level("const slow = .slow; pub const main = fn(): i32 { let l: Level = slow; "
                       "_ = l; return 0; };")));
    }

    SECTION("a runtime binding") {
        const auto res{helpers::resolve_diags("const f = fn(): void { let x = .fast; _ = x; };")};
        CHECK(res.message_contains("an enum literal only exists at compile time"));
    }

    SECTION("a runtime `auto` parameter") {
        const auto res{
            helpers::resolve_diags("const plain = fn(k: auto): i32 { _ = k; return 0; }; "
                                   "const f = fn(): i32 { return plain(.fast); };")};
        CHECK(res.message_contains("Argument 1 is an enum literal"));
    }

    SECTION("a type with no members still rejects `.name`") {
        const auto res{helpers::resolve_diags("const f = fn(): void { let x: i32 = .fast; };")};
        CHECK(res.message_contains("Can only access inner objects"));
    }
}

} // namespace ghoti::tests
