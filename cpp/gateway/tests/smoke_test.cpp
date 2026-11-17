#include "uos/rt/error.hpp"
#include "uos/rt/test.hpp"

TEST("harness compares json, strings, numbers and optionals") {
  EXPECT_EQ(uos::json::parse(R"({"ok":true})"), uos::json::value({{"ok", true}}));
  EXPECT_EQ(std::string("a"), "a");
  EXPECT_EQ(1 + 1, 2);
  EXPECT_EQ(std::optional<int>(), std::nullopt);
  EXPECT_EQ(std::vector<int>({1, 2}), std::vector<int>({1, 2}));
}

TEST("JsError carries name and message") {
  EXPECT_THROWS_MESSAGE(throw uos::rt::TypeError("bad"), uos::rt::JsError, "bad");
  EXPECT_EQ(uos::rt::RangeError("x").toString(), "RangeError: x");
}
