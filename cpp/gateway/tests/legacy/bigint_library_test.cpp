#include "uos/rt/error.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test.hpp"

#include <cmath>
#include <limits>
#include <utility>

using uos::rt::BigInt;

TEST("BigInt requires digits in signed and prefixed literals") {
  for (const std::string_view invalid : {"-", "+", "0x ", "0b\t", "0o\n", "-0x1"}) {
    EXPECT(!BigInt::parse(invalid).has_value());
    EXPECT_THROWS(BigInt::fromString(invalid), uos::rt::SyntaxError);
  }
  EXPECT(!BigInt::parse(std::string_view("17\0" "99", 5)).has_value());
  EXPECT_EQ(BigInt::fromString("\xc2\xa0+00018\xe3\x80\x80").toString(), "18");
  EXPECT_EQ(BigInt::fromString("0B101").toString(), "5");
  EXPECT_EQ(BigInt::fromString("0O17").toString(), "15");
}

TEST("BigInt bounds growth and handles the full fixed-width range") {
  constexpr auto limit = BigInt::maxLengthBits;
  const BigInt one(1), minusOne(-1);
  const BigInt upper = one << BigInt::fromUint64(limit - 1);
  const BigInt max = upper | (upper - one);
  EXPECT(BigInt::asIntN(limit, upper) == -upper);
  EXPECT(BigInt::asIntN(limit + 1, upper) == upper);
  EXPECT(BigInt::asIntN(limit, max) == minusOne);
  EXPECT(BigInt::asUintN(limit, minusOne) == max);
  EXPECT_THROWS(BigInt::asUintN(limit + 1, minusOne), uos::rt::RangeError);
  EXPECT_THROWS(one << BigInt::fromUint64(limit), uos::rt::RangeError);
  EXPECT_THROWS(upper * BigInt(2), uos::rt::RangeError);
  EXPECT_THROWS(BigInt(3).pow(BigInt::fromUint64(limit)), uos::rt::RangeError);
  EXPECT((minusOne >> (one << BigInt(80))) == minusOne);
  const std::string octalTail(static_cast<std::size_t>(limit / 3), '0');
  EXPECT(BigInt::fromString("0o1" + octalTail) == upper);
  EXPECT_THROWS(BigInt::fromString("0o2" + octalTail), uos::rt::RangeError);
}

TEST("BigInt converts binary64 ties and the overflow boundary") {
  constexpr auto largest = std::numeric_limits<double>::max();
  const BigInt one(1);
  const BigInt midpoint = (one << BigInt(1024)) - (one << BigInt(970));
  EXPECT_EQ((midpoint - one).toNumber(), largest);
  EXPECT(std::isinf(midpoint.toNumber()));
  EXPECT_EQ((one - midpoint).toNumber(), -largest);
  EXPECT(BigInt::fromNumber(largest) == (one << BigInt(1024)) - (one << BigInt(971)));
  EXPECT_EQ(BigInt::fromUint64(9007199254740993ULL).toNumber(), 9007199254740992.0);
  EXPECT_EQ(BigInt::fromUint64(9007199254740995ULL).toNumber(), 9007199254740996.0);
  EXPECT(!BigInt::fromNumber(-0.0).isNegative());
}

TEST("BigInt copies and moves its wide integer storage") {
  const BigInt expected = -(BigInt(1) << BigInt(512));
  BigInt copy = expected;
  BigInt moved = std::move(copy);
  EXPECT(moved == expected);
  EXPECT(copy.isZero());

  copy = moved;
  moved += BigInt(1);
  EXPECT(copy == expected);
  EXPECT(moved != expected);

  moved = std::move(copy);
  EXPECT(moved == expected);
  EXPECT(copy.isZero());
  EXPECT_EQ(copy.toNumber(), 0.0);
}
