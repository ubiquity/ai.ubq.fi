#pragma once
// Port of src/harmony/probes.ts. Only records and scenario results are serializable.
#include "uos/harmony/adapter.hpp"
#include "uos/harmony/classifier.hpp"
#include "uos/harmony/conversation.hpp"
#include "uos/rt/date.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace uos::harmony::probes {
  using ProbeGroup = std::string;
  using ProbeOutcome = std::string;
  struct ProbeRequestSummary {
    struct Tool { std::string name; std::optional<bool> strict; friend bool operator==(const Tool&, const Tool&) = default; };
    std::string style;
    std::string model;
    std::vector<std::string> roles;
    std::optional<std::vector<Tool>> tools;
    std::vector<bool> toolStrictnessValues;
    std::optional<std::string> reasoningEffortTopLevel;
    bool reasoningInSystem;
    std::string responseFormat;
    std::optional<bool> parallelToolCalls;
    std::optional<double> maxCompletionTokens;
    bool analysisInWire;
    double assistantToolTurns;
    double toolResultTurns;
  };
  struct ProbeResponseSummary {
    struct ToolCall { std::string id; std::string name; double argumentsChars; bool argumentsJsonValid; friend bool operator==(const ToolCall&, const ToolCall&) = default; };
    double status;
    std::string model;
    bool contentPresent;
    double contentChars;
    std::optional<std::string> contentPreview;
    bool reasoningPresent;
    double reasoningChars;
    std::vector<ToolCall> toolCalls;
    bool refusal;
    std::optional<std::string> finishReason;
  };
  struct ProbeStateSnapshot {
    double analysisLines;
    double analysisAfterDrop;
    double pendingToolCalls;
    bool completedFinal;
    friend bool operator==(const ProbeStateSnapshot&, const ProbeStateSnapshot&) = default;
  };
  struct ProbeTurnRecord {
    struct AdapterError { std::string code; std::string message; friend bool operator==(const AdapterError&, const AdapterError&) = default; };
    ProbeOutcome outcome;
    std::optional<double> status;
    double durationMs;
    std::optional<ProbeRequestSummary> request;
    std::optional<ProbeResponseSummary> response;
    std::optional<adapter::UpstreamError> upstreamError;
    std::optional<AdapterError> adapterError;
    std::optional<ProbeStateSnapshot> state;
    std::vector<std::string> notes;
    std::optional<classifier::BootstrapClassifierVerdict> verdict;
  };
  struct ProbeTurnRun {
    ProbeTurnRecord record;
    std::optional<types::NormalizedAssistantResponse> normalized;
    std::optional<classifier::BootstrapClassifierVerdict> verdict;
  };
  struct ProbeScenarioResult {
    std::string id;
    ProbeGroup group;
    std::string style;
    std::string description;
    std::optional<ProbeOutcome> expectedOutcome;
    ProbeOutcome outcome;
    std::string startedAt;
    double durationMs;
    std::vector<ProbeTurnRecord> turns;
    std::optional<classifier::BootstrapClassifierVerdict> verdict;
    std::optional<std::string> failure;
    std::vector<std::string> notes;
  };

  inline const types::ToolDefinition WEATHER_TOOL{
    .name = "get_weather", .description = "Gets the current weather in the provided location.",
    .parameters = {{"type", "object"}, {"properties", json::object{{"location", json::object{{"type", "string"}, {"description", "The city and state, e.g. San Francisco, CA"}}}}},
      {"required", json::array{"location"}}, {"additionalProperties", false}},
  };
  inline const types::ToolDefinition NOTE_TOOL{
    .name = "save_note", .description = "Saves a short note to the conversation log.",
    .parameters = {{"type", "object"}, {"properties", json::object{{"text", json::object{{"type", "string"}, {"description", "Note text"}}}}},
      {"required", json::array{"text"}}, {"additionalProperties", false}},
  };
  inline const json::object ANSWER_SCHEMA{
    {"type", "object"}, {"properties", json::object{{"answer", json::object{{"type", "integer"}, {"description", "The computed answer"}}}}},
    {"required", json::array{"answer"}}, {"additionalProperties", false},
  };
  conversation::Conversation user(std::string content);
  bool argsJsonValid(const std::string& argumentsText);

  // Omit<HarmonyRequestOptions, "turns">.
  struct ProbeTurnOptions {
    types::HarmonyCallStyle style;
    std::optional<types::HarmonyReasoningEffort> reasoningEffort{};
    std::optional<std::vector<types::ToolDefinition>> tools{};
    std::optional<types::ToolStrictnessMode> toolStrictnessMode{};
    std::optional<types::ResponseFormatParam> responseFormat{};
    std::optional<types::NativeResponseFormat> nativeResponseFormat{};
    std::optional<std::string> nativeToolResultStyle{};
    std::optional<types::CombinationPolicy> combinationPolicy{};
    std::optional<double> maxCompletionTokens{};
    std::optional<bool> parallelToolCalls{};
    std::optional<std::string> currentDate{};
    std::optional<std::string> instructions{};
    std::optional<std::string> namespace_{};
  };
  struct ProbeContext {
    struct RunRaw {
      std::function<rt::Task<ProbeTurnRun>(json::object, std::optional<conversation::Conversation>)> function;
      rt::Task<ProbeTurnRun> operator()(json::object body) const { return function(std::move(body), std::nullopt); }
      rt::Task<ProbeTurnRun> operator()(json::object body, conversation::Conversation conversation) const { return function(std::move(body), std::move(conversation)); }
    };
    adapter::HarmonyTransport transport;
    std::function<rt::Date()> now;
    std::function<rt::Task<ProbeTurnRun>(conversation::Conversation, ProbeTurnOptions)> runTurn;
    RunRaw runRaw;
    std::function<rt::Task<ProbeTurnRun>(classifier::BootstrapClassifierRequestOptions)> runClassifier;
  };
  ProbeContext createProbeContext(adapter::HarmonyTransport transport);
  ProbeContext createProbeContext(adapter::HarmonyTransport transport, std::function<rt::Date()> now);

  // Explicit sanitized serialization; ProbeTurnRun deliberately has no serializer.
  json::object toJson(const ProbeTurnRecord& value);
  json::object toJson(const ProbeScenarioResult& value);
}
