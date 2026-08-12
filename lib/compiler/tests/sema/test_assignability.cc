#include <catch2/catch_test_macros.hpp>
#include <stdx/memory.hh>
#include <stdx/types.hh>

#include "compiler/sema/context.hh"
#include "compiler/sema/type.hh"
#include "helpers/sema.hh"

namespace ghoti::tests {

TEST_CASE("Exact type identity and error recovery") {
    auto [ctx, idx]{helpers::resolve_and_check("const x := 42;")};

    auto& i32_t{ctx->get_type(sema::type_kind::I32)};
    auto& f32_t{ctx->get_type(sema::type_kind::F32)};
    auto& bool_t{ctx->get_type(sema::type_kind::BOOL)};
    auto& undef_t{ctx->get_type(sema::type_kind::UNDEFINED)};
    auto& poison_t{ctx->get_type(sema::type_kind::POISON)};
    auto& noreturn_t{ctx->get_type(sema::type_kind::NORETURN)};

    SECTION("Exact identity matches") {
        CHECK(sema::is_assignable(i32_t, i32_t));
        CHECK(sema::is_assignable(f32_t, f32_t));
        CHECK(sema::is_assignable(bool_t, bool_t));
    }

    SECTION("Poison operands allow error recovery") {
        CHECK(sema::is_assignable(poison_t, i32_t));
        CHECK(sema::is_assignable(i32_t, poison_t));
        CHECK(sema::is_assignable(poison_t, poison_t));
    }

    SECTION("Undefined is assignable to any type") {
        CHECK(sema::is_assignable(undef_t, i32_t));
        CHECK(sema::is_assignable(undef_t, f32_t));
        CHECK(sema::is_assignable(undef_t, bool_t));
    }

    SECTION("Noreturn is assignable to any type") {
        CHECK(sema::is_assignable(noreturn_t, i32_t));
        CHECK(sema::is_assignable(noreturn_t, f32_t));
        CHECK(sema::is_assignable(noreturn_t, bool_t));
    }
}

TEST_CASE("Numeric implicit widening table") {
    auto [ctx, idx]{helpers::resolve_and_check("const x := 42;")};

    auto& u8_t{ctx->get_type(sema::type_kind::U8)};
    auto& u32_t{ctx->get_type(sema::type_kind::U32)};
    auto& u64_t{ctx->get_type(sema::type_kind::U64)};
    auto& usize_t{ctx->get_type(sema::type_kind::USIZE)};
    auto& i32_t{ctx->get_type(sema::type_kind::I32)};
    auto& i64_t{ctx->get_type(sema::type_kind::I64)};
    auto& isize_t{ctx->get_type(sema::type_kind::ISIZE)};
    auto& f32_t{ctx->get_type(sema::type_kind::F32)};
    auto& f64_t{ctx->get_type(sema::type_kind::F64)};
    auto& bool_t{ctx->get_type(sema::type_kind::BOOL)};

    SECTION("u8 widens to u32, u64, usize") {
        CHECK(sema::is_assignable(u8_t, u32_t));
        CHECK(sema::is_assignable(u8_t, u64_t));
        CHECK(sema::is_assignable(u8_t, usize_t));
    }

    SECTION("u32 widens to u64, usize") {
        CHECK(sema::is_assignable(u32_t, u64_t));
        CHECK(sema::is_assignable(u32_t, usize_t));
        CHECK_FALSE(sema::is_assignable(u32_t, u8_t)); // narrowing requires explicit cast
    }

    SECTION("i32 widens to i64, isize") {
        CHECK(sema::is_assignable(i32_t, i64_t));
        CHECK(sema::is_assignable(i32_t, isize_t));
        CHECK_FALSE(sema::is_assignable(i64_t, i32_t)); // narrowing requires explicit cast
    }

    SECTION("f32 widens to f64") {
        CHECK(sema::is_assignable(f32_t, f64_t));
        CHECK_FALSE(sema::is_assignable(f64_t, f32_t)); // narrowing requires explicit cast
    }

    SECTION("Incompatible numeric kinds are rejected") {
        CHECK_FALSE(sema::is_assignable(f32_t, i32_t));
        CHECK_FALSE(sema::is_assignable(i32_t, f32_t));
        CHECK_FALSE(sema::is_assignable(u32_t, i32_t));
        CHECK_FALSE(sema::is_assignable(bool_t, i32_t));
        CHECK_FALSE(sema::is_assignable(i32_t, bool_t));
    }
}

TEST_CASE("Const correctness for values and pointers") {
    auto [ctx, idx]{helpers::resolve_and_check("const x := 42;")};

    auto& i32_mut{ctx->get_type<sema::types::mut::MUTABLE>(sema::type_kind::I32)};
    auto& i32_const{ctx->get_type<sema::types::mut::CONSTANT>(sema::type_kind::I32)};

    auto& ptr_to_mut_i32{ctx->analyzer.get_ctx().get_pointer(sema::types::mut::MUTABLE, i32_mut)};
    auto& ptr_to_const_i32{
        ctx->analyzer.get_ctx().get_pointer(sema::types::mut::MUTABLE, i32_const)};

    SECTION("Value types: by-value assignment allows const stripping and const adding") {
        CHECK(sema::is_assignable(i32_mut, i32_const));
        CHECK(sema::is_assignable(i32_const, i32_mut));
    }

    SECTION("Pointer types: ^T is assignable to ^const T") {
        CHECK(sema::is_assignable(ptr_to_mut_i32, ptr_to_const_i32));
    }

    SECTION("Pointer types: ^const T is NOT assignable to ^T") {
        CHECK_FALSE(sema::is_assignable(ptr_to_const_i32, ptr_to_mut_i32));
    }
}

TEST_CASE("nullptr assignability") {
    auto [ctx, idx]{helpers::resolve_and_check("const x := 42;")};

    auto& nullptr_t{ctx->get_type(sema::type_kind::NULLPTR)};
    auto& i32_mut{ctx->get_type<sema::types::mut::MUTABLE>(sema::type_kind::I32)};
    auto& ptr_to_i32{ctx->analyzer.get_ctx().get_pointer(sema::types::mut::MUTABLE, i32_mut)};
    auto& ref_to_i32{ctx->analyzer.get_ctx().get_reference(sema::types::mut::MUTABLE, i32_mut)};

    SECTION("nullptr is assignable to any pointer type") {
        CHECK(sema::is_assignable(nullptr_t, ptr_to_i32));
    }

    SECTION("nullptr is never assignable to a reference type") {
        CHECK_FALSE(sema::is_assignable(nullptr_t, ref_to_i32));
    }

    SECTION("nullptr is not assignable to plain value types") {
        CHECK_FALSE(sema::is_assignable(nullptr_t, i32_mut));
    }

    SECTION("a pointer is not assignable to nullptr's type") {
        CHECK_FALSE(sema::is_assignable(ptr_to_i32, nullptr_t));
    }
}

TEST_CASE("Slice and array coercions") {
    auto [ctx, idx]{helpers::resolve_and_check("const x := 42;")};

    auto& u8_mut{ctx->get_type<sema::types::mut::MUTABLE>(sema::type_kind::U8)};
    auto& u8_const{ctx->get_type<sema::types::mut::CONSTANT>(sema::type_kind::U8)};

    auto& slice_mut{ctx->analyzer.get_ctx().get_slice(sema::types::mut::MUTABLE, false, u8_mut)};
    auto& slice_const{
        ctx->analyzer.get_ctx().get_slice(sema::types::mut::MUTABLE, false, u8_const)};
    auto& slice_null_term_const{
        ctx->analyzer.get_ctx().get_slice(sema::types::mut::MUTABLE, true, u8_const)};

    auto& array_const_5{
        ctx->analyzer.get_ctx().get_array(sema::types::mut::MUTABLE, false, 5UZ, u8_const)};
    auto& array_null_term_const_6{
        ctx->analyzer.get_ctx().get_array(sema::types::mut::MUTABLE, true, 6UZ, u8_const)};

    SECTION("[]T is assignable to []const T") {
        CHECK(sema::is_assignable(slice_mut, slice_const));
        CHECK_FALSE(sema::is_assignable(slice_const, slice_mut));
    }

    SECTION("[:0]const T is one-way assignable to []const T") {
        CHECK(sema::is_assignable(slice_null_term_const, slice_const));
        CHECK_FALSE(sema::is_assignable(slice_const, slice_null_term_const));
    }

    SECTION("Array to slice coercion") {
        CHECK(sema::is_assignable(array_const_5, slice_const));
        CHECK(sema::is_assignable(array_null_term_const_6, slice_const));
        CHECK(sema::is_assignable(array_null_term_const_6, slice_null_term_const));
        CHECK_FALSE(sema::is_assignable(array_const_5, slice_null_term_const));
    }

    SECTION("[]mut T is assignable to []T, not the other way around") {
        auto& slice_mut_top{
            ctx->analyzer.get_ctx().get_slice(sema::types::mut::MUTABLE, false, u8_mut)};
        auto& slice_const_top{
            ctx->analyzer.get_ctx().get_slice(sema::types::mut::CONSTANT, false, u8_mut)};
        CHECK(sema::is_assignable(slice_mut_top, slice_const_top));
        CHECK_FALSE(sema::is_assignable(slice_const_top, slice_mut_top));
    }

    SECTION("[N]mut T decays to []T, not the other way around") {
        auto& array_mut_top{
            ctx->analyzer.get_ctx().get_array(sema::types::mut::MUTABLE, false, 5UZ, u8_mut)};
        auto& array_const_top{
            ctx->analyzer.get_ctx().get_array(sema::types::mut::CONSTANT, false, 5UZ, u8_mut)};
        auto& slice_const_top{
            ctx->analyzer.get_ctx().get_slice(sema::types::mut::CONSTANT, false, u8_mut)};
        auto& slice_mut_top{
            ctx->analyzer.get_ctx().get_slice(sema::types::mut::MUTABLE, false, u8_mut)};
        CHECK(sema::is_assignable(array_mut_top, slice_const_top));
        CHECK_FALSE(sema::is_assignable(array_const_top, slice_mut_top));
    }
}

} // namespace ghoti::tests
