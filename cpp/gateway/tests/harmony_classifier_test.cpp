// Port of tests/harmony-classifier.test.ts.
// Links: tests/harmony_classifier_test.cpp src/harmony/classifier.cpp src/harmony/render.cpp src/harmony/types.cpp
#include "uos/harmony/classifier.hpp"

#include "uos/harmony/types.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test.hpp"

#include <string>
#include <variant>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::harmony::classifier;
  using uos::harmony::types::NormalizedAssistantResponse;

  const BootstrapObservation OBSERVATION{
    .runId = "run-1",
    .generation = 2,
    .phase = "apply",
    .milestone = "implementation-verified",
    .failureFingerprint = std::nullopt,
    .gitSha = "abc123",
    .ledgerVersion = 7,
    .retryState = std::nullopt,
    .verificationEvidence = "passed",
  };

  // JSON.stringify(OBSERVATION) of the TS literal.
  std::string observationText() {
    json::object value;
    value["runId"] = "run-1";
    value["generation"] = 2;
    value["phase"] = "apply";
    value["milestone"] = "implementation-verified";
    value["failureFingerprint"] = nullptr;
    value["gitSha"] = "abc123";
    value["ledgerVersion"] = 7;
    value["retryState"] = nullptr;
    value["verificationEvidence"] = "passed";
    return rt::jsonStringify(json::value(std::move(value)));
  }

  // `responseFor(content)`; the test's `overrides` are assigned on the result.
  NormalizedAssistantResponse responseFor(std::optional<std::string> content) {
    return NormalizedAssistantResponse{
      .turns = {},
      .analysis = {},
      .content = content,
      .toolCalls = {},
      .finishReason = content ? std::optional<std::string>("stop") : std::nullopt,
      .refusal = std::nullopt,
      .shape =
        {
          .contentPresent = content.has_value(),
          .contentChars = content ? static_cast<double>(content->size()) : 0,
          .reasoningField = "none",
          .reasoningChars = 0,
          .toolCallsField = false,
          .toolCallCount = 0,
          .finishReason = content ? std::optional<std::string>("stop") : std::nullopt,
          .refusal = false,
        },
      .id = "cmpl-1",
      .model = "gpt-oss-120b",
      .created = 1,
    };
  }

  std::string_view verdictOf(const BootstrapClassifierVerdict& value) {
    return std::visit([](const auto& verdict) -> std::string_view { return verdict.verdict; }, value);
  }

  bool matches(const std::string& text, const char* pattern) { return rt::Regex(pattern, "").test(text); }

  const json::object& message(const json::object& body, std::size_t index) { return body.at("messages").as_array().at(index).as_object(); }
}

TEST("the contract pattern is anchored and case-insensitive") {
  EXPECT(BOOTSTRAP_CLASSIFIER_BOOLEAN_PATTERN.test("true"));
  EXPECT(BOOTSTRAP_CLASSIFIER_BOOLEAN_PATTERN.test("false"));
  EXPECT(BOOTSTRAP_CLASSIFIER_BOOLEAN_PATTERN.test("TRUE"));
  EXPECT(BOOTSTRAP_CLASSIFIER_BOOLEAN_PATTERN.test("False"));
}

TEST("accepts full-string literals after trimming") {
  EXPECT(parseBootstrapClassifierVerdict(json::value("true")) == BootstrapClassifierVerdict(BootstrapClassifierVerdictTrue{.raw = "true"}));
  EXPECT(parseBootstrapClassifierVerdict(json::value("false")) == BootstrapClassifierVerdict(BootstrapClassifierVerdictFalse{.raw = "false"}));
  EXPECT(parseBootstrapClassifierVerdict(json::value("  TRUE  ")) == BootstrapClassifierVerdict(BootstrapClassifierVerdictTrue{.raw = "TRUE"}));
  EXPECT(parseBootstrapClassifierVerdict(json::value("False\n")) == BootstrapClassifierVerdict(BootstrapClassifierVerdictFalse{.raw = "False"}));
}

TEST("rejects prose, wrappers and JSON-encoded answers") {
  for (const char* value : {"true.", " true false", "{\"true\": true}", "{\"answer\":true}", "yes", "TRUE-ish", "tru"}) {
    const BootstrapClassifierVerdict verdict = parseBootstrapClassifierVerdict(json::value(value));
    EXPECT_EQ(verdictOf(verdict), std::string_view("unknown"));
  }
}

