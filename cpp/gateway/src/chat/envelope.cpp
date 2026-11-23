// Port of src/chat/envelope.ts.
#include "uos/chat/envelope.hpp"
#include "uos/chat/completions.hpp"
#include "uos/chat/stream_translation.hpp"
#include "uos/deepseek/handlers.hpp"
#include "uos/provider/lithos_handlers.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/provider/cerebras.hpp"
#include "uos/provider/gemini.hpp"
#include "uos/provider/cerebras_handlers.hpp"
#include "uos/provider/openrouter_handlers.hpp"
#include "uos/provider/openrouter_request_controls.hpp"
#include "uos/provider/selection.hpp"
#include "uos/codex/auth.hpp"
#include "uos/models/codex_models_fetch.hpp"
#include "uos/openai.hpp"
#include "uos/input_normalization.hpp"
#include "uos/request_policy.hpp"
#include "uos/request.hpp"
#include "uos/inference_deadline.hpp"
#include "uos/paid_fallback/health.hpp"
#include "uos/paid_fallback/routing.hpp"
#include "uos/upstream_wire.hpp"
#include "uos/utils.hpp"
#include "uos/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/string.hpp"

#include <utility>
namespace uos::chat::envelope {
namespace {
  namespace tel = openai_telemetry;
  namespace policy = request_policy;
  namespace norm = input_normalization;
  namespace rs = responses_stream;
  namespace fs = responses_failover_stream;
  namespace sel = provider::selection;
  struct NotOk { rt::Response response; };
  template<class T> using Step = std::variant<T, NotOk>;
  rt::Response invalid(const std::string& message, std::optional<std::string> param = std::nullopt) {
    http::OpenaiErrorOptions options; if (param) options.param = param;
    return http::openaiError(400, message, "invalid_request_error", options);
  }
  json::object recordOf(const json::value& value) {
    if (value.is_object()) return value.as_object();
    json::object object;
    if (value.is_array()) for (std::size_t i = 0; i < value.as_array().size(); ++i) rt::objectDefine(object, rt::numberToString(static_cast<double>(i)), value.as_array()[i]);
    return object;
  }
  struct Envelope { json::object rawRecord; std::vector<std::string> warnings; std::optional<json::object> jsonObjectTextFormat; std::optional<sel::ProviderSelection> selection; };
  rt::Task<Step<Envelope>> parseChatCompletionsEnvelope(rt::Request req) {
    rt::Promise<json::value> bodyPromise(request::readJsonBody(req)); auto body = co_await bodyPromise.wait();
    if (!rt::truthy(body) || !utils::isRecord(body)) co_return NotOk{invalid("Invalid JSON body")};
    auto raw = body.is_object() ? std::move(body.as_object()) : recordOf(body);
    const auto model = rt::trim(rt::getString(raw, "model").value_or(""));
    if (model.size() > rt::limits::kRegexSubjectBytes) co_return NotOk{invalid("model is too long", "model")};
    rt::Promise<std::optional<sel::ProviderSelection>> selected(sel::loadProviderSelectionCached()); const auto selection = co_await selected.wait();
    const auto requested = deepseek::index::deepSeekUpstreamModelFor(model);
    const bool enabled = sel::isProviderEnabled("deepseek", selection);
    if (requested && !enabled && rt::hasOwn(raw, "thinking")) co_return NotOk{invalid("The DeepSeek `thinking` extension requires the DeepSeek provider to be enabled; remove `thinking` or enable provider `deepseek`.", "thinking")};
    std::optional<rt::OrderedSet<std::string>> extensions;
    if (enabled && requested) extensions = openai::DEEPSEEK_CHAT_EXTENSION_KEYS;
    else { rt::Promise<std::optional<rt::OrderedSet<std::string>>> pending(provider::openrouter_request_controls::openRouterPromptCacheRequestExtensions(raw)); extensions = co_await pending.wait(); }
    const auto unknown = openai::findUnknownKey(raw, openai::CHAT_COMPLETIONS_ALLOWED_KEYS, extensions ? &*extensions : nullptr);
    if (unknown) co_return NotOk{invalid("Unrecognized request argument supplied: " + *unknown)};
    if (extensions && extensions->has("cache_control")) { const auto error = provider::openrouter_request_controls::validateOpenRouterPromptCacheRequestControl(raw); if (error) co_return NotOk{*error}; }
    const auto controls = norm::validatePromptCacheControls(raw);
    if (const auto* error = std::get_if<norm::NormalizationResultNotOk>(&controls)) co_return NotOk{invalid(error->message, error->param)};
    const auto* format = rt::get(raw, "response_format"); std::optional<json::object> jsonFormat;
    if (utils::isRecord(format) && recordOf(*format).size() == 1 && utils::getString(rt::get(*format, "type")) == "json_object") jsonFormat = json::object{{"type", "json_object"}};
    rt::OrderedSet<std::string> handled{"messages", "model", "stream", "reasoning_effort", "max_completion_tokens", "tools", "tool_choice", "parallel_tool_calls", "prompt_cache_key", "prompt_cache_options", "prompt_cache_retention", "stream_options"};
    if (jsonFormat) handled.add("response_format");
    auto warnings = policy::buildIgnoredWarnings(raw, handled);
    co_return Envelope{std::move(raw), std::move(warnings), jsonFormat, selection};
  }
  struct Model { std::string modelRaw; std::string model; std::optional<double> maxCompletionTokens; };
  rt::Task<Step<Model>> resolveChatCompletionsModel(json::object raw, const tel::UsageContext* context) {
    const auto* value = rt::get(raw, "model"); const auto string = utils::getString(value);
    if (rt::hasOwn(raw, "model") && !string && !rt::isNullish(value)) co_return NotOk{invalid("model must be a string")};
    std::string modelRaw = rt::trim(string.value_or(""));
    if (modelRaw.empty()) { rt::Promise<std::optional<std::string>> pending(openai::getDefaultModel()); const auto model = co_await pending.wait(); if (!model) co_return NotOk{openai::defaultModelUnavailableError()}; modelRaw = *model; }
    const auto model = norm::normalizeModelForCodex(modelRaw);
    if (context && context->responseTelemetry) context->responseTelemetry->model = modelRaw;
    const auto allowance = policy::parseMaxCompletionTokensField(rt::get(raw, "max_completion_tokens"));
    if (const auto* error = std::get_if<policy::ParseFieldNotOk>(&allowance)) co_return NotOk{invalid(error->message, "max_completion_tokens")};
    co_return Model{modelRaw, model, std::get<policy::ParseMaxCompletionTokensFieldOk>(allowance).value};
  }
  struct Options { policy::CodexModelMetadata modelMetadata; json::array messagesRaw; std::optional<defaults::ReasoningEffort> reasoningEffort; bool stream; bool includeUsage; };
  rt::Task<Step<Options>> validateChatCompletionsOptions(std::string model, std::string modelRaw, json::object raw) {
    rt::Promise<policy::CodexModelMetadata> pending(policy::getCodexModelMetadata(model, "chat.completions")); const auto metadata = co_await pending.wait();
    const auto unavailable = policy::validateCodexModelAvailable(modelRaw, "chat.completions", metadata); if (unavailable) co_return NotOk{*unavailable};
    const auto capability = openai::temporaryFreeSurplusCapabilityError(model, raw); if (capability) co_return NotOk{*capability};
    const auto* messages = rt::getArray(raw, "messages"); if (!messages) co_return NotOk{invalid("messages must be an array")};
    if (messages->empty()) co_return NotOk{invalid("messages must be a non-empty array")};
    const auto reasoning = policy::parseReasoningEffortField(rt::get(raw, "reasoning_effort"), "reasoning_effort");
    if (const auto* error = std::get_if<policy::ParseFieldNotOk>(&reasoning)) co_return NotOk{invalid(error->message, "reasoning_effort")};
    const auto stream = policy::parseStreamField(rt::get(raw, "stream"));
    if (const auto* error = std::get_if<policy::ParseFieldNotOk>(&stream)) co_return NotOk{invalid(error->message, "stream")};
    const auto streamOptions = policy::parseChatStreamOptions(rt::get(raw, "stream_options"));
    if (const auto* error = std::get_if<policy::ParseFieldNotOk>(&streamOptions)) co_return NotOk{invalid(error->message, "stream_options")};
    co_return Options{metadata, *messages, std::get<policy::ParseReasoningEffortFieldOk>(reasoning).value, std::get<policy::ParseStreamFieldOk>(stream).value, std::get<policy::ParseChatStreamOptionsOk>(streamOptions).includeUsage};
  }
  struct Input { std::vector<::uos::types::ResponseInputItem> input; std::optional<std::string> instructions; };
  Step<Input> normalizeChatCompletionsInput(const json::array& messages, const Model& model, const Options& options, const json::object& raw) {
    std::vector<norm::NormalizedChatMessage> normalized; bool preserve = false;
    for (std::size_t i = 0; i < messages.size(); ++i) {
      const auto converted = norm::normalizeChatMessage(messages[i], static_cast<double>(i));
      if (const auto* error = std::get_if<norm::NormalizationResultNotOk>(&converted)) return NotOk{invalid(error->message, error->param)};
      const auto value = std::get<norm::NormalizationResultOk<norm::NormalizedChatMessage>>(converted).value;
      if (value.instructionContent) for (const auto& item : *value.instructionContent) { const auto jsonItem = json::value_from(item); const auto* breakpoint = rt::get(jsonItem, "prompt_cache_breakpoint"); if (utils::getString(rt::get(jsonItem, "type")) != "output_text" && utils::getString(breakpoint ? rt::get(*breakpoint, "mode") : nullptr) == "explicit") preserve = true; }
      normalized.push_back(value);
    }
    Input result; std::vector<std::string> parts;
    for (const auto& message : normalized) {
      if (preserve && message.instructionContent) result.input.emplace_back(::uos::types::ResponseMessageItem{"developer", *message.instructionContent});
      else if (message.instruction && !rt::trim(*message.instruction).empty()) parts.push_back(rt::trim(*message.instruction));
      result.input.insert(result.input.end(), message.input.begin(), message.input.end());
    }
    if (result.input.empty()) result.input.emplace_back(::uos::types::ResponseMessageItem{"user", {::uos::types::MessageContentItemInputText{""}}});
    if (!preserve) result.instructions = rt::trim(rt::join(parts, "\n\n"));
    const auto error = policy::validateKnownUnsupportedPromptCacheUse(model.modelRaw, options.modelMetadata, raw, result.input, "messages"); if (error) return NotOk{*error};
    return result;
  }
  struct Built { json::object codexBody; defaults::ReasoningEffort defaultReasoningLabel; };
  rt::Task<Built> buildChatCompletionsCodexBody(Model model, Input input, Options options, Envelope envelope) {
    rt::Promise<defaults::ReasoningEffort> pending(openai::getDefaultReasoningEffort()); const auto effort = co_await pending.wait();
    const auto label = policy::resolveDefaultReasoningLabel(options.modelMetadata.reasoning, effort);
    const json::object reasoning{{"effort", policy::reasoningEffortForCodexRequest(options.reasoningEffort.value_or(label), options.modelMetadata.reasoning)}};
    models::codex_models_fetch::BuildCodexRequestOptions buildOptions; buildOptions.reasoning = reasoning; buildOptions.instructions = rt::orUndefined(input.instructions);
    auto body = models::codex_models_fetch::buildCodexRequest(model.model, input.input, buildOptions);
    if (envelope.jsonObjectTextFormat) body["text"] = json::object{{"format", *envelope.jsonObjectTextFormat}};
    if (model.maxCompletionTokens) body["max_output_tokens"] = rt::makeNumber(*model.maxCompletionTokens);
    const std::vector<policy::PassthroughToolSchemaKey> keys{"tools", "tool_choice", "parallel_tool_calls", "prompt_cache_key", "prompt_cache_options", "prompt_cache_retention"};
    policy::applyPassthroughToCodexRequest(body, envelope.rawRecord, keys); body["store"] = false; co_return Built{body, label};
  }
  std::optional<tel::UsageTokens> completedTerminalUsageOf(const fs::PreparedResponsesStream& prepared) {
    if (!prepared.terminal || prepared.terminal->type != "response.completed") return std::nullopt;
    const auto* response = rt::get(prepared.terminal->value, "response"); return utils::isRecord(response) ? tel::extractUsageTokens(rt::get(*response, "usage")) : std::nullopt;
  }
  bool chatCompletionPreflightIsEmpty(const fs::PreparedResponsesStream& prepared, const std::optional<tel::UsageTokens>& usage, const tel::UsageContext* context) {
    if (!prepared.terminal || prepared.terminal->type != "response.completed" || prepared.semantic) return false;
    try { return stream_translation::preparedChatCompletionIsEmpty(prepared); } catch (...) { tel::recordTerminalUsage(context, usage, false); throw; }
  }
  rt::Task<rt::Response> rejectEmptyChatCompletion(fs::PreparedResponsesStream prepared, std::optional<tel::UsageTokens> usage, const tel::UsageContext* context, tel::MeteredTransportLifecycle lifecycle, std::function<void(std::string)> resolve, std::string provider, std::vector<std::string> warnings) {
    for (const auto& event : prepared.buffered) tel::recordResponsesEventTelemetry(context, event);
    stream_translation::recordEmptyUpstreamCompletion(context, lifecycle, usage, resolve);
    try { (void)co_await prepared.iterator.return_(); } catch (...) {} co_await rt::microtask();
    const rt::Nullable<std::string> param(nullptr);
    co_return tel::streamErrorResponse(502, std::string(stream_translation::EMPTY_UPSTREAM_COMPLETION_MESSAGE), "empty_upstream_completion", provider, warnings, "server_error", param);
  }
  rt::Task<rt::Response> respondChatCompletionsDispatchFailure(rt::Reason error, rt::AbortSignal signal, rt::AbortSignal downstream, const tel::UsageContext* context) {
    const auto type = tel::classifyPreHeaderFailure(error, signal, downstream); tel::recordStreamTerminalType(context, type);
    if (type != "cancelled") upstream_wire::logRedactedUpstreamError("[ai.ubq.fi] Upstream fetch failed:", error);
    co_await tel::recordErrorUsage(context).wait(); co_return upstream_wire::toPreHeaderErrorResponse(error, type, context && context->responseTelemetry ? context->responseTelemetry->provider : std::nullopt);
  }
  rt::Task<rt::Response> respondChatCompletionsUpstreamHttpFailure(rt::Response upstream, std::string provider, rt::AbortSignal signal, const tel::UsageContext* context, tel::MeteredTransportLifecycle lifecycle, std::function<void()> clear) {
    lifecycle.terminal("response.failed", std::nullopt); tel::recordStreamTerminalType(context, "response.failed"); co_await tel::recordErrorUsage(context).wait();
    auto cleanup = rt::scopeExit(clear); rt::Promise<rt::Response> pending(upstream_wire::toOpenAiUpstreamErrorResponse(upstream, provider, signal)); const auto normalized = co_await pending.wait();
    co_return tel::attachResponseTelemetry(normalized, context && context->responseTelemetry ? context->responseTelemetry : tel::createResponseTelemetryState());
  }
  struct Dispatched { rs::PreflightedResponsesStream preflight; std::string provider; tel::MeteredTransportLifecycle lifecycle; rt::AbortSignal signal; std::function<void(std::string)> resolveCodexProbe; std::vector<std::string> combinedWarnings; };
  rt::Task<void> cancelPreparedChatSource(rs::ResponsesStreamIterator iterator, std::function<void(rt::Reason)> abort, rt::Reason reason) {
    abort(std::move(reason));
    try { co_await iterator.return_(); } catch (...) {}
    co_await rt::microtask();
  }
  rt::Task<Step<Dispatched>> dispatchAndPreflightChatCompletions(json::object body, rt::Request req, Model model, Options options, std::optional<std::string> clientVersion, std::string reasoning, std::vector<std::string> warnings, const tel::UsageContext* context, rt::AbortSignal downstream) {
    const auto deadline = options.stream ? std::optional(inference_deadline::createStreamFirstEventDeadline(downstream)) : std::nullopt;
    const auto signal = deadline ? deadline->signal : openai::inferenceSignal(req, context); std::function<void()> clear = [deadline] { if (deadline) deadline->clear(); };
    const auto codexPreflight = options.stream ? std::make_shared<paid_fallback::routing::CodexStreamPreflight>() : nullptr;
    std::optional<tel::RoutedResponsesUpstream> routed; rt::Reason dispatchError;
    try {
      paid_fallback::routing::FetchResponsesWithPaidFallbackOptions fetchOptions{.model = model.model, .route = "chat.completions", .stream = options.stream, .reasoning = reasoning, .usageContext = context, .clientVersion = rt::orUndefined(clientVersion), .signal = signal};
      fetchOptions.codexStreamPreflight = codexPreflight;
      auto task = paid_fallback::routing::fetchResponsesWithPaidFallback(body, fetchOptions); rt::Promise<tel::RoutedResponsesUpstream> pending(std::move(task)); routed = co_await pending.wait();
    } catch (...) { dispatchError = std::current_exception(); }
    if (dispatchError) { clear(); auto task = respondChatCompletionsDispatchFailure(dispatchError, signal, downstream, context); rt::Promise<rt::Response> pending(std::move(task)); const auto response = co_await pending.wait(); co_return NotOk{response}; }
    const auto upstream = routed->response; const auto provider = routed->provider; const auto providerWarnings = policy::responseWarnings(upstream); warnings.insert(warnings.end(), providerWarnings.begin(), providerWarnings.end());
    const auto lifecycle = paid_fallback::health::createMeteredTransportLifecycle(routed->paidFallback, provider, routed->paidFallbackProviderRequestId.hasValue() ? std::optional(*routed->paidFallbackProviderRequestId) : std::nullopt, routed->paidFallbackBilling.hasValue() ? std::optional(*routed->paidFallbackBilling) : std::nullopt, model.model, routed->providerHealthOnly == true, routed->paidFallbackErrorHealth);
    auto resolved = std::make_shared<bool>(false); std::function<void(std::string)> resolve = [upstream, provider, resolved](std::string type) {
      if (provider != "chatgpt_codex" || *resolved) return;
      *resolved = true;
      if (type == "response.completed") rt::spawn(codex::auth::markCodexResponseCompleted(upstream), rt::ignoreRejection);
      else if (type == "response.failed" || type == "error" || type == "eof" || type == "deadline") rt::spawn(codex::auth::markCodexResponseUpstreamError(upstream), rt::ignoreRejection);
      else rt::spawn(codex::auth::releaseCodexResponseProbe(upstream), rt::ignoreRejection);
    };
    if (routed->gatewayResponse) { clear(); tel::recordStreamTerminalType(context, upstream.status() == 504 ? "deadline" : "error"); co_await tel::recordErrorUsage(context).wait(); co_return NotOk{upstream}; }
    if (!upstream.ok()) { auto task = respondChatCompletionsUpstreamHttpFailure(upstream, provider, signal, context, lifecycle, clear); rt::Promise<rt::Response> pending(std::move(task)); const auto response = co_await pending.wait(); co_return NotOk{response}; }
    if (!upstream.body()) { clear(); resolve("error"); lifecycle.ambiguous(); tel::recordStreamTerminalType(context, "error"); co_await tel::recordErrorUsage(context).wait(); co_return NotOk{tel::streamErrorResponse(502, "Codex upstream response missing body.", "codex_upstream_missing_body", provider, warnings)}; }
    std::optional<rs::PreflightedResponsesStream> preflight; rt::Reason preflightError;
    try {
      std::optional<rs::PreflightedResponsesStream> first;
      std::optional<fs::PreparedResponsesStream> prepared;
      if (provider == "chatgpt_codex" && codexPreflight && codexPreflight->error) rt::rethrow(codexPreflight->error);
      if (provider == "chatgpt_codex" && codexPreflight && codexPreflight->prepared) {
        prepared = std::exchange(codexPreflight->prepared, std::nullopt);
        const auto iterator = prepared->iterator;
        auto cancel = [iterator, abort = deadline->abort](rt::Reason reason) { return cancelPreparedChatSource(iterator, abort, std::move(reason)); };
        first = rs::PreflightedResponsesStream{prepared->buffered.front(), iterator, cancel};
      } else {
        const rs::PreflightResponsesStreamOptions preflightOptions;
        auto firstTask = rs::preflightResponsesStream(*upstream.body(), signal, preflightOptions);
        rt::Promise<rs::PreflightedResponsesStream> firstPending(std::move(firstTask));
        first = co_await firstPending.wait();
        tel::recordFirstUpstreamSseEvent(context);
        rs::ResponsesStreamIterator replay([source = *first](rt::Yield<rs::ResponsesStreamEvent> yield) -> rt::Task<void> {
          rt::Reason failure; try { co_await yield(source.first); for (;;) { auto next = co_await source.iterator.next(); if (next.done) break; co_await yield(*next.value); } } catch (...) { failure = std::current_exception(); }
          try { (void)co_await source.iterator.return_(); } catch (...) {} co_await rt::microtask(); if (failure) std::rethrow_exception(failure);
        });
        const fs::PrepareResponsesStreamForCommitOptions prepareOptions{.releaseOnProgress = options.stream && tel::supportsReasoningProgressRelease(provider)};
        auto prepareTask = fs::prepareResponsesStreamForCommit(replay, prepareOptions);
        rt::Promise<fs::PreparedResponsesStream> preparedPending(std::move(prepareTask));
        prepared = co_await preparedPending.wait();
      }
      clear();
      const auto usage = completedTerminalUsageOf(*prepared);
      if (chatCompletionPreflightIsEmpty(*prepared, usage, context)) { auto task = rejectEmptyChatCompletion(*prepared, usage, context, lifecycle, resolve, provider, warnings); rt::Promise<rt::Response> pending(std::move(task)); const auto response = co_await pending.wait(); co_return NotOk{response}; }
      preflight = stream_translation::chatSourceFromPrepared(std::move(*first), std::move(*prepared));
    } catch (...) { preflightError = std::current_exception(); }
    if (preflightError) {
      clear(); const auto type = tel::classifyStreamFailure(preflightError, signal, downstream); resolve(type); tel::recordStreamTerminalType(context, type);
      if (type != "cancelled") tel::recordResponsesFailureTelemetry(context, preflightError);
      if (type == "cancelled") lifecycle.cancelled(); else lifecycle.ambiguous();
      co_await tel::recordErrorUsage(context).wait(); co_return NotOk{tel::streamPreflightFailureResponse(type, provider, warnings)};
    }
    co_return Dispatched{*preflight, provider, lifecycle, signal, resolve, warnings};
  }
  rt::Task<rt::Response> handleInternal(rt::Request req, std::optional<tel::UsageContext> usageContext) {
    const auto* context = usageContext ? &*usageContext : nullptr;
    rt::Promise<Step<Envelope>> envelopePending(parseChatCompletionsEnvelope(req)); auto envelopeResult = co_await envelopePending.wait(); if (const auto* error = std::get_if<NotOk>(&envelopeResult)) co_return error->response; auto& envelope = std::get<Envelope>(envelopeResult);
    auto modelTask = resolveChatCompletionsModel(envelope.rawRecord, context); rt::Promise<Step<Model>> modelPending(std::move(modelTask)); const auto modelResult = co_await modelPending.wait(); if (const auto* error = std::get_if<NotOk>(&modelResult)) co_return error->response; const auto model = std::get<Model>(modelResult);
    if (provider::gemini::isGeminiModel(model.model) && provider::gemini::geminiOperatorOptIn() && sel::isProviderEnabled("gemini", envelope.selection)) {
      auto task = provider::gemini::handleGeminiChatCompletions(req, envelope.rawRecord, model.modelRaw, context);
      rt::Promise<rt::Response> pending(std::move(task));
      auto response = co_await pending.wait();
      co_return response;
    }
    const auto cerebras = provider::cerebras::cerebrasUpstreamModelFor(model.model);
    if (sel::isProviderEnabled("cerebras", envelope.selection) && cerebras) { auto task = provider::cerebras_handlers::handleCerebrasChatCompletions(req, envelope.rawRecord, model.modelRaw, context, *cerebras); rt::Promise<rt::Response> pending(std::move(task)); auto response = co_await pending.wait(); co_return response; }
    if (sel::isProviderEnabled("deepseek", envelope.selection) && deepseek::index::deepSeekUpstreamModelFor(model.model)) { auto task = deepseek::handlers::handleDeepSeekChatCompletions(req, std::move(envelope.rawRecord), model.modelRaw, context); rt::Promise<rt::Response> pending(std::move(task)); auto response = co_await pending.wait(); co_return response; }
    if (sel::isProviderEnabled("lithos", envelope.selection) && provider::lithos::lithosUpstreamModelFor(model.model)) { auto task = provider::lithos_handlers::handleLithosChatCompletions(req, envelope.rawRecord, model.modelRaw, context); rt::Promise<rt::Response> pending(std::move(task)); auto response = co_await pending.wait(); co_return response; }
    if (sel::isProviderEnabled("openrouter", envelope.selection)) { rt::Promise<std::optional<std::string>> lookup(provider::openrouter::resolveOpenRouterUpstreamModel(model.model)); const auto upstream = co_await lookup.wait(); if (upstream) { auto task = provider::openrouter_handlers::handleOpenRouterChatCompletions(req, envelope.rawRecord, model.model, context); rt::Promise<rt::Response> pending(std::move(task)); auto response = co_await pending.wait(); co_return response; } }
    auto optionsTask = validateChatCompletionsOptions(model.model, model.modelRaw, envelope.rawRecord); rt::Promise<Step<Options>> optionsPending(std::move(optionsTask)); const auto optionsResult = co_await optionsPending.wait(); if (const auto* error = std::get_if<NotOk>(&optionsResult)) co_return error->response; const auto options = std::get<Options>(optionsResult);
    const auto inputResult = normalizeChatCompletionsInput(options.messagesRaw, model, options, envelope.rawRecord); if (const auto* error = std::get_if<NotOk>(&inputResult)) co_return error->response; const auto input = std::get<Input>(inputResult);
    auto buildTask = buildChatCompletionsCodexBody(model, input, options, envelope); rt::Promise<Built> builtPending(std::move(buildTask)); const auto built = co_await builtPending.wait(); const auto reasoning = policy::resolveReasoningLabelFromEffort(options.reasoningEffort, built.defaultReasoningLabel);
    if (context && context->responseTelemetry) { context->responseTelemetry->reasoning = reasoning; context->responseTelemetry->outputTokenAllowance = model.maxCompletionTokens; context->responseTelemetry->semanticOutputObserved = false; }
    const tel::RecordRequestUsageDetails details{.model = model.modelRaw, .route = "chat.completions", .stream = options.stream, .reasoning = reasoning, .promptCacheKeyPresent = tel::promptCacheKeyPresent(envelope.rawRecord), .promptCacheMode = tel::promptCacheModeFor(envelope.rawRecord), .explicitBreakpointCount = tel::countExplicitPromptCacheBreakpoints(input.input)};
    co_await tel::recordRequestUsage(context, details).wait(); const auto downstream = openai::downstreamSignalFor(req, context); const auto version = options.modelMetadata.snapshot ? utils::getString(rt::get(*options.modelMetadata.snapshot, "client_version")) : std::nullopt;
    auto dispatchTask = dispatchAndPreflightChatCompletions(built.codexBody, req, model, options, version, reasoning, envelope.warnings, context, downstream); rt::Promise<Step<Dispatched>> pending(std::move(dispatchTask)); const auto dispatchResult = co_await pending.wait(); if (const auto* error = std::get_if<NotOk>(&dispatchResult)) co_return error->response; const auto dispatched = std::get<Dispatched>(dispatchResult);
    tel::recordFirstSemanticCommitment(context); std::optional<rt::Response> response;
    if (options.stream) response = completions::streamChatCompletions(dispatched.preflight, model.model, options.includeUsage, context, dispatched.provider, dispatched.lifecycle, dispatched.signal, downstream, dispatched.resolveCodexProbe);
    else { auto task = completions::completeChatCompletions(dispatched.preflight, model.model, context, dispatched.provider, dispatched.lifecycle, dispatched.signal, downstream, dispatched.combinedWarnings, dispatched.resolveCodexProbe); rt::Promise<rt::Response> completed(std::move(task)); response = co_await completed.wait(); }
    co_return policy::withUosWarning(*response, dispatched.combinedWarnings);
  }
}
rt::Task<rt::Response> handleChatCompletionsInternal(rt::Request req, const tel::UsageContext* context) { return handleInternal(req, context ? std::optional(*context) : std::nullopt); }
rt::Task<rt::Response> handleChatCompletions(rt::Request req, const tel::UsageContext* context) {
  std::function<rt::Task<rt::Response>(tel::UsageContext)> run = [req](tel::UsageContext value) { return handleInternal(req, std::move(value)); };
  return tel::runWithResponseTelemetry(context ? std::optional(*context) : std::nullopt, run);
}
}
