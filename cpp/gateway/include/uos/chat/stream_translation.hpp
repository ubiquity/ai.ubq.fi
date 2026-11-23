#pragma once
// Port of src/chat/stream-translation.ts.
#include "uos/openai_telemetry.hpp"
#include "uos/responses_failover_stream.hpp"
#include "uos/responses_stream.hpp"
#include "uos/rt/ordered.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uos::chat::stream_translation {
  void recordResponsesTerminal(const responses_stream::ResponsesStreamEvent& event, const openai_telemetry::UsageContext* usageContext = nullptr);
  std::string reconcileCompletedOutputText(const std::string& emittedText, const std::string& completedText);
  std::string reconcileCompletedRefusal(const std::string& emittedRefusal, const std::string& completedRefusal);
  std::string chatOutputTextPartKey(const json::object& event);
  std::string chatOutputTextPartStorageKey(const json::object& event);
  void setChatOutputTextPart(rt::OrderedMap<std::string, std::string>& parts, const json::object& event, const std::string& value);

  struct ReconciledChatContent {
    std::string outputText;
    std::string refusal;
    friend bool operator==(const ReconciledChatContent&, const ReconciledChatContent&) = default;
  };
  ReconciledChatContent reconcileChatContentPart(rt::OrderedMap<std::string, std::string>& outputTextParts,
                                               rt::OrderedMap<std::string, std::string>& refusalParts, const json::object& event, const json::value* part);
  ReconciledChatContent reconcileChatContentPart(rt::OrderedMap<std::string, std::string>& outputTextParts,
                                               rt::OrderedMap<std::string, std::string>& refusalParts, const json::object& event, const json::value& part);
  ReconciledChatContent reconcileChatOutputItemContent(rt::OrderedMap<std::string, std::string>& outputTextParts,
                                                     rt::OrderedMap<std::string, std::string>& refusalParts, const json::object& event, const json::value* item);
  ReconciledChatContent reconcileChatOutputItemContent(rt::OrderedMap<std::string, std::string>& outputTextParts,
                                                     rt::OrderedMap<std::string, std::string>& refusalParts, const json::object& event, const json::value& item);
  ReconciledChatContent reconcileChatResponseOutputContent(rt::OrderedMap<std::string, std::string>& outputTextParts,
                                                         rt::OrderedMap<std::string, std::string>& refusalParts, const json::object& event, const json::value* output);
  ReconciledChatContent reconcileChatResponseOutputContent(rt::OrderedMap<std::string, std::string>& outputTextParts,
                                                         rt::OrderedMap<std::string, std::string>& refusalParts, const json::object& event, const json::value& output);
  json::object withAccumulatedResponseText(const json::object& response, const std::string& text, double ignoredOutputPrefix = 0);
  json::object withAccumulatedResponseRefusal(const json::object& response, const std::string& refusal, double ignoredOutputPrefix = 0);
  json::object withAccumulatedResponseItems(const json::object& response, const std::vector<json::object>& accumulated);

  struct ChatFunctionCall {
    std::string key;
    double index;
    std::string callId;
    std::string name;
    std::string arguments;
    bool argumentsDone;
  };
  [[noreturn]] void malformedFunctionCallStream(std::string message);

  class ChatFunctionCallAccumulator {
  public:
    struct Added { std::shared_ptr<ChatFunctionCall> call; bool includeIdentity; std::string suffix; };
    struct Delta { std::shared_ptr<ChatFunctionCall> call; std::string delta; };
    struct Reconciled { std::shared_ptr<ChatFunctionCall> call; std::string suffix; };
    bool hasCalls() const;
    const std::vector<std::shared_ptr<ChatFunctionCall>>& calls() const;
    void assertFinalized() const;
    bool has(const json::object& event, const json::object* item = nullptr) const;
    std::optional<Added> add(const json::object& event, const json::value* item);
    std::optional<Added> add(const json::object& event, const json::value& item);
    Delta delta(const json::object& event);
    Reconciled done(const json::object& event);
    std::optional<Reconciled> reconcileItem(const json::object& event, const json::value* item);
    std::optional<Reconciled> reconcileItem(const json::object& event, const json::value& item);
    std::vector<Reconciled> reconcileOutput(const json::object& event, const json::value* output);
    std::vector<Reconciled> reconcileOutput(const json::object& event, const json::value& output);
  private:
    static std::optional<std::string> key(const json::object& event, const json::object* item = nullptr);
    std::shared_ptr<ChatFunctionCall> create(const json::object& event, const json::object& item);
    static std::string reconcileArguments(ChatFunctionCall& call, const std::string& finalArguments);
    std::vector<std::shared_ptr<ChatFunctionCall>> calls_;
    rt::OrderedMap<std::string, std::shared_ptr<ChatFunctionCall>> byKey_;
  };

  bool preparedChatCompletionIsEmpty(const responses_failover_stream::PreparedResponsesStream& prepared);
  struct ChatToolCallDeltaOptions { bool includeIdentity = false; std::optional<std::string> argumentsDelta{}; };
  json::object chatToolCallDelta(const ChatFunctionCall& call);
  json::object chatToolCallDelta(const ChatFunctionCall& call, const ChatToolCallDeltaOptions& options);
  responses_stream::PreflightedResponsesStream chatSourceFromPrepared(responses_stream::PreflightedResponsesStream source,
                                                                    responses_failover_stream::PreparedResponsesStream prepared);
  inline constexpr std::string_view EMPTY_UPSTREAM_COMPLETION_MESSAGE = "Upstream response completed with no translated semantic output.";
  json::object emptyUpstreamCompletionError();
  void markChatSemanticOutput(const openai_telemetry::UsageContext* context);
  void markFinalizedChatToolOutput(const openai_telemetry::UsageContext* context, const ChatFunctionCallAccumulator& functionCalls);
  bool translatedChatOutputObserved(const std::string& outputText, const std::string& refusal, const ChatFunctionCallAccumulator& functionCalls);
  rt::Task<void> recordSuccessfulChatCompletion(const openai_telemetry::UsageContext* context, openai_telemetry::MeteredTransportLifecycle lifecycle,
                                               std::optional<openai_telemetry::UsageTokens> usage,
                                               std::function<void(openai_telemetry::ResponseStreamTerminalType)> onResponseTerminal = {});
  void recordEmptyUpstreamCompletion(const openai_telemetry::UsageContext* context, const openai_telemetry::MeteredTransportLifecycle& lifecycle,
                                    const std::optional<openai_telemetry::UsageTokens>& usage,
                                    const std::function<void(openai_telemetry::ResponseStreamTerminalType)>& onResponseTerminal = {});
}
