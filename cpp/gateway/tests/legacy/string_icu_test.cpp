// Links: tests/legacy/string_icu_test.cpp
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"

namespace rt = uos::rt;
using namespace std::string_view_literals;

TEST("root casing preserves explicit UTF-16 boundaries") {
  const auto text = rt::fromUtf16(u"Iİıi\0AΣ\xD800"sv);
  EXPECT_EQ(rt::toLowerCase(text), rt::fromUtf16(u"ii\u0307ıi\0aς\xD800"sv));
  EXPECT_EQ(rt::toUpperCase(text), rt::fromUtf16(u"IİII\0AΣ\xD800"sv));
  EXPECT_EQ(rt::toUpperCase("\xE2\x82" "a"sv), "\xEF\xBF\xBD" "A"sv);
}

TEST("expanding uppercase mapping handles long inputs") {
  EXPECT_EQ(rt::toUpperCase(rt::repeat("ß", 40)), std::string(80, 'S'));
}

TEST("root collation handles Hangul, lone surrogates and embedded NUL") {
  EXPECT_EQ(rt::localeCompare("가", "\u1100\u1161"), 0);
  EXPECT_EQ(rt::localeCompare(rt::fromUtf16(u"\xD800"), rt::fromUtf16(u"\xD801")), -1);
  EXPECT_EQ(rt::localeCompare("a\0b"sv, "a\0c"sv), -1);
}
