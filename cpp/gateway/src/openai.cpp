// Port of src/openai.ts.
#include "uos/openai.hpp"
#include "uos/http.hpp"
#include "uos/inference_deadline.hpp"
#include "uos/models/codex_models.hpp"
#include "uos/openai_schema.hpp"
#include "uos/request_policy.hpp"
#include "uos/runtime_config.hpp"
#include "uos/rt/json.hpp"

namespace uos::openai {
  std::optional<rt::Response> temporaryFreeSurplusCapabilityError(const std::string& model, const json::object& body) {
    const auto* tools = rt::getArray(body, "tools");
    if (request_policy::isTemporaryFreeSurplusModel(model) && tools && !tools->empty()) {
      return http::openaiError(400, "The model '" + model + "' does not support tools through this gateway.",
        "unsupported_model_capability", {.param = "tools"});
    }
    return std::nullopt;
  }

  rt::Task<std::optional<std::string>> getDefaultModel() {
    auto runtime = co_await runtime_config::loadRuntimeConfig();
    if (runtime) co_return runtime->default_model;
    co_return models::codex_models::getCodexModelsSnapshotDefaultModel(std::nullopt);
  }

  rt::AbortSignal downstreamSignalFor(const rt::Request& request, const openai_telemetry::UsageContext* context) {
    return context && context->downstreamSignal ? *context->downstreamSignal : request.signal();
  }

  rt::AbortSignal inferenceSignal(const rt::Request& request, const openai_telemetry::UsageContext* context) {
    return inference_deadline::createInferenceSignal(downstreamSignalFor(request, context));
  }

  std::optional<CodexBankedResetOptionsForTest> codexBankedResetOptionsForTest;

  void setCodexBankedResetOptionsForTest(std::optional<CodexBankedResetOptionsForTest> options) {
    codexBankedResetOptionsForTest = std::move(options);
  }

  rt::Response defaultModelUnavailableError() {
    return http::openaiError(503, "Default model is unavailable: no configured default model or Codex model snapshot.", "server_error");
  }

  rt::Task<defaults::ReasoningEffort> getDefaultReasoningEffort() {
    auto runtime = co_await runtime_config::loadRuntimeConfig();
    co_return runtime ? runtime->default_reasoning_effort : defaults::DEFAULT_REASONING_EFFORT;
  }

  const rt::OrderedSet<std::string> CHAT_COMPLETIONS_ALLOWED_KEYS = [] { rt::OrderedSet<std::string> result; for (const auto key : openai_schema::CHAT_COMPLETIONS_REQUEST_KEYS) result.add(std::string(key)); return result; }();
  const rt::OrderedSet<std::string> RESPONSES_ALLOWED_KEYS = [] { rt::OrderedSet<std::string> result; for (const auto key : openai_schema::RESPONSES_REQUEST_KEYS) result.add(std::string(key)); return result; }();
  const rt::OrderedSet<std::string> CODEX_RESPONSES_EXTENSION_KEYS{"client_metadata"};
  const rt::OrderedSet<std::string> DEEPSEEK_CHAT_EXTENSION_KEYS{"thinking"};

  std::optional<std::string> findUnknownKey(const json::object& record, const rt::OrderedSet<std::string>& allowed,
    const rt::OrderedSet<std::string>* extensions) {
    for (const auto& key : rt::objectKeys(record)) {
      if (!allowed.has(key) && !(extensions && extensions->has(key))) return key;
    }
    return std::nullopt;
  }
}
