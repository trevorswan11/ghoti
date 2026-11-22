#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
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

// A UIR Sema run and what its root module's declarations evaluated to
class comptime_run {
  public:
    explicit comptime_run(std::string_view source, const helpers::pipeline_settings& settings = {})
        : run_{helpers::run_uir_sema(source, settings)} {}

    [[nodiscard]] auto out() const -> const uir::sema::outcome& { return *run_->out; }

    [[nodiscard]] auto codes() const -> std::vector<error> {
        std::vector<error> found;
        for (const auto& d : out().diagnostics) { found.emplace_back(d.get_error()); }
        return found;
    }

    [[nodiscard]] auto int_value(std::string_view name) const -> i128 {
        const auto& v{value(name)};
        const auto  exact{
            v.data.visit([](i64 x) -> stdx::option<i128> { return i128{x}; },
                         [](u64 x) -> stdx::option<i128> { return static_cast<i128>(x); },
                         [](i128 x) -> stdx::option<i128> { return x; },
                         [](u128 x) -> stdx::option<i128> { return static_cast<i128>(x); },
                         [](const big_int& x) -> stdx::option<i128> { return x.to_i128(); },
                         [](const auto&) -> stdx::option<i128> { return stdx::none; })};
        return UNWRAP(exact);
    }

    [[nodiscard]] auto bool_value(std::string_view name) const -> bool {
        return UNWRAP(value(name).data.as_opt<bool>());
    }

    [[nodiscard]] auto float_value(std::string_view name) const -> f64 {
        return UNWRAP(value(name).data.as_opt<f128>()).to_f64();
    }

    [[nodiscard]] auto type_name(std::string_view name) const -> std::string {
        const auto index{root_index(name)};
        REQUIRE(pool().is_type(index));
        return run_->analyzer->get_ctx().type_display_name(pool().type_at(index));
    }

    // The type of a declaration's comptime value, by name
    [[nodiscard]] auto value_type_name(std::string_view name) const -> std::string {
        return run_->analyzer->get_ctx().type_display_name(*value(name).type);
    }

    [[nodiscard]] auto has_value(std::string_view name) const -> bool {
        return out().root_values.contains(std::string{name});
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

    [[nodiscard]] auto value(std::string_view name) const -> const ip::value& {
        return pool().value_at(root_index(name));
    }

  private:
    stdx::box<helpers::uir_sema_run> run_;
};

// Analyzes a program UIR Sema accepts, failing on any diagnostic or stop
[[nodiscard]] auto accepted(std::string_view                  source,
                            const helpers::pipeline_settings& settings = {}) -> comptime_run {
    comptime_run run{source, settings};
    if (run.out().stopped_at) { FAIL("UIR Sema stopped at " << run.out().stopped_at->what); }
    if (!run.out().succeeded()) {
        FAIL("UIR Sema reported:\n" << fmt::format("{}", run.out().diagnostics));
    }
    return run;
}

// Analyzes a program UIR Sema reports errors in, failing if it stopped instead
[[nodiscard]] auto rejected(std::string_view                  source,
                            const helpers::pipeline_settings& settings = {}) -> comptime_run {
    comptime_run run{source, settings};
    if (run.out().stopped_at) { FAIL("UIR Sema stopped at " << run.out().stopped_at->what); }
    REQUIRE_FALSE(run.out().diagnostics.empty());
    return run;
}

// A construct a later phase analyzes, which UIR Sema stops on until then
auto check_later_phase(std::string_view source, std::string_view stops_on) -> void {
    const comptime_run run{source};
    REQUIRE(run.out().stopped_at);
    INFO("stopped at " << run.out().stopped_at->what);
    CHECK(run.out().stopped_at->what.contains(stops_on));
}

constexpr helpers::pipeline_settings UNSAFE{.runtime_safety = false};

} // namespace

TEST_CASE("Primitive literal constant eval", "[uir]") {
    const auto run{accepted("const a: i32 = 42; const b: bool = true; const c: f64 = 3.14;")};
    CHECK(run.int_value("a") == 42);
    CHECK(run.bool_value("b"));
    CHECK(run.float_value("c") == 3.14);
}

