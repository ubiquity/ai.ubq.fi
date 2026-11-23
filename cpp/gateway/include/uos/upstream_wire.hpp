#pragma once
// Port of src/upstream-wire.ts.
#include "uos/api_key_policy.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/prelude.hpp"
#include <optional>
#include <string>
#include <string_view>
namespace uos::upstream_wire {
  // Carrier for Object.assign(new Error(message), {status}) until rt has own Error fields.
  struct ErrorWithStatus : rt::JsError {
    json::value status;
    ErrorWithStatus(std::string message_, json::value status_) : rt::JsError(std::move(message_)), status(std::move(status_)) {}
  };
  bool isGatewayDeadlineResponse(const rt::Response& response, const rt::AbortSignal& requestSignal);
  std::string formatErrorSnippet(const rt::Reason& error, double maxLen = 280);
  std::string formatErrorSnippet(const json::value& error, double maxLen = 280);
  std::string formatErrorSnippet(const json::value* error, double maxLen = 280);
  std::string formatErrorSnippet(std::string_view error, double maxLen = 280);
  void logRedactedUpstreamError(const std::string& label, const rt::Reason& error);
  inline constexpr double MAX_PROVIDER_REQUEST_ID_CHARS = 256;
  std::optional<std::string> normalizeProviderRequestId(const json::value& value);
  std::optional<std::string> normalizeProviderRequestId(const json::value* value);
  std::optional<std::string> normalizeProviderRequestId(std::string_view value);
  std::optional<std::string> providerRequestIdFromResponse(const rt::Response& response);
  rt::Response toCodexErrorResponse(const rt::Reason& error, const std::optional<std::string>& provider = std::nullopt);
  rt::Response toPreHeaderErrorResponse(const rt::Reason& error, const std::string& terminalType, const std::optional<std::string>& provider = std::nullopt);
  rt::Response toCerebrasErrorResponse(const rt::Reason& error);
  rt::Headers cerebrasResponseHeaders(const std::optional<std::string>& providerRequestId, const std::optional<std::string>& warning = std::nullopt);
  inline constexpr std::string_view GPT_OSS_STREAM_DOWNGRADED_WARNING = "gpt_oss_stream_downgraded";
  struct CompletionAnswerBearingOutput {
    std::string text;
    std::optional<std::string> refusal{};
    double toolCallCount;
  };
  bool isAnswerBearingCompletion(const CompletionAnswerBearingOutput& output);
  bool chatCompletionHasAnswerBearingOutput(const json::object& completion);
  bool chatChunkHasAnswerBearingOutput(const json::object& chunk);
  rt::Response streamCerebrasChatCompletion(const json::object& completion, bool includeUsage, const rt::Headers& headers);
  rt::Task<rt::Response> toOpenAiUpstreamErrorResponse(rt::Response upstream, std::string provider, rt::AbortSignal signal);
  void cancelResponseBody(rt::Response response);
  rt::Task<rt::Response> toCerebrasUpstreamErrorResponse(rt::Response upstream, std::optional<rt::AbortSignal> signal = std::nullopt);
  rt::Response toDeepSeekErrorResponse(const rt::Reason& error);
  rt::Headers deepseekResponseHeaders(const std::optional<std::string>& providerRequestId);
  rt::Headers lithosResponseHeaders(const std::optional<std::string>& providerRequestId);
  rt::Headers openRouterResponseHeaders(const std::optional<std::string>& providerRequestId);
  rt::Response toLithosErrorResponse(const rt::Reason& error);
  rt::Task<rt::Response> toLithosUpstreamErrorResponse(rt::Response upstream, std::optional<rt::AbortSignal> signal = std::nullopt, std::optional<json::object> bodyDiagnostic = std::nullopt);
  json::object deepSeekChatBodyDiagnostic(const json::object& body);
  rt::Task<rt::Response> toDeepSeekUpstreamErrorResponse(rt::Response upstream, std::optional<rt::AbortSignal> signal = std::nullopt, std::optional<json::object> bodyDiagnostic = std::nullopt);
  rt::Response apiKeyQuotaDispatchErrorResponse(const api_key_policy::ApiKeyQuotaDispatchError& error);
}
