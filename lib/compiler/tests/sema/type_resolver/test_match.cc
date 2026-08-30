#include <sstream>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <stdx/types.hh>

#include "compiler/sema/error.hh"
#include "compiler/sema/symbol.hh"
#include "compiler/sema/type.hh"
#include "helpers/common.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

using mock_file = helpers::mock_file;

TEST_CASE("Resolving well-formed enum matching") {
    helpers::resolve_and_check("using E = enum { a }; _ = match (E) { .a => 5 };");
    helpers::resolve_and_check("using E = enum { a, b }; _ = match (E) { .a => 5, .b => 6 };");
    helpers::resolve_and_check("using E = enum { a, b }; _ = match (E) { .a => 5, _ => 6 };");
    helpers::resolve_and_check("using E = enum { a, b, _ }; _ = match (E) { .a => 5, _ => 6 };");
}

TEST_CASE("Resolving a match on a compile-time type value") {
    helpers::resolve_and_check("const T := i32; _ = match (T) { i32 => 1, bool => 2, _ => 0 };");
    helpers::resolve_and_check("const T := ^i32; _ = match (T) { i32 => 1, ^i32 => 2, _ => 0 };");
    // A non-selected arm may reference undeclared names; it is never type-checked.
    helpers::resolve_and_check(
        "const T := i32; _ = match (T) { i32 => 1, bool => nope_not_real, _ => 0 };");
}

TEST_CASE("A match on a type requires a catch-all arm") {
    helpers::test_resolver_fail("const T := i32; _ = match (T) { i32 => 1, bool => 2 };",
                                sema::diagnostic{"A 'match' on a type requires a catch-all '_' arm",
                                                 sema::error::ILLEGAL_MATCH_PATTERN,
                                                 std::pair{0UZ, 20UZ}});
}

TEST_CASE("A match on a type rejects value patterns and captures") {
    helpers::test_resolver_fail(
        "const T := i32; _ = match (T) { 5 => 1, _ => 0 };",
        sema::diagnostic{"A 'match' on a type expects every arm pattern to be a type",
                         sema::error::ILLEGAL_MATCH_PATTERN,
                         std::pair{0UZ, 32UZ}});
    helpers::test_resolver_fail("const T := i32; _ = match (T) { i32 => |x| 1, _ => 0 };",
                                sema::diagnostic{"A 'match' on a type cannot bind a capture",
                                                 sema::error::ILLEGAL_MATCH_PATTERN,
                                                 std::pair{0UZ, 40UZ}});
}

TEST_CASE("Resolving well-formed union matching") {
    helpers::resolve_and_check("using U = union { a: i32 }; _ = match (U) { .a => 5 };");
    helpers::resolve_and_check(
        "using U = union { a: i32, b: i64 }; _ = match (U) { .a => 5, .b => 4 };");
    helpers::resolve_and_check(
        "using U = union { a: i32, b: i64 }; _ = match (U) { .a => 5, _ => 4 };");
}

TEST_CASE("Resolving capturing match arms") {
    const auto test_arm_capture = [](std::string_view input,
                                     std::string_view capture_name,
                                     usize            capture_idx,
                                     auto&&           expected_type_fn) -> void {
        auto [ctx, _]{helpers::resolve_and_check(input)};
        auto [sym, data, actual_data]{
            ctx->get_type_sym_info<sema::symbols::match_capture>(capture_name, capture_idx)};
        CHECK(actual_data == expected_type_fn(*ctx));
    };

    test_arm_capture("using E = enum { a }; _ = match (E) { .a => |a| 5 };",
                     "a",
                     2,
                     [](helpers::sema_test_context& ctx) -> auto& {
                         return ctx.get_type(sema::type_kind::ENUM, 1);
                     });

    test_arm_capture("using U = union { a: i32 }; _ = match (U) { .a => |a| 5 };",
                     "a",
                     2,
                     [](helpers::sema_test_context& ctx) -> auto& {
                         return ctx.get_type(sema::type_kind::I32);
                     });
}