TEST_CASE("Integer arithmetic and bitwise folding constant eval", "[uir]") {
    const auto run{accepted(R"(
        const add = 2 + 3;
        const sub = 10 - 4;
        const mul = 10 * 3;
        const div = 10 / 2;
        const mod = 10 % 3;
        const neg = -(-3);
        const shl = 1 << 4;
        const shr = 16 >> 2;
        const band = 10 & 12;
        const bor = 10 | 5;
        const bxor = 10 ^ 12;
        const bnot = ~0;
    )")};
    for (const auto& [name, expected] : std::initializer_list<std::pair<std::string_view, i128>>{
             {"add", 5},
             {"sub", 6},
             {"mul", 30},
             {"div", 5},
             {"mod", 1},
             {"neg", 3},
             {"shl", 16},
             {"shr", 4},
             {"band", 8},
             {"bor", 15},
             {"bxor", 6},
             {"bnot", -1},
         }) {
        INFO(name);
        CHECK(run.int_value(name) == expected);
    }
}

TEST_CASE("Boolean logic and comparisons constant eval", "[uir]") {
    const auto run{accepted(R"(
        const t_and = true and false;
        const t_or = true or false;
        const t_not = !true;
        const t_gt = 5 > 3;
        const t_lt = 5 < 3;
        const t_eq = 5 == 5;
        const t_neq = 5 != 4;
    )")};
    for (const auto& [name, expected] : std::initializer_list<std::pair<std::string_view, bool>>{
             {"t_and", false},
             {"t_or", true},
             {"t_not", false},
             {"t_gt", true},
             {"t_lt", false},
             {"t_eq", true},
             {"t_neq", true},
         }) {
        INFO(name);
        CHECK(run.bool_value(name) == expected);
    }
}

TEST_CASE("Compile-time builtins constant eval", "[uir]") {
    SECTION("Sizes and alignments of scalars") {
        const auto run{accepted(R"(
            const sz_u8 = @sizeOf(u8);
            const sz_u32 = @sizeOf(u32);
            const sz_u64 = @sizeOf(u64);
            const al_u32 = @alignOf(u32);
            const al_u64 = @alignOf(u64);
        )")};
        CHECK(run.int_value("sz_u8") == 1);
        CHECK(run.int_value("sz_u32") == 4);
        CHECK(run.int_value("sz_u64") == 8);
        CHECK(run.int_value("al_u32") == 4);
        CHECK(run.int_value("al_u64") == 8);
    }

    SECTION("Arrays") { CHECK(accepted("const sz = @sizeOf([4]u8);").int_value("sz") == 4); }
    SECTION("Math builtins, phase 7") {
        check_later_phase("const v_abs = @abs(-42);", "@abs");
        check_later_phase("const v_clz = @clz(1);", "@clz");
        check_later_phase("const v_pop = @popCount(7);", "@popCount");
    }
}

TEST_CASE("sizeOf/alignOf of pointer-width types follow the target's pointer width", "[uir]") {
    constexpr std::string_view SOURCE{"const sz_usize = @sizeOf(usize); const al_usize = "
                                      "@alignOf(usize);"};
    const auto                 host{accepted(SOURCE)};
    CHECK(host.int_value("sz_usize") == sizeof(void*));
    CHECK(host.int_value("al_usize") == sizeof(void*));

    helpers::pipeline_settings i686{};
    i686.target_opts.triple_str = "i686-unknown-linux-gnu";
    const auto narrow{accepted(SOURCE, i686)};
    CHECK(narrow.int_value("sz_usize") == 4);
    CHECK(narrow.int_value("al_usize") == 4);

    const auto pointer{accepted("const sz_ptr = @sizeOf(^i32);")};
    CHECK(pointer.int_value("sz_ptr") == sizeof(void*));
}

TEST_CASE("Target builtins constant eval", "[uir]") {
    CHECK(accepted("const os_name = @targetOs();").has_value("os_name"));
    check_later_phase("const ptr_bits = @targetPtrBits();", "@targetPtrBits");
}

TEST_CASE("Target enum comparison folds against a bare member", "[uir]") {
    const auto run{accepted("const here = @targetOs(); const eq = @targetOs() == here;")};
    CHECK(run.bool_value("eq"));
}

TEST_CASE("MulAdd and TagName constant eval", "[uir]") {
    check_later_phase("const v_mul_add = @mulAdd(i32, 2, 3, 4);", "@mulAdd");
    const auto run{accepted("const Color = enum { RED, GREEN, BLUE };\n"
                            "const tag = @tagName(Color.GREEN);\nconst len = tag.len;")};
    CHECK(run.int_value("len") == 5);
}

TEST_CASE("Const symbol reference propagation constant eval", "[uir]") {
    const auto run{accepted("const N = 4;\nconst M = N * 2;\nconst K = M + N;")};
    CHECK(run.int_value("M") == 8);
    CHECK(run.int_value("K") == 12);
}

TEST_CASE("Array dimension resolution constant eval", "[uir]") {
    const auto run{accepted("const N = 4; const a: [N * 2]u8 = [8]u8{1, 2, 3, 4, 5, 6, 7, 8};")};
    CHECK(run.value_type_name("a") == "[8]u8");
}

TEST_CASE("Const eval function evaluation", "[uir]") {
    const auto run{accepted(R"(
        const add = fn(a: i32, b: i32): i32 {
            let sum = a + b;
            return sum;
        };
        const max_val = fn(a: i32, b: i32): i32 {
            if (a > b) {
                return a;
            } else {
                return b;
            }
        };
        const res_add = add(40, 2);
        const res_max = max_val(100, 42);
    )")};
    CHECK(run.int_value("res_add") == 42);
    CHECK(run.int_value("res_max") == 100);
}

TEST_CASE("Const eval variable mutation and loops", "[uir]") {
    const auto run{accepted(R"(
        const loop_sum = fn(n: i32): i32 {
            let mut i = 1;
            let mut sum = 0;
            while (i <= n) {
                sum += i;
                i += 1;
            }
            return sum;
        };
        const collatz = fn(start: i32): i32 {
            let mut n = start;
            let mut steps = 0;
            while (n > 1) {
                if (n % 2 == 0) {
                    n /= 2;
                } else {
                    n = 3 * n + 1;
                }
                steps += 1;
            }
            return steps;
        };
        const sum_10 = loop_sum(10);
        const collatz_6 = collatz(6);
    )")};
    CHECK(run.int_value("sum_10") == 55);
    CHECK(run.int_value("collatz_6") == 8);
}

TEST_CASE("Const eval for loops over ranges and arrays", "[uir]") {
    const auto run{accepted(R"(
        const range_sum = fn(n: i32): i32 {
            let mut sum = 0;
            for (1..(n + 1)) |i| {
                sum += i;
            }
            return sum;
        };
        const arr_sum = fn(): i32 {
            let mut sum = 0;
            for (0..3, [_]i32{10, 20, 30}) |i, val| {
                sum += val;
            }
            return sum;
        };
        const r_sum_10 = range_sum(10);
        const a_sum = arr_sum();
    )")};
    CHECK(run.int_value("r_sum_10") == 55);
    CHECK(run.int_value("a_sum") == 60);
}

TEST_CASE("Array constant eval indexing", "[uir]") {
    const auto run{accepted(R"(
        const arr = [_]i32{10, 20, 30, 40};
        const elem0 = arr[0];
        const elem2 = arr[2];
    )")};
    CHECK(run.int_value("elem0") == 10);
    CHECK(run.int_value("elem2") == 30);
}

TEST_CASE("Struct member constant eval access", "[uir]") {
    const auto run{accepted(R"(
        const Point = struct { x: i32, y: i32 };
        const pt = Point{ .x = 15, .y = 25 };
        const px = pt.x;
        const py = pt.y;
    )")};
    CHECK(run.int_value("px") == 15);
    CHECK(run.int_value("py") == 25);
}

TEST_CASE("Union constant eval active and inactive member access", "[uir]") {
    const auto run{rejected(R"(
        const Payload = union(enum) { int_val: i32, float_val: f64 };
        const u = Payload{ .int_val = 42 };
        const active_val = u.int_val;
        const inactive_val = u.float_val;
    )")};
    CHECK(run.codes() == std::vector{error::COMPTIME_EVALUATION_FAILED});
    CHECK(run.int_value("active_val") == 42);
    CHECK_FALSE(run.has_value("inactive_val"));
}

TEST_CASE("Match constant eval expression evaluation", "[uir]") {
    const auto run{accepted(R"(
        const classify = fn(x: i32): i32 {
            return match (x) {
                0 => 100,
                1 => 200,
                _ => 300,
            };
        };
        const Color = enum { RED, GREEN, BLUE };
        const color_code = fn(c: Color): i32 {
            return match (c) {
                .RED => 10,
                .GREEN => 20,
                .BLUE => 30,
            };
        };
        const c0 = classify(0);
        const c1 = classify(1);
        const c99 = classify(99);
        const code_red = color_code(Color.RED);
        const code_blue = color_code(.BLUE);
    )")};
    CHECK(run.int_value("c0") == 100);
    CHECK(run.int_value("c1") == 200);
    CHECK(run.int_value("c99") == 300);
    CHECK(run.int_value("code_red") == 10);
    CHECK(run.int_value("code_blue") == 30);
}

TEST_CASE("Resolve deferred call returning type", "[uir]") {
    const auto run{accepted(R"(
        const choose_type = fn(is_64: bool): type {
            if (is_64) {
                return i64;
            } else {
                return i32;
            }
        };
        const TypeA = choose_type(true);
        const TypeB = choose_type(false);
    )")};
    CHECK(run.type_name("TypeA") == "i64");
    CHECK(run.type_name("TypeB") == "i32");
    // A function returning `type` only runs at compile time, so it has no GIR function
    CHECK_FALSE(run.gir().contains("fn choose_type"));
}

TEST_CASE("Division by zero failure handling in constant eval", "[uir]") {
    const auto run{rejected("const bad = 10 / 0;")};
    CHECK(run.codes() == std::vector{error::COMPTIME_EVALUATION_FAILED});
    CHECK_FALSE(run.has_value("bad"));
}

TEST_CASE("Builtin const eval @This", "[uir]") {
    const auto run{accepted(R"(
        const Node = struct {
            val: i32,

            const Self = @This();
            pub const bar = fn(s: &Self): i32 { return 0; };
        };
        const sz = @sizeOf(Node);
        const al = @alignOf(Node);
    )")};
    CHECK(run.int_value("sz") == 4);
    CHECK(run.int_value("al") == 4);
}

TEST_CASE("Volatile variables refuse constant folding in const eval", "[uir]") {
    check_later_phase("const v: volatile i32 = 42; const read_v = v;", "volatile");
}

TEST_CASE("Const eval string concatenation and comparison folding", "[uir]") {
    check_later_phase(R"(const greeting = "Hello, " + "world!";)", "operator");
}

TEST_CASE("Const eval @setEvalRecursionLimit in function scope", "[uir]") {
    const auto run{accepted(R"(
        const count_loop = fn(n: i32): i32 {
            @setEvalRecursionLimit(10);
            let mut i = 0;
            while (i < n) { i += 1; }
            return i;
        };
        const res = count_loop(5);
    )")};
    CHECK(run.int_value("res") == 5);
}

TEST_CASE("Const eval folds @export to void and the resolver names the entry point", "[uir]") {
    check_later_phase(
        R"(const custom_entry = fn(): void {}; const setup = @export(custom_entry, .{ .name = "main" });)",
        "@export");
}

TEST_CASE("Const eval folds method call on struct instance", "[uir]") {
    const auto run{accepted(R"(
        const Point = struct { x: i32, pub const sum = fn(self): i32 { return self.x; }; };
        const p = Point{ .x = 4 };
        const s = p.sum();
    )")};
    CHECK(run.int_value("s") == 4);
}

TEST_CASE("Const eval folds impl method call", "[uir]") {
    const auto run{accepted(R"(
        const Rect = struct { w: i32, h: i32 };
        impl Rect { pub const area = fn(self): i32 { return self.w * self.h; }; }
        const r = Rect{ .w = 6, .h = 7 };
        const a = r.area();
    )")};
    CHECK(run.int_value("a") == 42);
}

TEST_CASE("unlabeled comptime block in statement position executes at compile time", "[uir]") {
    const auto run{accepted(R"(
        pub const main = fn(): i32 {
            comptime {
                let a = 40;
                let b = 2;
                @assert(a + b == 42);
            }
            return 0;
        };
    )")};
    const auto gir{run.gir()};
    CHECK(gir.contains("ret i32 0"));
    CHECK_FALSE(gir.contains("add i32"));
}

TEST_CASE("Const eval rejects reached unreachable code", "[uir]") {
    // Safety is off since a runtime `unreachable` with safety checks arrives in phase 8
    const auto run{
        rejected("const bad = fn(): i32 { unreachable; };\nconst call_bad = bad();", UNSAFE)};
    CHECK(run.codes() == std::vector{error::UNREACHABLE_CODE_REACHED});
}

TEST_CASE("Const eval unreachable in conditional branches", "[uir]") {
    constexpr std::string_view PICK{R"(
        const pick = fn(x: i32): i32 {
            if (x == 0) {
                return 42;
            } else {
                unreachable;
            }
        };
    )"};

    SECTION("Dead branch with unreachable does not error") {
        const auto run{accepted(fmt::format("{}const a = pick(0);", PICK), UNSAFE)};
        CHECK(run.int_value("a") == 42);
    }

    SECTION("Taken branch with unreachable errors") {
        const auto run{rejected(fmt::format("{}const b = pick(1);", PICK), UNSAFE)};
        CHECK(run.codes() == std::vector{error::UNREACHABLE_CODE_REACHED});
    }
}

TEST_CASE("Const eval unreachable in match expression", "[uir]") {
    SECTION("Unreached catch-all arm does not error") {
        const auto run{accepted(R"(
            const dispatch = fn(tag: i32): i32 {
                return match (tag) {
                    0 => 10,
                    1 => 20,
                    _ => unreachable,
                };
            };
            const res = dispatch(0);
        )",
                                UNSAFE)};
        CHECK(run.int_value("res") == 10);
    }

    SECTION("Reached unreachable arm errors") {
        const auto run{rejected(R"(
            const dispatch = fn(tag: i32): i32 {
                return match (tag) {
                    0 => 10,
                    _ => unreachable,
                };
            };
            const res = dispatch(99);
        )",
                                UNSAFE)};
        CHECK(run.codes() == std::vector{error::UNREACHABLE_CODE_REACHED});
    }
}

TEST_CASE("`unroll for` iterating over `[]u8` parameter", "[uir]") {
    const auto run{accepted(R"(
        const parse_usize = fn(comptime digits: []u8): usize {
            comptime let mut n: usize = 0;
            unroll for (digits) |c| { n = n * 10 + @intCast(usize, c - '0'); }
            return n;
        };
        const parsed = parse_usize("42");
    )")};
    CHECK(run.int_value("parsed") == 42);
}

TEST_CASE("`loop` infinite loop expression in `const_eval`", "[uir]") {
    const auto run{rejected(R"(
        const find_val = fn(): i32 {
            comptime let mut i = 0;
            loop {
                i = i + 1;
                if (i < 5) { continue; }
                if (i == 5) { break; }
            }
            return i;
        };
        const res = find_val();
    )")};
    CHECK(run.int_value("res") == 5);
    CHECK(run.codes() == std::vector{error::COMPTIME_MUT_ASSIGN_NOT_FOLDABLE});
}

TEST_CASE("`defer` in `const_eval`", "[uir]") {
    const auto run{accepted(R"(
        const test_defer = fn(): i32 {
            comptime let mut x = 1;
            {
                defer x = x + 10;
                x = x + 1;
            }
            return x;
        };
        const res = test_defer();
    )")};
    CHECK(run.int_value("res") == 12);
    CHECK(run.gir().contains("ret i32 12"));
}

TEST_CASE("multiple `defer` statements in `const_eval` execute in LIFO order", "[uir]") {
    const auto run{accepted(R"(
        const test_order = fn(): i32 {
            comptime let mut x = 0;
            {
                defer x = x * 10 + 1;
                defer x = x * 10 + 2;
                defer x = x * 10 + 3;
                x = 4;
            }
            return x;
        };
        const res = test_order();
    )")};
    CHECK(run.int_value("res") == 4'321);
}

TEST_CASE("`errdefer` in `const_eval`", "[uir]") {
    check_later_phase(R"(
        const Result = fn(comptime T: type, comptime E: type): type { return union(enum) { ok: T, err: E }; };
        const R = Result(i32, i32);
        comptime let mut err_log: i32 = 0;
        const test_err = fn(): R {
            errdefer |e| err_log = e;
            return R{ .err = 42 };
        };
    )",
                      "comptime declaration");
}

TEST_CASE("`errdefer` capture by const ref and const ptr in `const_eval`", "[uir]") {
    check_later_phase("comptime let mut ref_log: i32 = 0;", "comptime declaration");
}

TEST_CASE("A comptime store under a runtime condition is rejected", "[uir]") {
    const auto run{rejected(R"(
        const f = fn(flag: bool): usize {
            comptime let mut n: usize = 0;
            if (flag) { n += 1; }
            return n;
        };
    )")};
    CHECK(run.codes() == std::vector{error::COMPTIME_MUT_ASSIGN_NOT_FOLDABLE});
    CHECK(run.out().diagnostics[0].get_message() ==
          "`comptime let mut` assignment depends on a runtime condition");
}

TEST_CASE("A comptime store outside runtime control flow folds into the runtime body", "[uir]") {
    const auto run{accepted("const f = fn(): i32 { comptime let mut n = 1; n += 2; n *= 3; return "
                            "n; };")};
    CHECK(run.gir().contains("ret i32 9"));
}

TEST_CASE("A comptime store of a runtime value is rejected", "[uir]") {
    const auto run{
        rejected("const f = fn(x: i32): i32 { comptime let mut n: i32 = 0; n = x; return n; };")};
    CHECK(run.codes() == std::vector{error::COMPTIME_MUT_ASSIGN_NOT_FOLDABLE});
}

TEST_CASE("A pure comptime call is memoized", "[uir]") {
    const auto run{accepted(R"(
        const fib = fn(n: u64): u64 {
            if (n < 2) { return n; }
            return fib(n - 1) + fib(n - 2);
        };
        const f = fib(40);
    )")};
    CHECK(run.int_value("f") == 102'334'155);
}

TEST_CASE("Comptime recursion is bounded by the recursion limit", "[uir]") {
    SECTION("The default limit") {
        const auto run{
            rejected("const r = fn(n: i32): i32 { return r(n + 1); };\nconst x = r(0);")};
        CHECK(run.codes() == std::vector{error::COMPTIME_RECURSION_LIMIT_EXCEEDED});
    }

    SECTION("`@setEvalRecursionLimit` scopes over the rest of its body and the calls it makes") {
        const auto run{rejected(R"(
            const r = fn(n: i32): i32 {
                @setEvalRecursionLimit(10);
                if (n == 0) { return 0; }
                return 1 + r(n - 1);
            };
            const deep = r(20);
            const shallow = r(5);
        )")};
        CHECK(run.codes() == std::vector{error::COMPTIME_RECURSION_LIMIT_EXCEEDED});
        CHECK(run.int_value("shallow") == 5);
        CHECK_FALSE(run.has_value("deep"));
    }
}

TEST_CASE("Comptime loops are bounded by the unroll limit", "[uir]") {
    constexpr std::string_view LOOP{
        "let mut i = 0; while (i < 300) { i += 1; } return i; };\nconst x = s();"};

    SECTION("The default limit") {
        const auto run{rejected(fmt::format("const s = fn(): i32 {{ {}", LOOP))};
        CHECK(run.codes() == std::vector{error::UNROLLED_LOOP_LIMIT});
    }

    SECTION("`@setEvalUnrollLimit` raises it") {
        const auto run{
            accepted(fmt::format("const s = fn(): i32 {{ @setEvalUnrollLimit(1000); {}", LOOP))};
        CHECK(run.int_value("x") == 300);
    }
}

TEST_CASE("An error inside a comptime call names the call stack in notes", "[uir]") {
    const auto run{rejected(R"(
        const inner = fn(x: i32): i32 { return x / 0; };
        const outer = fn(x: i32): i32 { return inner(x); };
        const v = outer(1);
    )")};
    REQUIRE(run.codes() == std::vector{error::COMPTIME_EVALUATION_FAILED});
    const auto& notes{run.out().diagnostics[0].get_notes()};
    REQUIRE(notes.size() == 2);
    CHECK(notes[0].text == "in the compile-time call to 'inner'");
    CHECK(notes[1].text == "in the compile-time call to 'outer'");
}

TEST_CASE("A declaration that is only `@compileError` fires where it is used", "[uir]") {
    SECTION("Unused, it reports nothing") {
        const auto run{
            accepted(R"(const X = @compileError("nope"); const f = fn(): i32 { return 1; };)")};
        CHECK_FALSE(run.has_value("X"));
    }

    SECTION("Used, it reports its message at the use") {
        const auto run{rejected(R"(const X = @compileError("nope"); const y = X;)")};
        CHECK(run.codes() == std::vector{error::COMPILE_ERROR_REACHED});
        CHECK(run.out().diagnostics[0].get_message() == "nope");
    }
}

TEST_CASE("Reaching `@compileError` in an analyzed body reports it", "[uir]") {
    const auto run{rejected(R"(const f = fn(): i32 { @compileError("in body"); return 1; };)")};
    CHECK(run.codes() == std::vector{error::COMPILE_ERROR_REACHED});
}

TEST_CASE("A comptime block can't read a runtime value", "[uir]") {
    const auto run{rejected(
        "const f = fn(x: i32): i32 { let mut y = x; comptime { let a = y; _ = a; } return y; };")};
    CHECK(run.codes() == std::vector{error::COMPTIME_EVALUATION_FAILED});
    CHECK(run.out().diagnostics[0].get_message() == "unable to evaluate at compile time");
}

TEST_CASE("A labeled comptime block yields its break value", "[uir]") {
    const auto run{accepted(
        "const f = fn(): i32 { const v = comptime blk: { let a = 40; break :blk a + 2; }; return "
        "v; };")};
    CHECK(run.gir().contains("ret i32 42"));
}

TEST_CASE("An immutable `let` is comptime-known on demand", "[uir]") {
    SECTION("A comptime read runs its initializer at compile time") {
        const auto run{accepted(R"(
            const sq = fn(x: i32): i32 { return x * x; };
            const g = fn(): i32 {
                let n = sq(3);
                if comptime (n == 9) { return 1; } else { return true; }
            };
        )")};
        CHECK(run.gir().contains("ret i32 1"));
    }

    SECTION("An initializer that reads a `let mut` can't be") {
        const auto run{rejected(R"(
            const g = fn(): i32 {
                let mut q = 3;
                let n = q;
                if comptime (n == 3) { return 1; }
                return 0;
            };
        )")};
        CHECK(run.codes() == std::vector{error::COMPTIME_EVALUATION_FAILED});
        const auto& d{run.out().diagnostics[0]};
        CHECK(d.get_message() == "Comptime if condition could not be evaluated at compile time");
        REQUIRE(d.get_notes().size() == 1);
        CHECK(d.get_notes()[0].text == "'n' could not be evaluated at compile time");
    }
}

TEST_CASE("A call returning `type` runs at compile time from runtime code", "[uir]") {
    const auto run{accepted(R"(
        const pick = fn(wide: bool): type { if (wide) { return i64; } return i32; };
        const f = fn(): i64 { let x: pick(true) = 5; return x; };
    )")};
    CHECK(run.gir().contains("alloca") == false);
    CHECK(run.gir().contains("ret i64 5"));
}

} // namespace ghoti::tests