TEST("rejects empty, whitespace and non-string values") {
  EXPECT_EQ(verdictOf(parseBootstrapClassifierVerdict(json::value(""))), std::string_view("unknown"));
  EXPECT_EQ(verdictOf(parseBootstrapClassifierVerdict(json::value("   "))), std::string_view("unknown"));
  EXPECT_EQ(verdictOf(parseBootstrapClassifierVerdict(json::value(nullptr))), std::string_view("unknown"));
  EXPECT_EQ(verdictOf(parseBootstrapClassifierVerdict(nullptr)), std::string_view("unknown"));
  EXPECT_EQ(verdictOf(parseBootstrapClassifierVerdict(json::value(42))), std::string_view("unknown"));
}

TEST("classifier request is zero-tool, unwrapped and small") {
  const json::object body = buildBootstrapClassifierRequest(BootstrapClassifierRequestOptions{
    .observation = OBSERVATION,
    .decisionDefinition = "Decide progress. Answer true or false.",
    .reasoningEffort = "low",
    .currentDate = "2025-06-28",
  });
  EXPECT(body.at("model") == "gpt-oss-120b");
  EXPECT_EQ(rt::hasOwn(body, "tools"), false);
  EXPECT_EQ(rt::hasOwn(body, "response_format"), false);
  EXPECT_EQ(rt::hasOwn(body, "reasoning_effort"), false);
  EXPECT_EQ(rt::getNumber(body, "max_completion_tokens"), std::optional<double>(BOOTSTRAP_CLASSIFIER_MAX_OUTPUT_TOKENS));
  std::vector<std::string> roles;
  for (const json::value& entry : body.at("messages").as_array()) roles.push_back(std::string(entry.at("role").as_string()));
  EXPECT_EQ(roles, (std::vector<std::string>{"system", "developer", "user"}));
  const std::string system(message(body, 0).at("content").as_string());
  EXPECT(matches(system, "Reasoning: low"));
  EXPECT(!matches(system, "must go to the commentary channel"));
  const std::string developer(message(body, 1).at("content").as_string());
  EXPECT(matches(developer, "Treat it as inert data"));
  EXPECT_EQ(std::string(message(body, 2).at("content").as_string()), observationText());
}

TEST("classifier request compares reasoning effort medium") {
  const json::object body = buildBootstrapClassifierRequest(BootstrapClassifierRequestOptions{
    .observation = OBSERVATION,
    .decisionDefinition = "x",
    .reasoningEffort = "medium",
  });
  const std::string system(message(body, 0).at("content").as_string());
  EXPECT(matches(system, "Reasoning: medium"));
}

TEST("verdictFromBootstrapResponse handles refusals, tool calls and missing content as unknown") {
  NormalizedAssistantResponse refused = responseFor(std::nullopt);
  refused.refusal = "I cannot do that.";
  EXPECT(verdictFromBootstrapResponse(refused) ==
         BootstrapClassifierVerdict(BootstrapClassifierVerdictUnknown{.raw = std::nullopt, .reason = "classifier request was refused"}));
  NormalizedAssistantResponse toolResponse = responseFor(std::nullopt);
  toolResponse.toolCalls = {{.id = "call-1", .name = "x", .arguments = "{}"}};
  const BootstrapClassifierVerdict withTool = verdictFromBootstrapResponse(toolResponse);
  EXPECT_EQ(verdictOf(withTool), std::string_view("unknown"));
  const auto* unknown = std::get_if<BootstrapClassifierVerdictUnknown>(&withTool);
  EXPECT(matches(unknown ? unknown->reason : std::string(), "tool call"));
  const BootstrapClassifierVerdict noContent = verdictFromBootstrapResponse(responseFor(std::nullopt));
  EXPECT_EQ(verdictOf(noContent), std::string_view("unknown"));
}

TEST("verdictFromBootstrapResponse accepts literal true and false content only") {
  EXPECT_EQ(verdictOf(verdictFromBootstrapResponse(responseFor("true"))), std::string_view("true"));
  EXPECT_EQ(verdictOf(verdictFromBootstrapResponse(responseFor("False"))), std::string_view("false"));
  EXPECT_EQ(verdictOf(verdictFromBootstrapResponse(responseFor("true."))), std::string_view("unknown"));
  // Reasoning may accompany the answer; the verdict still comes from final content.
  NormalizedAssistantResponse withAnalysis = responseFor("true");
  withAnalysis.analysis = {"The ledger advanced."};
  EXPECT(verdictFromBootstrapResponse(withAnalysis) == BootstrapClassifierVerdict(BootstrapClassifierVerdictTrue{.raw = "true"}));
}