TEST_CASE("Resolving well-formed builtin-type matching") {
    SECTION("Bools") {
        helpers::resolve_and_check("_ = match (true) { true => {}, false => {} };");
        helpers::resolve_and_check("_ = match (true) { true => {}, _ => {} };");
    }

    SECTION("Bytes") {
        std::ostringstream arms;
        for (usize i{0}; i < 256; ++i) { fmt::print(arms, "{} => 0,", i); }
        const auto input{fmt::format("_ = match('0') {{ {} }};", arms.view())};
        helpers::resolve_and_check(input);
        helpers::resolve_and_check("_ = match ('0') { 0 => {}, _ => {} };");
    }
}

TEST_CASE("Resolving match arm captures with reference/pointer modifiers") {
    helpers::resolve_and_check(
        "using U = union { a: i32 }; var u := U{ .a = 5 }; _ = match (u) { .a => |&v| v };");
    helpers::resolve_and_check(
        "using U = union { a: i32 }; var u := U{ .a = 5 }; _ = match (u) { .a => |&mut v| v };");
    helpers::resolve_and_check(
        "using U = union { a: i32 }; var u := U{ .a = 5 }; _ = match (u) { .a => |^v| *v };");
    helpers::resolve_and_check(
        "using U = union { a: i32 }; var u := U{ .a = 5 }; _ = match (u) { .a => |^mut v| *v };");

    // Whole-value (non-union) captures accept the same modifiers
    helpers::resolve_and_check("var x: i32 = 1; _ = match (x) { 1 => |&mut v| v, _ => 0 };");
}

TEST_CASE("Illegal mutable capture of an immutable match arm value") {
    SECTION("By mutable reference, union field") {
        helpers::test_resolver_fail(
            "using U = union { a: i32 }; const u := U{ .a = 5 }; match (u) { .a => |&mut v| v };",
            sema::diagnostic{"Cannot capture an immutable value by mutable reference",
                             sema::error::ASSIGNMENT_TO_CONST,
                             std::pair{0UZ, 76UZ}});
    }

    SECTION("By mutable pointer, union field") {
        helpers::test_resolver_fail(
            "using U = union { a: i32 }; const u := U{ .a = 5 }; match (u) { .a => |^mut v| *v };",
            sema::diagnostic{"Cannot capture an immutable value by mutable pointer",
                             sema::error::ASSIGNMENT_TO_CONST,
                             std::pair{0UZ, 76UZ}});
    }

    SECTION("By mutable reference, whole value") {
        helpers::test_resolver_fail(
            "const x: i32 = 1; match (x) { 1 => |&mut v| v, _ => 0 };",
            sema::diagnostic{"Cannot capture an immutable value by mutable reference",
                             sema::error::ASSIGNMENT_TO_CONST,
                             std::pair{0UZ, 41UZ}});
    }
}

TEST_CASE("Illegal match over an untagged union") {
    helpers::test_resolver_fail(
        "using U = extern union { a: i32, b: i64 }; match (U) { .a => 5, .b => 4 };",
        sema::diagnostic{"Cannot match on an untagged union; it has no runtime tag",
                         sema::error::TYPE_MISMATCH,
                         std::pair{0UZ, 50UZ}});
}

