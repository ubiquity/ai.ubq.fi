// Differential cases for src/harmony/tools/result.ts (its bun test, tests/harmony-tools-router.test.ts,
// needs src/harmony/tools/{router,fakes}.ts): the expected values were recorded from Bun 1.4.0
// running the TypeScript module on the same inputs.
#include "uos/harmony/tools/result.hpp"

#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"

namespace {
  using namespace uos::harmony::tools::result;
  namespace rt = uos::rt;
}

TEST("clipToolText clips at a UTF-16 limit with the truncation suffix") {
  EXPECT_EQ(clipToolText("abc"), "abc");
  EXPECT_EQ(clipToolText("abcd", 4), "abcd");
  EXPECT_EQ(clipToolText("abcdef", 4), "abcd…[truncated]");
  EXPECT_EQ(clipToolText("abc", 0), "…[truncated]");
  EXPECT_EQ(clipToolText("abcdef", 2.7), "ab…[truncated]");
  EXPECT_EQ(clipToolText("abcdef", -1), "abcde…[truncated]");
  EXPECT_EQ(clipToolText("abcdef", rt::Number::NaN), "abcdef");
  // "a😀b" clipped at 2 keeps the lone high surrogate U+D83D.
  EXPECT_EQ(clipToolText("a\xF0\x9F\x98\x80" "b", 2), "a\xED\xA0\xBD…[truncated]");
  EXPECT_EQ(rt::length(clipToolText(rt::repeat("x", 8'001))), 8'012);
  EXPECT_EQ(rt::length(clipToolText(rt::repeat("x", 8'000))), 8'000);
}

TEST("toolFailure builds { ok, error, error_code } only") {
  const ToolResult failure = toolFailure("not_found", "missing");
  EXPECT(failure == (ToolResult{.ok = false, .error = "missing", .error_code = "not_found"}));
  EXPECT(!failure.output && !failure.exit_code && !failure.stdout && !failure.stderr);
}

TEST("ToolExecutionError is an Error named ToolExecutionError carrying its code") {
  const ToolExecutionError error("write_scope", "nope");
  EXPECT_EQ(error.name, "ToolExecutionError");
  EXPECT_EQ(error.message, "nope");
  EXPECT_EQ(error.code, "write_scope");
  EXPECT_EQ(error.toString(), "ToolExecutionError: nope");
  EXPECT_THROWS_MESSAGE(throw ToolExecutionError("internal", "boom"), rt::JsError, "boom");
}

TEST("the exported limits") {
  EXPECT_EQ(TOOL_OUTPUT_LIMIT, 8'000);
  EXPECT_EQ(SEARCH_LINE_LIMIT, 200);
  EXPECT_EQ(SHELL_DEFAULT_TIMEOUT_MS, 20'000);
}
