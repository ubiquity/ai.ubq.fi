#pragma once
// Port of src/responses-failover-stream.ts.
#include "uos/responses_stream.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/prelude.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uos::responses_failover_stream {
  // "text" | "tool_call"
  using ResponsesSemanticKind = std::string;

  inline constexpr double MAX_RESPONSES_PRECOMMIT_EVENTS = 10'000;
  inline constexpr double MAX_RESPONSES_PRECOMMIT_CHARS = 32 * 1024 * 1024;

  // `ResponsesSemanticKind | null`: std::nullopt is null.
  std::optional<ResponsesSemanticKind> responsesEventSemanticKind(const responses_stream::ResponsesStreamEvent& event);

  /** Buffered Responses payloads use the same answer-bearing output-item rule as streamed events. */
  bool responsesPayloadHasAnswerBearingOutput(const json::object& payload);

  /** Reports active hidden-reasoning work without classifying it as client-visible output. */
  bool responsesEventReportsProgress(const responses_stream::ResponsesStreamEvent& event);

  struct PreparedResponsesStream {
    responses_stream::ResponsesStreamIterator iterator;
    std::vector<responses_stream::ResponsesStreamEvent> buffered;
    double bufferedChars;
    std::optional<responses_stream::ResponsesStreamEvent> semantic;
    std::optional<ResponsesSemanticKind> semanticKind;
    std::optional<responses_stream::ResponsesStreamEvent> terminal;
  };

  double appendResponsesPrecommitEvent(std::vector<responses_stream::ResponsesStreamEvent>& buffered, const responses_stream::ResponsesStreamEvent& event,
                                       double bufferedChars);

  // The `options` of prepareResponsesStreamForCommit.
  struct PrepareResponsesStreamForCommitOptions {
    std::function<void(const responses_stream::ResponsesStreamEvent& event)> onEvent = {};
    std::function<void(const responses_stream::ResponsesStreamEvent& event)> onProgress = {};
    std::optional<bool> releaseOnProgress = std::nullopt;
  };

  /** Holds all provider events until semantic output or a valid terminal owns the attempt. */
  [[nodiscard]] rt::Task<PreparedResponsesStream> prepareResponsesStreamForCommit(responses_stream::ResponsesStreamIterator iterator,
                                                                                  PrepareResponsesStreamForCommitOptions options);
  [[nodiscard]] rt::Task<PreparedResponsesStream> prepareResponsesStreamForCommit(responses_stream::ResponsesStreamIterator iterator);

  responses_stream::ResponsesStreamEvent responseEventFromValue(json::object value);

  responses_stream::ResponsesStreamEvent rewriteResponsesEventForWarning(const responses_stream::ResponsesStreamEvent& event, const json::object& warningItem,
                                                                         double sequenceNumber);

  responses_stream::ResponsesStreamEvent rewriteResponsesEventSequence(const responses_stream::ResponsesStreamEvent& event, double sequenceNumber);

  /** The single source of truth for the warning text this gateway injects. */
  std::string failoverWarningText(std::string_view actualModel);

  /**
   * Identifies an assistant item this gateway injected as a failover notice, so a
   * replayed conversation does not send the notice back upstream as if the user
   * or the model had written it. The id prefix alone is not enough: it is
   * attacker-controllable input, so the shape and the exact text are checked too.
   */
  // `value: unknown`: nullptr is undefined.
  bool isGatewayFailoverWarningItem(const json::value& value);
  bool isGatewayFailoverWarningItem(const json::value* value);

  // Readonly<{ item: Record<string, unknown>; events: ResponsesStreamEvent[] }>
  struct FailoverWarningEvents {
    json::object item;
    std::vector<responses_stream::ResponsesStreamEvent> events;
  };

  FailoverWarningEvents buildFailoverWarningEvents(std::string_view actualModel, std::string_view responseId, double startingSequenceNumber = 0);

  // `string | null`: std::nullopt is null.
  std::optional<std::string> responseIdFromEvents(const std::vector<responses_stream::ResponsesStreamEvent>& events);

  struct OwnedResponsesStreamFailureDetails {
    responses_stream::ResponsesStreamFailureKind failureKind;
    bool responseCreatedObserved;
    bool semanticCommitmentObserved;
    // "response.failed" | "error" | null
    std::optional<std::string> syntheticTerminalType;
    std::optional<responses_stream::ResponsesStreamEvent> upstreamTerminal;
  };

  responses_stream::ResponsesStreamEvent failureEventAfterCommit(std::string_view responseId, double sequenceNumber, const std::vector<json::object>& output = {},
                                                                 const json::object& responseTemplate = {});

  bool isSyntheticResponsesFailureEvent(const responses_stream::ResponsesStreamEvent& event);

  // OwnedResponsesStreamOptions. The lifecycle callbacks are started and never awaited.
  struct OwnedResponsesStreamOptions {
    std::optional<bool> codexContextErrorCompatibility = std::nullopt;
    std::vector<responses_stream::ResponsesStreamEvent> initial;
    responses_stream::ResponsesStreamIterator iterator;
    // `string | null`
    std::optional<std::string> responseId;
    struct Warning {
      std::string model;
    };
    std::optional<Warning> warning = std::nullopt;
    std::optional<rt::AbortSignal> signal = std::nullopt;
    std::optional<rt::AbortSignal> downstreamSignal = std::nullopt;
    // `(reason?: unknown) => void`; a null Reason is undefined.
    std::function<void(rt::Reason reason)> abortUpstream = {};
    responses_stream::MaybeAsyncCallback<responses_stream::ResponsesStreamEvent> onEvent = {};
    std::function<void(const responses_stream::ResponsesStreamEvent& event)> validateEvent = {};
    responses_stream::MaybeAsyncCallback<rt::Reason, OwnedResponsesStreamFailureDetails> onFailure = {};
    responses_stream::MaybeAsyncCallback<rt::Reason> onCancel = {};
  };

  /** Owns event ordering and guarantees at most one client-visible terminal event. */
  rt::ByteStream createOwnedResponsesStream(OwnedResponsesStreamOptions options);
}
