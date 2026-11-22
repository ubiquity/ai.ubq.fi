// Port of src/responses-request.ts.
#include "uos/responses_request.hpp"
#include "uos/codex/index.hpp"
#include "uos/http.hpp"
#include "uos/request.hpp"
#include "uos/input_normalization.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/selection.hpp"
#include "uos/provider/openrouter_request_controls.hpp"
#include "uos/paid_fallback/health.hpp"
#include "uos/deepseek/flash_aliases.hpp"
#include "uos/openai.hpp"
#include "uos/utils.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/limits.hpp"
namespace uos::responses_request {
namespace {
  namespace n = input_normalization;
  namespace policy = request_policy;
  rt::Response invalid(std::string message, std::optional<std::string> param = std::nullopt) {
    http::OpenaiErrorOptions options; if (param) options.param = param;
    return http::openaiError(400, std::move(message), "invalid_request_error", options);
  }
  struct ResponsesInputAccumulator { std::vector<::uos::types::ResponseInputItem> converted; std::vector<::uos::types::MessageContentItem> contentBuffer; bool sawNonContentItem = false; };
  void flushResponsesContentBuffer(ResponsesInputAccumulator& accumulator) {
    if (accumulator.contentBuffer.empty()) return;
    accumulator.converted.push_back(::uos::types::ResponseMessageItem{"user", accumulator.contentBuffer}); accumulator.contentBuffer.clear();
  }
  void appendContent(::uos::types::MessageContentItem item, ResponsesInputAccumulator& accumulator) {
    if (accumulator.sawNonContentItem) accumulator.converted.push_back(::uos::types::ResponseMessageItem{"user", {std::move(item)}});
    else accumulator.contentBuffer.push_back(std::move(item));
  }
  std::optional<rt::Response> appendResponsesInputEntry(const json::value& msg, const std::string& param, ResponsesInputAccumulator& accumulator) {
    const auto messageType = msg.is_object() ? rt::getString(msg, "type") : std::nullopt;
    if (messageType == "message" || (!messageType && utils::isRecord(msg) && rt::get(msg, "role"))) {
      if (responses_failover_stream::isGatewayFailoverWarningItem(msg)) return std::nullopt;
      const auto mapped = n::normalizeResponseMessageItem(msg, param);
      if (const auto* error = std::get_if<n::NormalizationResultNotOk>(&mapped)) return invalid(error->message, error->param);
      flushResponsesContentBuffer(accumulator); accumulator.converted.push_back(std::get<n::NormalizationResultOk<::uos::types::ResponseMessageItem>>(mapped).value);
      accumulator.sawNonContentItem = true; return std::nullopt;
    }
    if (msg.is_string()) { appendContent(::uos::types::MessageContentItemInputText{std::string(msg.get_string())}, accumulator); return std::nullopt; }
    if (messageType == "input_text" || messageType == "input_image" || messageType == "input_file") {
      const auto contentItem = n::normalizeResponseContentItem(msg, param, "user");
      if (const auto* error = std::get_if<n::NormalizationResultNotOk>(&contentItem)) return invalid(error->message, error->param);
      appendContent(std::get<n::NormalizationResultOk<::uos::types::MessageContentItem>>(contentItem).value, accumulator); return std::nullopt;
    }
    if (msg.is_object() && rt::hasOwn(msg.get_object(), "prompt_cache_breakpoint")) return invalid("prompt_cache_breakpoint is only valid on supported input content blocks", param + ".prompt_cache_breakpoint");
    if (utils::isRecord(msg) && messageType && *messageType != "message") {
      if (messageType->starts_with("input_") || *messageType == "text" || *messageType == "image_url" || *messageType == "output_text") return invalid(param + ".type is not supported", param + ".type");
      flushResponsesContentBuffer(accumulator);
      const auto normalized = n::normalizeFunctionCallOutputItem(msg.get_object(), param);
      if (const auto* error = std::get_if<n::NormalizationResultNotOk>(&normalized)) return invalid(error->message, error->param);
      accumulator.converted.push_back(std::get<n::NormalizationResultOk<::uos::types::ResponseInputItem>>(normalized).value); accumulator.sawNonContentItem = true; return std::nullopt;
    }
    return invalid("Invalid message in input[]", param);
  }
}
rt::Task<ResponsesStep<ReadResponsesRequestValue>> readResponsesRequest(rt::Request req, const json::value* parsedBody) {
  json::value rawBody;
  if (!parsedBody) { auto task = request::readJsonBody(req); rawBody = co_await std::move(task); co_await rt::microtask(); }
  else rawBody = *parsedBody;
  if (!rt::truthy(rawBody) || !utils::isRecord(rawBody)) co_return ResponsesStepNotOk{invalid("Invalid JSON body")};
  // A TypeScript array passes the original isRecord guard, then has no named request fields.
  json::object record;
  if (rawBody.is_object()) record = std::move(rawBody.get_object());
  else for (std::size_t i = 0; i < rawBody.get_array().size(); ++i) record[rt::numberToString(static_cast<double>(i))] = std::move(rawBody.get_array()[i]);
  if (rt::trim(rt::getString(record, "model").value_or("")).size() > rt::limits::kRegexSubjectBytes)
    co_return ResponsesStepNotOk{invalid("model is too long", "model")};
  co_return ResponsesStepOk<ReadResponsesRequestValue>{{std::move(record)}};
}
std::optional<rt::Response> validateResponsesRequestFields(const json::object& rawRecord, const ::uos::types::ResponsesRequest& rawBody, const rt::OrderedSet<std::string>* providerExtensions) {
  auto extensions = openai::CODEX_RESPONSES_EXTENSION_KEYS;
  if (providerExtensions) for (const auto& key : *providerExtensions) extensions.add(key);
  const auto unknownKey = openai::findUnknownKey(rawRecord, openai::RESPONSES_ALLOWED_KEYS, &extensions);
  if (unknownKey && !unknownKey->empty()) return invalid("Unrecognized request argument supplied: " + *unknownKey);
  if (providerExtensions && providerExtensions->has("cache_control")) {
    const auto error = provider::openrouter_request_controls::validateOpenRouterPromptCacheRequestControl(rawRecord); if (error) return error;
  }
  const auto positiveInteger = [](const json::value* value) { return rt::isNullish(value) || (rt::Number::isSafeInteger(*value) && rt::toNumber(value) > 0); };
  if (!positiveInteger(rt::get(rawRecord, "max_output_tokens"))) return invalid("max_output_tokens must be a positive integer", "max_output_tokens");
  const auto* parallel = rt::get(rawRecord, "parallel_tool_calls"); if (parallel && !parallel->is_bool()) return invalid("parallel_tool_calls must be a boolean", "parallel_tool_calls");
  if (!positiveInteger(rt::get(rawRecord, "max_tool_calls"))) return invalid("max_tool_calls must be a positive integer", "max_tool_calls");
  const auto controls = n::validatePromptCacheControls(rawRecord);
  if (const auto* error = std::get_if<n::NormalizationResultNotOk>(&controls)) return invalid(error->message, error->param);
  if (rt::hasOwn(rawRecord, "client_metadata")) {
    const auto* metadata = rt::getObject(rawBody, "client_metadata");
    if (!metadata || std::any_of(metadata->begin(), metadata->end(), [](const auto& entry) { return !entry.value().is_string(); })) return invalid("client_metadata must be an object with string values", "client_metadata");
  }
  return std::nullopt;
}
ResponsesStep<ResponsesStreamSettings> resolveResponsesStreamSettings(const json::object& rawRecord, const ::uos::types::ResponsesRequest& rawBody) {
  const rt::OrderedSet<std::string> used{"model", "input", "stream", "reasoning", "instructions", "tools", "tool_choice", "parallel_tool_calls", "max_output_tokens", "prompt_cache_key", "prompt_cache_options", "prompt_cache_retention", "text", "include", "context_management", "client_metadata"};
  auto warnings = policy::buildIgnoredWarnings(rawRecord, used); const auto parsedStream = policy::parseStreamField(rt::get(rawBody, "stream"));
  if (const auto* error = std::get_if<policy::ParseFieldNotOk>(&parsedStream)) return ResponsesStepNotOk{invalid(error->message, "stream")};
  return ResponsesStepOk<ResponsesStreamSettings>{{warnings, std::get<policy::ParseStreamFieldOk>(parsedStream).value}};
}
rt::Task<ResponsesStep<ResponsesModel>> resolveResponsesModel(const json::object& rawRecord, const openai_telemetry::UsageContext* usageContext) {
  const ResolveResponsesModelOptions options; auto task = resolveResponsesModel(rawRecord, usageContext, options); auto awaitedResult1 = co_await std::move(task); co_return awaitedResult1;
}
rt::Task<ResponsesStep<ResponsesModel>> resolveResponsesModel(const json::object& rawRecord, const openai_telemetry::UsageContext* usageContext, ResolveResponsesModelOptions options) {
  const auto* rawModelValue = rt::get(rawRecord, "model"); const auto modelRawValue = rt::getString(rawRecord, "model");
  if (rt::hasOwn(rawRecord, "model") && !modelRawValue && !rt::isNullish(rawModelValue)) co_return ResponsesStepNotOk{invalid("model must be a string")};
  auto modelRaw = rt::trim(modelRawValue.value_or(""));
  if (modelRaw.empty()) { auto defaultModel = co_await openai::getDefaultModel(); co_await rt::microtask(); if (!defaultModel || defaultModel->empty()) co_return ResponsesStepNotOk{openai::defaultModelUnavailableError()}; modelRaw = *defaultModel; }
  const auto model = n::normalizeModelForCodex(modelRaw);
  if (usageContext && usageContext->responseTelemetry) usageContext->responseTelemetry->model = modelRaw;
  const std::string route = "responses"; auto metadataTask = policy::getCodexModelMetadata(model, route); auto modelMetadata = co_await std::move(metadataTask); co_await rt::microtask();
  bool verifiedOpenLuxResponses = false;
  if (options.deepSeekWaterfallPaidHop == "openlux" && deepseek::flash_aliases::isDeepSeekFlashPaidModel(model)) {
    provider::metered::FetchMeteredModelsOptions catalogOptions; catalogOptions.cachedOnly = true;
    auto catalogTask = provider::metered::fetchMeteredModels(catalogOptions); auto catalog = co_await std::move(catalogTask); co_await rt::microtask();
    auto selection = co_await provider::selection::loadProviderSelectionCached(); co_await rt::microtask();
    paid_fallback::health::PaidRoutingStateInput input{.meteredCatalog = catalog, .surplusCatalog = std::nullopt, .codexModelKnown = false, .endpointType = "openai-response", .requestUsesTools = false, .model = model, .selection = selection, .allowedPaidProviders = options.allowedPaidProviders, .deepSeekWaterfallPaidHop = options.deepSeekWaterfallPaidHop};
    const auto routing = paid_fallback::health::resolvePaidRoutingState(input); verifiedOpenLuxResponses = rt::includes(routing.paidProviders, std::string("metered"));
  }
  const auto availability = verifiedOpenLuxResponses ? std::nullopt : policy::validateCodexModelAvailable(modelRaw, route, modelMetadata);
  if (availability) co_return ResponsesStepNotOk{*availability};
  const auto capability = openai::temporaryFreeSurplusCapabilityError(model, rawRecord); if (capability) co_return ResponsesStepNotOk{*capability};
  co_return ResponsesStepOk<ResponsesModel>{{modelRaw, model, modelMetadata}};
}
ResponsesStep<std::vector<::uos::types::ResponseInputItem>> normalizeResponsesInput(const json::value& inputRaw) { return normalizeResponsesInput(&inputRaw); }
ResponsesStep<std::vector<::uos::types::ResponseInputItem>> normalizeResponsesInput(const json::value* inputRaw) {
  using Result = ResponsesStepOk<std::vector<::uos::types::ResponseInputItem>>;
  if (!inputRaw) return Result{{}};
  if (inputRaw->is_string()) return Result{{::uos::types::ResponseMessageItem{"user", {::uos::types::MessageContentItemInputText{std::string(inputRaw->get_string())}}}}};
  if (!inputRaw->is_array()) return ResponsesStepNotOk{invalid("input must be a string or an array")};
  ResponsesInputAccumulator accumulator;
  for (std::size_t i = 0; i < inputRaw->get_array().size(); ++i) {
    const auto error = appendResponsesInputEntry(inputRaw->get_array()[i], "input[" + rt::numberToString(static_cast<double>(i)) + "]", accumulator);
    if (error) return ResponsesStepNotOk{*error};
  }
  if (!accumulator.sawNonContentItem || !accumulator.contentBuffer.empty()) flushResponsesContentBuffer(accumulator);
  return Result{std::move(accumulator.converted)};
}
rt::Task<ResponsesStep<ResponsesReasoning>> resolveResponsesReasoning(const json::object& rawRecord, const ::uos::types::ResponsesRequest& rawBody,
  const policy::CodexModelMetadata& modelMetadata, const openai_telemetry::UsageContext* usageContext) {
  const auto reasoning = policy::parseReasoningParam(rt::get(rawBody, "reasoning"));
  if (const auto* error = std::get_if<policy::ParseFieldNotOk>(&reasoning)) co_return ResponsesStepNotOk{invalid(error->message, "reasoning")};
  std::optional<std::string> instructions;
  if (rt::hasOwn(rawRecord, "instructions")) {
    const auto* value = rt::get(rawBody, "instructions");
    if (value && value->is_null()) instructions = std::nullopt;
    else if (value && value->is_string()) instructions = std::string(value->get_string());
    else co_return ResponsesStepNotOk{invalid("instructions must be a string")};
  }
  const auto defaultEffort = co_await openai::getDefaultReasoningEffort(); co_await rt::microtask();
  const auto& modelReasoning = modelMetadata.reasoning;
  const auto defaultLabel = policy::resolveDefaultReasoningLabel(modelReasoning, defaultEffort);
  const auto& parsed = std::get<policy::ParseReasoningParamOk>(reasoning).value;
  const auto label = policy::resolveReasoningLabelFromParam(parsed, defaultLabel);
  if (usageContext && usageContext->responseTelemetry) usageContext->responseTelemetry->reasoning = label;
  auto value = policy::normalizeReasoningParamForCodex(parsed, modelReasoning);
  if (!value && !parsed) value = json::object{{"effort", policy::reasoningEffortForCodexRequest(defaultLabel, modelReasoning)}};
  co_return ResponsesStepOk<ResponsesReasoning>{{label, std::move(instructions), std::move(value)}};
}
ResponsesUpstreamBodies buildResponsesUpstreamBodies(const std::string& model, const std::vector<::uos::types::ResponseInputItem>& input,
  const std::optional<std::string>& instructions, const std::optional<json::object>& reasoning, const json::object& rawRecord) {
  models::codex_models_fetch::BuildCodexRequestOptions options; options.reasoning = rt::orUndefined(reasoning); options.instructions = rt::orUndefined(instructions);
  auto codexBody = codex::index::buildCodexRequest(model, input, options);
  if (const auto* value = rt::get(rawRecord, "max_output_tokens")) codexBody["max_output_tokens"] = *value;
  const std::vector<policy::PassthroughToolSchemaKey> keys{"tools", "tool_choice", "parallel_tool_calls", "prompt_cache_key", "prompt_cache_options", "prompt_cache_retention", "text", "include", "context_management"};
  policy::applyPassthroughToCodexRequest(codexBody, rawRecord, keys); codexBody["model"] = model; codexBody["stream"] = true; codexBody["store"] = false;
  auto removedProviderBody = codexBody; removedProviderBody["input"] = json::value_from(input);
  for (const auto* key : {"max_output_tokens", "max_tool_calls", "metadata", "safety_identifier", "service_tier", "temperature", "top_p", "truncation", "user"}) if (const auto* value = rt::get(rawRecord, key)) removedProviderBody[key] = *value;
  return {std::move(codexBody), std::move(removedProviderBody)};
}
}