TEST_CASE("Illegal resolved enum matcher type") {
    SECTION("Non-exhaustive") {
        helpers::test_resolver_fail(
            "using E = enum { a, _ }; match (E) { 3 => 5 };",
            sema::diagnostic{
                "Match expressions over non-exhaustive enums must have a catch all arm",
                sema::error::ILLEGAL_MATCH_PATTERN,
                std::pair{0UZ, 25UZ},
            });
    }

    SECTION("Duplicate enumerations") {
        const auto expected_diag = [](std::string_view suffix, usize col) -> sema::diagnostic {
            return {
                fmt::format("Match expression contains duplicate enumeration{}", suffix),
                sema::error::DUPLICATE_ENUMERATION,
                std::pair{0UZ, col},
            };
        };

        helpers::test_resolver_fail("using E = enum { a }; match (E) { .a => 5, .a => 4 };",
                                    expected_diag(": a", 22UZ));
        helpers::test_resolver_fail(
            "using E = enum { a, b }; match (E) { .a => 5, .a => 4, .b => 3, .b => 2 };",
            expected_diag("s: a, b", 25UZ));
    }

    SECTION("Unknown enumerations") {
        const auto expected_diag = [](std::string_view suffix) -> sema::diagnostic {
            return {
                fmt::format("Match expression contains unknown enumeration{}", suffix),
                sema::error::UNKNOWN_ENUMERATION,
                std::pair{0UZ, 22UZ},
            };
        };

        helpers::test_resolver_fail("using E = enum { a }; match (E) { .a => 5, .b => 4 };",
                                    expected_diag(": b"));
        helpers::test_resolver_fail(
            "using E = enum { a }; match (E) { .a => 5, .b => 4, .c => 3 };",
            expected_diag("s: b, c"));
    }
}

TEST_CASE("Illegal resolved union matcher type") {
    SECTION("Pattern ast node restriction") {
        helpers::test_resolver_fail(
            "using U = union { a: i32 }; match (U) { 3 => 5 };",
            sema::diagnostic{
                "Match arm may only have an implicit access pattern in this context",
                sema::error::ILLEGAL_MATCH_PATTERN,
                std::pair{0UZ, 40UZ},
            });
    }

    SECTION("Duplicate fields") {
        const auto expected_diag = [](std::string_view suffix, usize col) -> sema::diagnostic {
            return {
                fmt::format("Match expression contains duplicate union field{}", suffix),
                sema::error::DUPLICATE_FIELD,
                std::pair{0UZ, col},
            };
        };

        helpers::test_resolver_fail("using U = union { a: i32 }; match (U) { .a => 5, .a => 4 };",
                                    expected_diag(": a", 28UZ));
        helpers::test_resolver_fail(
            "using U = union { a: i32, b: i64 }; match (U) { .a => 5, .a => 4, .b => 3, .b => 2 };",
            expected_diag("s: a, b", 36UZ));
    }

    SECTION("Missing fields") {
        const auto expected_diag = [](std::string_view suffix, usize col) -> sema::diagnostic {
            return {
                fmt::format("Match expression is non-exhaustive; missing union field{}", suffix),
                sema::error::MISSING_FIELD,
                std::pair{0UZ, col},
            };
        };

        helpers::test_resolver_fail("using U = union { a: i32, b: i64 }; match (U) { .a => 5 };",
                                    expected_diag(": b", 36UZ));
        helpers::test_resolver_fail(
            "using U = union { a: i32, b: i64, c: f32 }; match (U) { .a => 5 };",
            expected_diag("s: b, c", 44UZ));
    }

    SECTION("Unknown fields") {
        const auto expected_diag = [](std::string_view suffix) -> sema::diagnostic {
            return {
                fmt::format("Match expression contains unknown union field{}", suffix),
                sema::error::UNKNOWN_FIELD,
                std::pair{0UZ, 28UZ},
            };
        };

        helpers::test_resolver_fail("using U = union { a: i32 }; match (U) { .a => 5, .b => 4 };",
                                    expected_diag(": b"));
        helpers::test_resolver_fail(
            "using U = union { a: i32 }; match (U) { .a => 5, .b => 4, .c => 3 };",
            expected_diag("s: b, c"));
    }
}

