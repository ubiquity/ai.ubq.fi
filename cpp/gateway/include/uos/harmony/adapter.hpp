#pragma once
// Port of src/harmony/adapter.ts. Transport stays injected and all reasoning stays out of replay.
#include "uos/harmony/types.hpp"
#include "uos/provider/cerebras.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace uos::harmony::adapter {
  struct HarmonyRequestOptions {
    types::HarmonyCallStyle style;
    std::vector<types::ConversationTurn> turns;
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

  struct BuiltHarmonyRequest {
    struct Metadata {
      struct ToolEntry {
        std::string name;
        bool strict;
        friend bool operator==(const ToolEntry&, const ToolEntry&) = default;
      };
      std::string model;
      std::vector<std::string> messageRoles;
      std::string toolsRendered;
      std::vector<ToolEntry> toolEntries;
      std::vector<bool> toolStrictnessValues;
      std::optional<types::HarmonyReasoningEffort> reasoningEffortTopLevel;
      bool reasoningEffortInSystem;
      std::string responseFormat;
      std::optional<bool> parallelToolCalls;
      std::optional<double> maxCompletionTokens;
      bool analysisInWire;
      double assistantToolTurns;
      double toolResultTurns;
    };
    types::HarmonyCallStyle style;
    json::object body;
    Metadata metadata;
  };

  std::vector<types::ToolDefinition> normalizeToolStrictness(const std::vector<types::ToolDefinition>& tools, bool strictness);
  BuiltHarmonyRequest buildCerebrasHarmonyRequest(const HarmonyRequestOptions& options);

  struct NormalizedChoice {
    std::optional<std::string> content;
    std::optional<std::string> refusal;
    std::optional<std::string> reasoning;
    std::vector<types::ToolCall> toolCalls;
    std::optional<std::string> finishReason;
    std::string reasoningField;
  };
  std::vector<types::HarmonyTurn> harmonyTurnsFromChoice(const NormalizedChoice& choice);
  struct HarmonyNormalizationError {
    std::string error;
    friend bool operator==(const HarmonyNormalizationError&, const HarmonyNormalizationError&) = default;
  };
  using NormalizeHarmonyChatCompletionResult = std::variant<types::NormalizedAssistantResponse, HarmonyNormalizationError>;
  NormalizeHarmonyChatCompletionResult normalizeHarmonyChatCompletion(const json::value* value, const std::string& requestedModel);
  NormalizeHarmonyChatCompletionResult normalizeHarmonyChatCompletion(const json::value& value, const std::string& requestedModel);
  NormalizeHarmonyChatCompletionResult normalizeHarmonyChatCompletion(const json::value* value);
  NormalizeHarmonyChatCompletionResult normalizeHarmonyChatCompletion(const json::value& value);

  struct HarmonyTransportRequestOptions { std::optional<rt::AbortSignal> signal{}; };
  // A JS function value: copies preserve identity, and a one-argument fake may ignore options.
  class HarmonyTransport {
  public:
    using Function = std::function<rt::Task<rt::Response>(json::object, std::optional<HarmonyTransportRequestOptions>)>;
    HarmonyTransport() = default;
    template <class F> requires(!std::is_same_v<std::decay_t<F>, HarmonyTransport>)
    HarmonyTransport(F function) : function_(std::make_shared<const Function>(wrap(std::move(function)))) {}
    rt::Task<rt::Response> operator()(json::object body) const;
    rt::Task<rt::Response> operator()(json::object body, HarmonyTransportRequestOptions options) const;
    explicit operator bool() const noexcept { return function_ != nullptr; }
    friend bool operator==(const HarmonyTransport&, const HarmonyTransport&) = default;
  private:
    template <class F> static Function wrap(F function) {
      auto held = std::make_shared<F>(std::move(function));
      return [held](json::object body, std::optional<HarmonyTransportRequestOptions> options) -> rt::Task<rt::Response> {
        if constexpr (std::is_invocable_v<F&, json::object, std::optional<HarmonyTransportRequestOptions>>) {
          return (*held)(std::move(body), std::move(options));
        } else {
          return (*held)(std::move(body));
        }
      };
    }
    static rt::Task<rt::Response> keepAlive(std::shared_ptr<const Function> function, rt::Task<rt::Response> task);
    std::shared_ptr<const Function> function_;
  };
  struct HarmonyTransportOptions {
    rt::Nullable<std::string> apiKey{};
    provider::cerebras::CerebrasFetch fetcher{};
  };
  HarmonyTransport createCerebrasTransport();
  HarmonyTransport createCerebrasTransport(const HarmonyTransportOptions& options);

  struct UpstreamError {
    std::optional<std::string> code;
    std::optional<std::string> message;
    friend bool operator==(const UpstreamError&, const UpstreamError&) = default;
  };
  struct RunTurnResultOk {
    static constexpr bool ok = true;
    double status;
    types::NormalizedAssistantResponse normalized;
  };
  struct RunTurnResultNotOk {
    static constexpr bool ok = false;
    double status;
    std::optional<UpstreamError> upstreamError;
    std::optional<std::string> normalizationError;
  };
  using RunTurnResult = std::variant<RunTurnResultOk, RunTurnResultNotOk>;
  rt::Task<RunTurnResult> runHarmonyTurn(HarmonyRequestOptions options, HarmonyTransport transport);
}
