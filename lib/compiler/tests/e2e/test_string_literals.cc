#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helpers/codegen.hh"

namespace ghoti::tests {

TEST_CASE("Local string literal indexed directly") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const s := "hi";
            return @as(i32, s[0]);
        };
    )") == 'h');
}

TEST_CASE("String literal escape sequences are decoded to their actual bytes") {
    SECTION("\\n decodes to a single newline byte, not two literal characters") {
        CHECK(helpers::compile_and_run(R"(
            pub const main := fn(): i32 {
                const s := "a\nb";
                return @as(i32, s.len);
            };
        )") == 3);
    }

    SECTION("A decoded escape byte round-trips correctly by index") {
        CHECK(helpers::compile_and_run(R"(
            pub const main := fn(): i32 {
                const s := "a\tb";
                return @as(i32, s[1]);
            };
        )") == '\t');
    }

    SECTION("\\\\ decodes to a single backslash, not two") {
        CHECK(helpers::compile_and_run(R"(
            pub const main := fn(): i32 {
                const s := "a\\b";
                return @as(i32, s.len);
            };
        )") == 3);
    }
}

TEST_CASE("Char literal escape sequences are decoded to their actual bytes") {
    SECTION("\\\" decodes to a literal double-quote byte") {
        CHECK(helpers::compile_and_run(R"(
            pub const main := fn(): i32 {
                const c: u8 = '\"';
                return @as(i32, c);
            };
        )") == '"');
    }
}

TEST_CASE("Local string literal .len is the character count, excluding the sentinel") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const s := "hello";
            return @as(i32, s.len);
        };
    )") == 5);
}

TEST_CASE("A string literal's storage keeps room for the sentinel") {
    SECTION("@sizeOf a [N:0]u8 is N + 1") {
        CHECK(helpers::compile_and_run(R"(
            pub const main := fn(): i32 {
                return @as(i32, @sizeOf([5:0]u8));
            };
        )") == 6);
    }

    SECTION("indexing at .len reads the terminator") {
        CHECK(helpers::compile_and_run(R"(
            pub const main := fn(): i32 {
                const s := "hello";
                return @as(i32, s[s.len]);
            };
        )") == 0);
    }

    SECTION("a `.ptr` walk still finds the terminating NUL") {
        CHECK(helpers::compile_and_run(R"(
            pub const main := fn(): i32 {
                const s := "hi";
                const end: ^u8 = @ptrFromInt(^u8, @intFromPtr(s.ptr) + 2uz);
                return @as(i32, *end);
            };
        )") == 0);
    }
}

TEST_CASE("Local string literal .ptr decays to a valid pointer to its bytes") {
    CHECK(helpers::compile_and_run(R"(
        pub const main := fn(): i32 {
            const s := "hi";
            const p: ^u8 = s.ptr;
            return @as(i32, *p);
        };
    )") == 'h');
}

TEST_CASE("String literal passed directly to a slice parameter") {
    CHECK(helpers::compile_and_run(R"(
        const first_byte := fn(msg: []u8): i32 {
            return @as(i32, msg[0]);
        };
        pub const main := fn(): i32 {
            return first_byte("Ax");
        };
    )") == 'A');
}

TEST_CASE("String literal passed directly to a slice parameter has the character-count length") {
    CHECK(helpers::compile_and_run(R"(
        const len_of := fn(msg: []u8): i32 {
            return @as(i32, msg.len);
        };
        pub const main := fn(): i32 {
            return len_of("hello");
        };
    )") == 5);
}

TEST_CASE("A `[:0]T` null-terminated slice type is usable in value position") {
    SECTION("annotating a string literal binding") {
        CHECK(helpers::compile_and_run(R"(
            pub const main := fn(): i32 {
                const s: [:0]u8 = "hello";
                return @as(i32, s.len);
            };
        )") == 5);
    }

    SECTION("as a function parameter type") {
        CHECK(helpers::compile_and_run(R"(
            const len_of := fn(msg: [:0]u8): i32 {
                return @as(i32, msg.len);
            };
            pub const main := fn(): i32 {
                return len_of("hello");
            };
        )") == 5);
    }
}

TEST_CASE("Top-level scalar const referenced from a function") {
    CHECK(helpers::compile_and_run(R"(
        const FOO := 42;
        pub const main := fn(): i32 {
            return FOO;
        };
    )") == 42);
}

TEST_CASE("Top-level scalar const used inside an expression") {
    CHECK(helpers::compile_and_run(R"(
        const FOO := 42;
        pub const main := fn(): i32 {
            return FOO + 1;
        };
    )") == 43);
}

TEST_CASE("Top-level string const indexed directly (no .ptr)") {
    CHECK(helpers::compile_and_run(R"(
        const GREETING := "hi";
        pub const main := fn(): i32 {
            return @as(i32, GREETING[0]);
        };
    )") == 'h');
}

TEST_CASE("Top-level string const .ptr decays to a valid pointer to its bytes") {
    CHECK(helpers::compile_and_run(R"(
        const GREETING := "hi";
        pub const main := fn(): i32 {
            const p: ^u8 = GREETING.ptr;
            return @as(i32, *p);
        };
    )") == 'h');
}

} // namespace ghoti::tests
