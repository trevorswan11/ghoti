#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "support/big_int.hh"
#include "support/int128.hh"

namespace ghoti::tests {

namespace {

[[nodiscard]] auto big(std::string_view digits) -> big_int {
    const bool negative{digits.starts_with('-')};
    if (negative) { digits.remove_prefix(1); }
    const auto parsed{*big_int::parse(digits, 10)};
    return negative ? -parsed : parsed;
}

} // namespace

TEST_CASE("big_int parses and prints in any base") {
    CHECK(big_int::parse("123456789123456798123456789", 10)->to_string() ==
          "123456789123456798123456789");
    CHECK(big_int::parse("ff", 16)->to_string() == "255");
    CHECK(big_int::parse("101", 2)->to_string() == "5");
    CHECK(big_int::parse("17", 8)->to_string() == "15");
    CHECK_FALSE(big_int::parse("12a", 10));
    CHECK_FALSE(big_int::parse("", 10));
    CHECK(big("-0").to_string() == "0");
    CHECK_FALSE(big("-0").is_negative());
}

TEST_CASE("big_int arithmetic is exact past 128 bits") {
    const auto max_u128{big_int{std::numeric_limits<u128>::max()}};
    const auto past{max_u128 + big_int{i128{1}}};
    CHECK(past.to_string() == "340282366920938463463374607431768211456");
    CHECK_FALSE(past.to_u128());
    CHECK((past - big_int{i128{1}}).to_u128() == std::numeric_limits<u128>::max());

    const auto square{past * past};
    CHECK(square == big_int{i128{1}}.shl(256));
    CHECK(square / past == past);
    CHECK((square + big_int{i128{7}}) % past == big_int{i128{7}});

    CHECK(big("-7") / big("2") == big("-3"));
    CHECK(big("-7") % big("2") == big("-1"));
    CHECK(big("7") % big("-2") == big("1"));
    CHECK(big("-5") + big("3") == big("-2"));
    CHECK(big("3") - big("5") == big("-2"));
}

TEST_CASE("big_int shifts floor and bitwise operators use two's complement") {
    CHECK(big("-7").shr(1) == big("-4"));
    CHECK(big("-8").shr(1) == big("-4"));
    CHECK(big("7").shr(1) == big("3"));
    CHECK(big("-1").shr(500) == big("-1"));

    CHECK((big("-1") & big("255")) == big("255"));
    CHECK((big("-256") | big("255")) == big("-1"));
    CHECK((big("12") ^ big("10")) == big("6"));
    CHECK((big("-12") ^ big("10")) == big("-2"));
    CHECK(~big("0") == big("-1"));
    CHECK(~big("-1") == big("0"));
    const auto wide{big_int{i128{1}}.shl(200)};
    CHECK((wide | big("1")) - wide == big("1"));
    CHECK((-wide & wide) == wide);
}

TEST_CASE("big_int sizes and fixed-width views") {
    CHECK(big("0").signed_bits() == 1);
    CHECK(big("-1").signed_bits() == 1);
    CHECK(big("127").signed_bits() == 8);
    CHECK(big("-128").signed_bits() == 8);
    CHECK(big("-129").signed_bits() == 9);
    CHECK(big("255").unsigned_bits() == 8);
    CHECK_FALSE(big("-1").unsigned_bits());
    CHECK(big("255").fits(8, false));
    CHECK_FALSE(big("255").fits(8, true));
    CHECK(big("123456789123456798123456789").unsigned_bits() == 87);

    CHECK(big("-1").to_words(70) == std::vector<u64>{~u64{0}, 0x3F});
    CHECK(big_int::from_words({~u64{0}, 0x3F}, 70, true) == big("-1"));
    CHECK(big_int::from_words({~u64{0}, 0x3F}, 70, false) == big_int{i128{1}}.shl(70) - big("1"));
    CHECK(big("-2").to_i128() == i128{-2});
    CHECK_FALSE(big_int{i128{1}}.shl(127).to_i128());
    CHECK(big_int{std::numeric_limits<i128>::min()}.to_i128() == std::numeric_limits<i128>::min());
}

} // namespace ghoti::tests
