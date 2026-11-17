// Port of tests/reliability-feedback.test.ts.
// "feedback: required, non-empty and undefined rules are enforced" passes `{ path: "a", pattern:
// undefined }`: a JSON object cannot hold an undefined member, so the port passes `pattern: null`
// (still a key filesystem.read does not declare, the property the assertion checks).
// Links: tests/reliability_feedback_test.cpp src/harmony/reliability/feedback.cpp src/harmony/tools/schemas.cpp
//        src/harmony/types.cpp
#include "uos/harmony/reliability/feedback.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::harmony::reliability::feedback;

  bool matches(const std::string& text, const char* pattern) { return rt::Regex(pattern, "").test(text); }

  std::vector<std::string> codesOf(const DetailedValidationResult& result) {
    std::vector<std::string> codes;
    for (const ToolArgumentIssue& issue : result.issues) codes.push_back(issue.code);
    return codes;
  }

  const ToolArgumentIssue* findIssue(const DetailedValidationResult& result, std::string_view code) {
    const auto found = std::find_if(result.issues.begin(), result.issues.end(), [code](const ToolArgumentIssue& issue) { return issue.code == code; });
    return found == result.issues.end() ? nullptr : &*found;
  }
}

TEST("feedback: a fully valid call has no issues") {
  const DetailedValidationResult result = validateToolArgumentsDetailed("filesystem.read", json::value(json::object{{"path", "docs/spec.txt"}}));
  EXPECT_EQ(result.valid, true);
  EXPECT(result.issues.empty());
  EXPECT(rt::deepEqual(json::value(result.arguments), json::object{{"path", "docs/spec.txt"}}));
}

TEST("feedback: reports every issue at once with stable codes and hints") {
  const DetailedValidationResult result = validateToolArgumentsDetailed("filesystem.read", json::value(json::object{{"path", 42}, {"file", "x"}}));
  EXPECT_EQ(result.valid, false);
  const std::vector<std::string> codes = codesOf(result);
  EXPECT(rt::includes(codes, std::string("wrong_type")));
  EXPECT(rt::includes(codes, std::string("unexpected_argument")));
  const ToolArgumentIssue* unexpected = findIssue(result, "unexpected_argument");
  ASSERT(unexpected != nullptr);
  EXPECT(matches(unexpected->hint, "allowed: path"));
  const ToolArgumentIssue* wrongType = findIssue(result, "wrong_type");
  ASSERT(wrongType != nullptr);
  EXPECT(matches(wrongType->message, "must be a string"));
}

TEST("feedback: required, non-empty and undefined rules are enforced") {
  const DetailedValidationResult missing = validateToolArgumentsDetailed("shell.exec", json::value(json::object{}));
  EXPECT_EQ(missing.valid, false);
  ASSERT(!missing.issues.empty());
  EXPECT_EQ(missing.issues[0].code, "missing_required");
  EXPECT(matches(missing.issues[0].hint, "command"));

  const DetailedValidationResult empty = validateToolArgumentsDetailed("shell.exec", json::value(json::object{{"command", ""}}));
  EXPECT_EQ(empty.valid, false);
  ASSERT(!empty.issues.empty());
  EXPECT_EQ(empty.issues[0].code, "non_empty");

  const DetailedValidationResult undefinedArg = validateToolArgumentsDetailed("filesystem.read", json::value(json::object{{"path", "a"}, {"pattern", nullptr}}));
  EXPECT_EQ(undefinedArg.valid, false);
  EXPECT(rt::includes(codesOf(undefinedArg), std::string("unexpected_argument")));
}

TEST("feedback: plan arrays validate item by item") {
  const DetailedValidationResult bad = validateToolArgumentsDetailed("task.update_plan", json::value(json::object{{"plan", json::array{"ok", ""}}}));
  EXPECT_EQ(bad.valid, false);
  ASSERT(!bad.issues.empty());
  EXPECT_EQ(bad.issues[0].code, "bad_array_item");
  EXPECT(matches(bad.issues[0].location, "plan\\[1\\]"));
  const DetailedValidationResult good = validateToolArgumentsDetailed("task.update_plan", json::value(json::object{{"plan", json::array{"a", "b"}}}));
  EXPECT_EQ(good.valid, true);
}

TEST("feedback: unknown tools produce one deterministic issue") {
  const DetailedValidationResult result = validateToolArgumentsDetailed("filesystem.write", json::value(json::object{{"path", "x"}}));
  EXPECT_EQ(result.valid, false);
  ASSERT_EQ(result.issues.size(), std::size_t{1});
  EXPECT_EQ(result.issues[0].code, "unknown_tool");
  EXPECT(matches(result.issues[0].hint, "filesystem\\.read"));
}

TEST("feedback: non-object arguments are rejected deterministically") {
  const DetailedValidationResult result = validateToolArgumentsDetailed("filesystem.read", json::value("not-an-object"));
  EXPECT_EQ(result.valid, false);
  ASSERT(!result.issues.empty());
  EXPECT_EQ(result.issues[0].code, "not_an_object");
}

TEST("feedback: rendered feedback is one deterministic line with every issue") {
  const DetailedValidationResult result = validateToolArgumentsDetailed("filesystem.read", json::value(json::object{{"path", 42}, {"file", "x"}}));
  const std::string rendered = renderValidationFeedback("filesystem.read", result);
  EXPECT(matches(rendered, "^invalid arguments \\(2 issue\\(s\\)\\)"));
  EXPECT(matches(rendered, "#1: "));
  EXPECT(matches(rendered, "#2: "));
  const std::string again = renderValidationFeedback("filesystem.read", result);
  EXPECT_EQ(rendered, again);
}

TEST("feedback: invalidCallLabel is a stable short code") {
  const DetailedValidationResult result = validateToolArgumentsDetailed("shell.exec", json::value(json::object{{"command", ""}}));
  const std::string label = invalidCallLabel(result);
  EXPECT(matches(label, "^non_empty:"));
}
