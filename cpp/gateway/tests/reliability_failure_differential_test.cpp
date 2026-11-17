// Differential cases for src/harmony/reliability/failure.ts (no direct bun tests): every abort
// rule with the detail renderers' number formatting (NaN, Infinity, -0, 1e21, fractions), the
// thresholds of the later rules, and generated inputs that exercise the precedence order. Expected
// values were recorded from the TypeScript module under Bun 1.4.0
// (tests/reliability_failure_differential_vectors.inc).
#include "uos/harmony/reliability/failure.hpp"

#include "uos/harmony/reliability/state.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test.hpp"

#include <cstdio>
#include <string>

#include "reliability_failure_differential_vectors.inc"

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::harmony::reliability::failure;
  namespace state = uos::harmony::reliability::state;

  double number(const json::value& value) {
    if (value.is_string()) return rt::stringToNumber(std::string(value.as_string()));
    return *rt::getNumber(value);
  }

  ClassificationInput inputOf(const json::object& encoded) {
    state::StructuredTaskState taskState = state::emptyTaskState();
    const json::object& overrides = encoded.at("state").as_object();
    if (const json::value* value = rt::get(overrides, "semanticLoopStreak")) taskState.semanticLoopStreak = number(*value);
    if (const json::value* value = rt::get(overrides, "finalAttempts")) taskState.finalAttempts = number(*value);
    if (const json::value* value = rt::get(overrides, "duplicateCalls")) taskState.duplicateCalls = number(*value);
    if (const json::value* value = rt::get(overrides, "invalidCalls")) taskState.invalidCalls = number(*value);
    if (const json::array* finals = rt::getArray(overrides, "finals")) {
      for (const json::value& item : *finals) {
        taskState.finals.push_back({.content = std::string(item.at("content").as_string()), .accepted = item.at("accepted").as_bool(), .seq = number(item.at("seq"))});
      }
    }
    if (const json::array* pending = rt::getArray(overrides, "pendingVerification")) {
      for (const json::value& item : *pending) taskState.pendingVerification.push_back({.path = std::string(item.at("path").as_string()), .seq = number(item.at("seq"))});
    }
    if (const json::array* commands = rt::getArray(overrides, "unresolvedCommands")) {
      for (const json::value& item : *commands) taskState.unresolvedCommands.push_back({.command = std::string(item.at("command").as_string()), .seq = number(item.at("seq"))});
    }
    if (const json::array* edits = rt::getArray(overrides, "unresolvedEdits")) {
      for (const json::value& item : *edits) taskState.unresolvedEdits.push_back({.path = std::string(item.at("path").as_string()), .seq = number(item.at("seq"))});
    }
    ClassificationInput input{
      .state = std::move(taskState),
      .invalidCallStreak = number(encoded.at("invalidCallStreak")),
      .loopStreak = number(encoded.at("loopStreak")),
      .guardRejections = number(encoded.at("guardRejections")),
      .finalAccepted = encoded.at("finalAccepted").as_bool(),
    };
    if (const json::value* reason = rt::get(encoded, "abortedReason")) {
      if (reason->is_null()) input.abortedReason = nullptr;
      else input.abortedReason = std::string(reason->as_string());
    }
    return input;
  }
}

TEST("differential: classifyReliability matches Bun") {
  const json::value vectors = rt::jsonParse(kFailureVectors);
  int checked = 0;
  for (const json::value& row : vectors.as_array()) {
    const json::array& vector = row.as_array();
    const ReliabilityClassification classification = classifyReliability(inputOf(vector[0].as_object()));
    const std::string got = rt::jsonStringify(json::value_from(classification));
    const std::string wanted = rt::jsonStringify(vector[1]);
    if (got != wanted) {
      EXPECT_EQ(got, wanted);
      std::fprintf(stderr, "      input: %s\n", rt::jsonStringify(vector[0]).c_str());
    }
    checked += 1;
  }
  EXPECT_EQ(checked, 354);
}

TEST("an undefined and a null abort reason both fall through to the state rules") {
  ClassificationInput input{.state = state::emptyTaskState(), .invalidCallStreak = 9, .loopStreak = 0, .guardRejections = 9, .finalAccepted = false};
  input.state.duplicateCalls = 3;
  const ReliabilityClassification undefinedReason = classifyReliability(input);
  input.abortedReason = nullptr;
  const ReliabilityClassification nullReason = classifyReliability(input);
  EXPECT(undefinedReason == nullReason);
  EXPECT_EQ(undefinedReason.failure_class, "duplicate_loop");
  EXPECT_EQ(undefinedReason.detail, "3 duplicate calls recorded");
  input.abortedReason = std::string("guard_exhausted");
  EXPECT(classifyReliability(input) == (ReliabilityClassification{.failure_class = "guard_exhausted", .detail = "final guard rejected 9 attempts"}));
}

TEST("ReliabilityClassification converts to and from JSON") {
  const ReliabilityClassification none{.failure_class = std::nullopt, .detail = std::nullopt};
  EXPECT_EQ(rt::jsonStringify(json::value_from(none)), std::string(R"({"failure_class":null,"detail":null})"));
  const ReliabilityClassification stalled{.failure_class = "stalled", .detail = "max turns reached without completion"};
  const json::value written = json::value_from(stalled);
  EXPECT_EQ(rt::jsonStringify(written), std::string(R"({"failure_class":"stalled","detail":"max turns reached without completion"})"));
  EXPECT(json::value_to<ReliabilityClassification>(written) == stalled);
  EXPECT_THROWS_MESSAGE(json::value_to<ReliabilityClassification>(rt::jsonParse(R"({"failure_class":null})")), rt::TypeError,
                        "ReliabilityClassification.detail is undefined, not string or null");
}
