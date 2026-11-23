#pragma once
// Port of src/provider/openrouter.ts.
#include "uos/api_key_policy.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/sentinel/upstream_capture.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uos::provider::openrouter {
  inline constexpr std::string_view OPENROUTER_BASE_URL = "https://openrouter.ai/api/v1";
  inline constexpr std::string_view OPENROUTER_API_KEY_ENV = "OPENROUTER_API_KEY";
  inline constexpr std::string_view OPENROUTER_SYSTEMONE_URL = "https://openrouter.ai/api/v1/systemone";
  inline constexpr std::string_view OPENROUTER_DECISIONS_URL = "https://openrouter.ai/api/alpha/decisions";
  inline constexpr std::string_view OPENROUTER_CHAT_COMPLETIONS_URL = "https://openrouter.ai/api/v1/chat/completions";
  inline constexpr std::string_view OPENROUTER_RESPONSES_URL = "https://openrouter.ai/api/v1/responses";
  inline constexpr double OPENROUTER_FETCH_TIMEOUT_MS = 20'000;
  inline constexpr double OPENROUTER_DECISIONS_TIMEOUT_MS = 60'000;
  using OpenRouterErrorCode = std::string;
  class OpenRouterError : public rt::JsError {
  public:
    OpenRouterErrorCode code;
    double status;
    std::optional<double> upstreamStatus;
    bool beforeTransport;
    explicit OpenRouterError(OpenRouterErrorCode code_, double status_, std::optional<double> upstreamStatus_ = std::nullopt, bool beforeTransport_ = false)
      : rt::JsError(code_, "OpenRouterError"), code(std::move(code_)), status(status_), upstreamStatus(upstreamStatus_), beforeTransport(beforeTransport_) {}
  };
  using OpenRouterFetch = rt::Fetch;
  std::vector<std::string> openRouterTextModelIds();
  std::vector<std::string> openRouterDecisionModelIds();
  std::optional<std::string> openRouterUpstreamModelFor(std::string_view model);
  std::optional<std::string> openRouterDecisionUpstreamModelFor(std::string_view model);
  rt::Task<std::optional<std::string>> resolveOpenRouterUpstreamModel(std::string model);
  rt::Task<std::optional<std::string>> resolveOpenRouterDecisionUpstreamModel(std::string model);
  struct OpenRouterDispatchHooks {
    std::function<rt::Task<std::optional<api_key_policy::ApiKeyProviderDispatch>>()> beforeDispatch{};
    std::function<void()> onDispatch{};
    std::function<void()> onHeaders{};
    std::optional<sentinel::upstream_capture::SentinelUpstreamRecorder> sentinelUpstreamRecorder{};
  };
  struct OpenRouterDispatchInit {
    rt::AbortSignal signal;
    std::optional<OpenRouterDispatchHooks> hooks{};
  };
  rt::Task<rt::Response> fetchOpenRouterChatCompletions(const json::object& body, const OpenRouterDispatchInit& init);
  rt::Task<rt::Response> fetchOpenRouterResponses(const json::object& body, const OpenRouterDispatchInit& init);
  std::optional<std::string> readOpenRouterApiKey();
  struct OpenRouterDecisionInput {
    json::object body;
    rt::Nullable<std::string> apiKey{};
    OpenRouterFetch fetcher{};
    std::optional<double> timeoutMs{};
    std::optional<OpenRouterDispatchHooks> hooks{};
    std::optional<rt::AbortSignal> signal{};
  };
  rt::Task<json::object> fetchOpenRouterSystemOne(const OpenRouterDecisionInput& input);
  rt::Task<json::object> fetchOpenRouterDecisions(const OpenRouterDecisionInput& input);
}