TEST_CASE("Illegal match arms with primitives") {
    SECTION("Required catch-all") {
        const auto expected_diag = [](std::string_view kind) -> sema::diagnostic {
            return {
                fmt::format("Matching on type '{}' requires a catch all arm with a pattern of '_'",
                            kind),
                sema::error::TYPE_MISMATCH,
                std::pair{0UZ, 7UZ},
            };
        };

        helpers::test_resolver_fail("match (1) { 3 => 5 };", expected_diag("i32"));
        helpers::test_resolver_fail("match (1l) { 3 => 5 };", expected_diag("i64"));
        helpers::test_resolver_fail("match (1z) { 3 => 5 };", expected_diag("isize"));
        helpers::test_resolver_fail("match (1u) { 3 => 5 };", expected_diag("u32"));
        helpers::test_resolver_fail("match (1ul) { 3 => 5 };", expected_diag("u64"));
        helpers::test_resolver_fail("match (1UZ) { 3 => 5 };", expected_diag("usize"));
    }

    SECTION("Optional catch-all") {
        const auto expected_diag = [](std::string_view kind,
                                      usize            required_arms) -> sema::diagnostic {
            return {
                fmt::format("Matching on type '{}' requires a catch all arm with "
                            "a pattern of '_' or exactly {} patterned arms",
                            kind,
                            required_arms),
                sema::error::TYPE_MISMATCH,
                std::pair{0UZ, 7UZ},
            };
        };

        helpers::test_resolver_fail("match ('0') { 3 => 5 };", expected_diag("u8", 256));
        helpers::test_resolver_fail("match (false) { 3 => 5 };", expected_diag("bool", 2));
    }
}

TEST_CASE("Illegal resolved builtin matcher type") {
    SECTION("Floats") {
        const auto expected_diag = [] -> sema::diagnostic {
            return {
                "Cannot match on floats due to precision; use an if statement with explicit "
                "precision handling",
                sema::error::TYPE_MISMATCH,
                std::pair{0UZ, 7UZ},
            };
        };

        helpers::test_resolver_fail("match (2.3f) { 3 => 5 };", expected_diag());
        helpers::test_resolver_fail("match (2.3) { 3 => 5 };", expected_diag());
    }

    SECTION("Empty types") {
        const auto expected_diag = [] -> sema::diagnostic {
            return {
                "Empty types cannot be matched on",
                sema::error::TYPE_MISMATCH,
                std::pair{0UZ, 7UZ},
            };
        };

        helpers::test_resolver_fail("match ({}) { 3 => 5 };", expected_diag());
        helpers::test_resolver_fail("match (undefined) { 3 => 5 };", expected_diag());
    }

    SECTION("Other illegal builtin types") {
        helpers::test_resolver_fail(
            "using A = opaque; match (A) { 3 => 5 };",
            sema::diagnostic{
                "Can only match on integers, bytes, and booleans; found 'opaque'",
                sema::error::TYPE_MISMATCH,
                std::pair{0UZ, 25UZ},
            });
    }
}

TEST_CASE("Illegal resolved arbitrary matcher type") {
    const auto expected_diag = [](std::string_view kind, usize col) -> sema::diagnostic {
        return {
            fmt::format("Can only match on enums, unions, and certain primitive types; found '{}'",
                        kind),
            sema::error::TYPE_MISMATCH,
            std::pair{0UZ, col},
        };
    };

    // A `type`-valued scrutinee is now a compile-time type match, which needs a `_` arm.
    helpers::test_resolver_fail("match (@typeOf(i32)) { 3 => 5 };",
                                sema::diagnostic{"A 'match' on a type requires a catch-all '_' arm",
                                                 sema::error::ILLEGAL_MATCH_PATTERN,
                                                 std::pair{0UZ, 0UZ}});
    helpers::test_resolver_fail("match (^4) { 3 => 5 };", expected_diag("pointer", 7));
    helpers::test_resolver_fail("match (&mut 4) { 3 => 5 };", expected_diag("reference", 7));
    helpers::test_resolver_fail("var a: fn(): void = undefined; match (a) { 3 => 5 };",
                                expected_diag("function", 38));
    helpers::test_resolver_fail(
        "import std; match (std) { 3 => 5 };",
        helpers::make_vector<mock_file>(mock_file{"std.gh", "pub extern var a: i32;", "std"}),
        expected_diag("module", 19));
}

} // namespace ghoti::tests
