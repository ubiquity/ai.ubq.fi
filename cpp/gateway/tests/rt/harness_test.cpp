// The test harness (uos/rt/test.hpp). EXPECT_THROWS_MATCHING is node's
// `assert.throws(fn, /pattern/)`, which runs the regex on String(thrown); every pass and fail
// verdict below was recorded with bun 1.4 running node:assert on the same values. The checks
// that must fail run inside failuresOf: their report line is printed, then the count restored.
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace rt = uos::rt;
namespace json = uos::json;

namespace {
  // The number of failures one check reported; they are not counted against this test.
  template <class F> int failuresOf(F check) {
    const int before = rt::test::failures();
    check();
    const int added = rt::test::failures() - before;
    rt::test::failures() = before;
    return added;
  }

  rt::JsError named(std::string name, std::string message) { return rt::JsError(std::move(message), std::move(name)); }
}

TEST("EXPECT_THROWS_MATCHING passes when the pattern matches String(thrown), as assert.throws does") {
  EXPECT_THROWS_MATCHING(throw rt::TypeError("bad thing"), "^TypeError: bad");
  EXPECT_THROWS_MATCHING(rt::rethrow(rt::thrown(json::value("plain value"))), "plain");
  EXPECT_THROWS_MATCHING(rt::rethrow(rt::thrown(json::value(42))), "^42$");
  EXPECT_THROWS_MATCHING(rt::rethrow(rt::thrown(json::value(json::object{{"a", 1}}))), "object Object");
  EXPECT_THROWS_MATCHING(rt::rethrow(nullptr), "^undefined$");
  EXPECT_THROWS_MATCHING(throw rt::RangeError(""), "^RangeError$");
  EXPECT_THROWS_MATCHING(throw named("", "msg"), "^msg$");
  EXPECT_THROWS_MATCHING(throw named("Custom", "msg"), "^Custom: msg$");
  EXPECT_THROWS_MATCHING(throw rt::JsError("ABC"), rt::Regex("abc", "i"));
  EXPECT_THROWS_MATCHING(throw rt::JsError("line1\nline2"), rt::Regex("^line2$", "m"));
  const std::string pattern = "^Error: line1";
  EXPECT_THROWS_MATCHING(throw rt::JsError("line1\nline2"), pattern);
  EXPECT_THROWS_MATCHING(throw rt::JsError("x"), std::string_view("^Error: x$"));
}

TEST("EXPECT_THROWS_MATCHING reports one failure when nothing throws or String(thrown) does not match") {
  EXPECT_EQ(failuresOf([] { EXPECT_THROWS_MATCHING(throw rt::TypeError("bad thing"), "^bad"); }), 1);
  EXPECT_EQ(failuresOf([] { EXPECT_THROWS_MATCHING((void)0, "x"); }), 1);
  EXPECT_EQ(failuresOf([] { EXPECT_THROWS_MATCHING(throw rt::JsError("a\nb"), "a.b"); }), 1);
  // A g regex starts at its lastIndex, as RegExp.prototype.exec does: "Error: x" has no
  // "Error" at or after index 3.
  rt::Regex global("Error", "g");
  global.lastIndex = 3;
  EXPECT_EQ(failuresOf([&] { EXPECT_THROWS_MATCHING(throw rt::JsError("x"), global); }), 1);
}

TEST("an ASSERT failing inside EXPECT_THROWS_MATCHING still ends the test") {
  bool propagated = false;
  EXPECT_EQ(failuresOf([&] {
              try {
                EXPECT_THROWS_MATCHING([] { ASSERT(false); }(), "x");
              } catch (const rt::test::Failure&) {
                propagated = true;
              }
            }),
            1);
  EXPECT(propagated);
}
