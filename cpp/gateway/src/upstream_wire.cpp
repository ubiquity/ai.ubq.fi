/** Keep local-vs-provider refusal provenance out of the client-visible wire. */

/**
 * A status-only 504 is still treated as a local deadline for an already
 * aborted request, while an explicitly marked upstream refusal wins over a
 * later abort of that signal.
 */

/**
 * The gateway's terminal-validity question, answered once for every
 * translation route: does this accumulated output contain something a client
 * can act on?
 *
 * Assistant text, a refusal, or a tool call is answer-bearing. Reasoning is
 * not: it is streaming progress (`reasoning` on Cerebras, `reasoning_content`
 * on DeepSeek), and a completion whose only output is reasoning hands the
 * client nothing to act on. Providers contribute only the field names their
 * wire shape uses, through the adapters below; they do not re-answer the
 * question, and a route-local copy of this rule must not come back.
 */

/** Assistant text, in the provider's text field. */

/** Refusal text, when the provider's shape carries one. */

/** How many tool calls the output carries. */

/**
 * Reads the answer-bearing view out of one Chat Completions `message` or
 * `delta` object. Both wire shapes spell the fields the same way, and the
 * provider-specific reasoning fields are deliberately absent from this view:
 * `reasoning` (Cerebras) and `reasoning_content` (DeepSeek) are streaming
 * progress, and reading either one here is exactly the bug this rule removes.
 */

/** Buffered Chat completion: every choice's `message`. */

/** Streamed Chat chunk: every choice's `delta`. */

/**
 * The LithosAI route's response headers. This provider publishes NO request-id
 * header at all, so the correlation field is normally absent; the seam exists so
 * the day a vendor sends one it has exactly one place to land.
 */

/**
 * The provider's documented final 4xx statuses. This route never retries a
 * dispatch (there is no failover, race, or backoff loop on it), and the
 * `x-should-retry: false` header states the same contract to a downstream
 * client so a caller does not turn "out of credit" or "unknown model" into a
 * retry storm.
 */

/** The three per-minute budgets whose name rides `error.type` on a 429. */

/**
 * One bounded, trimmed text field. The engine-parameter envelope carries an
 * INTEGER `code`, so a safe integer is accepted and rendered as its own digits
 * rather than being dropped; anything else is not a code this route can state.
 */

/**
 * Reads either error envelope this provider is documented to send:
 *
 * 1. `{"error":{"message":…,"type":…,"code":…}}` for auth, validation and
 *    model failures.
 * 2. `{"object":"error","message":…,"type":"BadRequestError","code":400}` with
 *    an INTEGER `code` for engine parameter violations.
 *
 * A non-JSON body is tolerated instead of throwing: the error path may not
 * itself fail, and nothing from the body is reflected as a code unless it is
 * already a string or a safe integer.
 */

/**
 * The client-visible code for one upstream refusal. The vendor semantics are
 * kept distinct rather than collapsed into one generic upstream error:
 *
 * - `402` is out of credit; the vendor's own code is carried when present.
 * - `404` is an unknown model.
 * - `429` is split by `error.type`: a per-minute budget (`requests`,
 *   `input_tokens`, `output_tokens`) becomes `rate_limit_exceeded`, while a
 *   model at capacity becomes `provider_overloaded`. A 429 whose body names
 *   neither is reported as the more common budget case.
 * - Everything else keeps the vendor's code, falling back to the route's own.
 */

/** One provider's refusal type. `402` and `429` are distinct on this wire. */

/**
 * Bounded, content-free digest of a projected DeepSeek Chat body. Upstream 4xx
 * answers are client-visible but were previously opaque in the server log: a
 * production HTTP 400 was only diagnosable by reading the client's rollout file.
 * Message text, tool names, arguments, and ids are deliberately excluded — the
 * digest carries shapes, not prompts. Only the final 16 message positions are
 * projected; fixed vocabularies and scalar counts bound nested metadata and
 * serialization independently of history and content-part sizes.
 */
#include "uos/upstream_wire.hpp"
#include "uos/bounded_response_body.hpp"
#include "uos/codex/auth.hpp"
#include "uos/deepseek/index.hpp"
#include "uos/http.hpp"
#include "uos/provider/cerebras.hpp"
#include "uos/provider/cerebras_rate_limits.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/weak.hpp"
#include "uos/utils.hpp"
#include <algorithm>
#include <array>
#include <unordered_set>
namespace uos::upstream_wire {
  namespace {
    namespace ca = codex::auth;
    namespace ce = provider::cerebras;
    namespace ds = deepseek::index;
    namespace li = provider::lithos;
    using api_key_policy::ApiKeyQuotaDispatchError;
    rt::WeakMap<rt::Response, std::string> refusalResponseOrigins;
    rt::Response markGatewayDeadlineResponse(rt::Response response) { refusalResponseOrigins.set(response, "gateway_deadline"); return response; }
    rt::Response markUpstreamRefusalResponse(rt::Response response) { refusalResponseOrigins.set(response, "upstream"); return response; }
    const std::unordered_set<std::string> REDACTED_UPSTREAM_DIAGNOSTIC_CODES{
      "api_key_quota_reservation_unavailable", "codex_auth_missing", "codex_auth_invalid", "codex_auth_refresh_failed", "refresh_token_reused", "codex_auth_refresh_unreachable", "codex_auth_owner_unavailable", "codex_auth_owner_conflict", "codex_upstream_unreachable", "gateway_timeout", "invalid_api_key", "rate_limit_exceeded", "server_error", "metered_api_key_missing", "metered_pricing_unavailable", "metered_pricing_invalid", "metered_status_unavailable", "metered_status_invalid", "metered_request_invalid", "metered_upstream_unreachable", "metered_logs_unavailable", "metered_logs_invalid", "surplus_api_key_missing", "surplus_request_invalid", "surplus_upstream_unreachable"};
    std::optional<double> redactedDiagnosticStatus(double status) { return rt::Number::isSafeInteger(status) && status >= 100 && status <= 599 ? std::optional(status) : std::nullopt; }
    json::object redactedUpstreamErrorDiagnostic(const rt::Reason& error) {
      std::string errorClass = "unknown";
      std::optional<double> status;
      std::optional<std::string> code;
      const auto typed = [&](const auto& value, std::string name) {
        errorClass = std::move(name); status = redactedDiagnosticStatus(value.status);
        if (REDACTED_UPSTREAM_DIAGNOSTIC_CODES.contains(value.code)) code = value.code;
      };
      if (error) try { std::rethrow_exception(error); }
      catch (const ApiKeyQuotaDispatchError& value) { typed(value, "ApiKeyQuotaDispatchError"); }
      catch (const ca::CodexError& value) { typed(value, "CodexError"); }
      catch (const provider::metered::MeteredError& value) { typed(value, "MeteredError"); }
      catch (const provider::surplus::SurplusError& value) { typed(value, "SurplusError"); }
      catch (const rt::DOMException&) { errorClass = "DOMException"; }
      catch (const rt::TypeError&) { errorClass = "TypeError"; }
      catch (const ErrorWithStatus& value) { errorClass = "Error"; if (rt::isNumber(value.status)) status = redactedDiagnosticStatus(rt::toNumber(value.status)); }
      catch (const rt::JsError&) { errorClass = "Error"; }
      catch (...) {}
      return {{"error_class", errorClass}, {"status", rt::orNull(status).toJson().value()}, {"code", rt::orNull(code).toJson().value()}};
    }
    rt::Response withUpstreamProviderHeader(rt::Response response, const std::optional<std::string>& provider) {
      if (!provider || provider->empty()) return response;
      auto headers = response.headers(); headers.set("x-uos-upstream", *provider);
      return rt::Response(rt::bodyInit(response.body()), {.status = response.status(), .statusText = response.statusText(), .headers = headers});
    }
    std::string paidUpstreamProviderLabel(const std::optional<std::string>& provider) { return provider == "surplus" ? "Surplus" : provider == "metered" ? "Metered" : "Codex"; }
    rt::Response codexUpstreamErrorResponse(const ca::CodexError& error) {
      const bool authFailure = error.code == "codex_auth_invalid" || error.code == "codex_auth_refresh_failed" || error.code == "refresh_token_reused";
      http::OpenaiErrorOptions options;
      if (error.code == "gateway_timeout" || error.code == "codex_auth_refresh_failed" || error.code == "refresh_token_reused") options.type = "server_error";
      if (authFailure) options.headers = rt::Headers{{"x-uos-warning", std::string(ca::CODEX_AUTH_REAUTH_WARNING)}};
      return http::openaiError(error.code == "codex_auth_refresh_failed" || error.code == "refresh_token_reused" ? 503 : error.status, error.message, error.code, options);
    }
    rt::Response unreachableUpstreamErrorResponse(const rt::Reason& error, const std::optional<std::string>& provider) {
      const auto detail = formatErrorSnippet(error);
      const auto paidProvider = provider == "metered" || provider == "surplus" ? provider : std::nullopt;
      const auto label = paidUpstreamProviderLabel(paidProvider);
      const auto message = detail.empty() ? label + " upstream request failed." : label + " upstream request failed: " + detail;
      if (paidProvider) return http::openaiError(502, message, *paidProvider + "_upstream_unreachable", {.type = "server_error", .param = std::optional<std::string>{}});
      return http::openaiError(502, message, "codex_upstream_unreachable");
    }
    rt::Response quotaResponse(const ApiKeyQuotaDispatchError& error, bool includeParam) {
      rt::Headers headers;
      for (const auto& [name, value] : error.headers) headers.set(name, value);
      http::OpenaiErrorOptions options{.type = error.errorType, .headers = headers};
      if (includeParam) options.param = std::optional<std::string>{};
      return http::openaiError(error.status, error.message, error.code, options);
    }
    template<class Error> rt::Response directErrorResponse(const rt::Reason& reason, const std::string& provider) {
      std::optional<rt::Response> response;
      if (reason) try { std::rethrow_exception(reason); }
      catch (const ApiKeyQuotaDispatchError& error) { response = quotaResponse(error, false); }
      catch (const Error& error) { response = http::openaiError(error.status, error.message, error.code, {.type = error.status >= 500 ? "server_error" : "invalid_request_error"}); }
      catch (const rt::JsError& error) {
        if (error.name == "TimeoutError") response = http::openaiError(504, "Upstream request exceeded the gateway deadline.", "gateway_timeout", {.type = "server_error"});
        else if (error.name == "AbortError") response = http::openaiError(499, "Request was cancelled.", "request_cancelled", {.type = "server_error"});
      }
      catch (...) {}
      if (!response) response = http::openaiError(502, "Upstream request could not be completed.", provider + "_upstream_unreachable", {.type = "server_error"});
      return withUpstreamProviderHeader(*response, provider);
    }
    CompletionAnswerBearingOutput answerBearingOutputFromChatFields(const json::object& fields) {
      const auto* calls = rt::getArray(fields, "tool_calls");
      return {utils::getString(rt::get(fields, "content")).value_or(""), utils::getString(rt::get(fields, "refusal")).value_or(""), calls ? static_cast<double>(calls->size()) : 0};
    }
    bool chatFieldsHaveAnswerBearingOutput(const json::object& value, std::string_view field) {
      const auto* choices = rt::getArray(value, "choices");
      if (!choices) return false;
      for (const auto& choice : *choices) if (choice.is_object()) {
        const auto* fields = rt::getObject(choice, field);
        if (fields && isAnswerBearingCompletion(answerBearingOutputFromChatFields(*fields))) return true;
      }
      return false;
    }
    json::array cerebrasChatToolCallDeltas(const json::array& toolCalls) {
      json::array result;
      for (std::size_t index = 0; index < toolCalls.size(); ++index) {
        const auto& call = toolCalls[index];
        if (!call.is_object()) continue;
        const auto* function = rt::getObject(call, "function");
        if (!function) continue;
        json::object row{{"index", rt::makeNumber(static_cast<double>(index))}};
        if (const auto* id = rt::get(call, "id")) rt::objectDefine(row, "id", *id);
        rt::objectDefine(row, "type", "function");
        json::object fn;
        for (const auto key : {"name", "arguments"}) if (const auto* field = rt::get(*function, key)) rt::objectDefine(fn, key, *field);
        rt::objectDefine(row, "function", fn); result.push_back(std::move(row));
      }
      return result;
    }
    json::object cerebrasChatDelta(const json::object& message) {
      json::object delta{{"role", "assistant"}};
      for (const auto key : {"reasoning", "content", "refusal"}) {
        const auto value = utils::getString(rt::get(message, key));
        if (value && (std::string_view(key) == "content" || !value->empty())) rt::objectDefine(delta, key, json::value(*value));
      }
      if (const auto* calls = rt::getArray(message, "tool_calls")) rt::objectDefine(delta, "tool_calls", cerebrasChatToolCallDeltas(*calls));
      return delta;
    }
    struct UpstreamErrorDetails { std::string message; std::optional<std::string> type{}; std::optional<std::string> code{}; std::optional<std::optional<std::string>> param{}; };
    std::optional<std::string> getJsonString(const json::value* value, std::string_view key) {
      if (!utils::isRecord(value)) return std::nullopt;
      const auto str = utils::getString(rt::get(*value, key));
      if (!str || rt::trim(*str).empty()) return std::nullopt;
      return rt::trim(*str);
    }
    std::optional<UpstreamErrorDetails> upstreamErrorDetailsFromParsedJson(const json::value& parsed) {
      if (!utils::isRecord(parsed)) return std::nullopt;
      const auto* rawError = rt::get(parsed, "error");
      const auto* error = utils::isRecord(rawError) ? rawError : nullptr;
      auto message = getJsonString(error, "message");
      if (!message) message = getJsonString(&parsed, "detail");
      if (!message) message = getJsonString(&parsed, "message");
      if (!message) return std::nullopt;
      UpstreamErrorDetails result{*message};
      result.type = getJsonString(error, "type"); if (!result.type) result.type = getJsonString(&parsed, "type");
      result.code = getJsonString(error, "code"); if (!result.code) result.code = getJsonString(&parsed, "code");
      if (error && error->is_object() && rt::hasOwn(error->as_object(), "param")) result.param = utils::getString(rt::get(*error, "param"));
      return result;
    }
    UpstreamErrorDetails parseUpstreamErrorDetails(const std::string& text, const std::string& statusText) {
      const auto trimmed = rt::trim(text);
      if (!trimmed.empty()) try {
        const auto details = upstreamErrorDetailsFromParsedJson(rt::jsonParse(trimmed));
        if (details) return *details;
      } catch (...) {}
      const auto snippet = trimmed.empty() ? std::string{} : formatErrorSnippet(std::string_view(trimmed));
      return {snippet.empty() ? statusText.empty() ? "Upstream request failed." : statusText : snippet};
    }
    std::string upstreamStatusToErrorType(double status, const std::optional<std::string>& type = std::nullopt) {
      if (type && !type->empty()) return *type;
      return status >= 500 ? "server_error" : status == 429 ? "rate_limit_error" : "invalid_request_error";
    }
    inline constexpr double UPSTREAM_ERROR_MESSAGE_MAX = 1'000;
    inline constexpr double UPSTREAM_ERROR_CODE_MAX = 200;
    struct UpstreamErrorDetail { std::optional<std::string> message{}; std::optional<std::string> code{}; };
    std::optional<std::string> pickString(const json::value* value, double max) {
      const auto str = utils::getString(value);
      if (!str) return std::nullopt;
      const auto trimmed = rt::trim(rt::slice(*str, 0, max));
      return trimmed.empty() ? std::nullopt : std::optional(trimmed);
    }
    const json::value* coalesceField(const json::value* error, const json::value& body, std::string_view key) {
      const auto* field = error ? rt::get(*error, key) : nullptr;
      return rt::isNullish(field) ? rt::get(body, key) : field;
    }
    UpstreamErrorDetail parseUpstreamErrorDetail(const json::value& body) {
      if (!body.is_object()) return {};
      const auto* error = rt::get(body, "error");
      if (!error || !error->is_object()) error = nullptr;
      return {pickString(coalesceField(error, body, "message"), UPSTREAM_ERROR_MESSAGE_MAX), pickString(coalesceField(error, body, "code"), UPSTREAM_ERROR_CODE_MAX)};
    }
    rt::Task<bounded_response_body::BoundedResponseBody> captureErrorBody(rt::Response upstream, std::optional<rt::AbortSignal> signal, std::string reason) {
      bounded_response_body::ReadBoundedResponseBodyOptions options{.signal = signal, .maxBytes = bounded_response_body::BOUNDED_RESPONSE_BODY_MAX_BYTES, .timeoutMs = bounded_response_body::BOUNDED_RESPONSE_BODY_TIMEOUT_MS, .cancellationReason = json::value(reason)};
      auto task = bounded_response_body::readBoundedResponseBody(upstream, options);
      auto captured = co_await std::move(task);
      co_return captured;
    }
    struct LithosUpstreamErrorDetail { std::optional<std::string> message; std::optional<std::string> type; std::optional<std::string> code; };
    std::optional<std::string> lithosErrorTextField(const json::value* value, double max) {
      if (value && value->is_string()) return pickString(value, max);
      if (value && rt::isNumber(*value) && rt::Number::isSafeInteger(rt::toNumber(*value))) return rt::numberToString(rt::toNumber(*value));
      return std::nullopt;
    }
    LithosUpstreamErrorDetail parseLithosUpstreamErrorDetail(const std::string& text) {
      try {
        const auto parsed = rt::jsonParse(text);
        if (!parsed.is_object()) return {};
        const auto* error = rt::get(parsed, "error"); if (!error || !error->is_object()) error = nullptr;
        return {lithosErrorTextField(coalesceField(error, parsed, "message"), UPSTREAM_ERROR_MESSAGE_MAX), lithosErrorTextField(coalesceField(error, parsed, "type"), UPSTREAM_ERROR_CODE_MAX), lithosErrorTextField(coalesceField(error, parsed, "code"), UPSTREAM_ERROR_CODE_MAX)};
      } catch (...) { return {}; }
    }
    bool lithosBudgetType(const std::optional<std::string>& type) { return type == "requests" || type == "input_tokens" || type == "output_tokens"; }
    std::string lithosUpstreamErrorCode(double status, const LithosUpstreamErrorDetail& detail) {
      if (status == 429) return detail.type == "provider_overloaded" || detail.code == "provider_overloaded" ? "provider_overloaded" : "rate_limit_exceeded";
      if (status == 402) return detail.code.value_or("insufficient_quota");
      if (status == 401 || status == 403) return detail.code.value_or("auth_invalid");
      if (status == 404) return detail.code.value_or("model_not_found");
      if (detail.code && !detail.code->empty()) return *detail.code;
      return lithosBudgetType(detail.type) ? "rate_limit_exceeded" : "lithos_upstream_error";
    }
    std::string lithosUpstreamErrorType(double status) { return status == 402 ? "insufficient_quota" : status == 429 ? "rate_limit_error" : status >= 500 ? "server_error" : "invalid_request_error"; }
    std::string diagnosticReasoningLabel(const json::value* value) { return value && value->is_string() ? value->as_string().empty() ? "empty" : "present" : "absent"; }
    std::string diagnosticContentLabel(const json::value* value) { return std::string(rt::isNullish(value) ? "null" : value->is_array() ? "parts" : rt::typeOf(value)); }
    template<class Range> std::optional<std::string> diagnosticKnownString(const json::value* value, const Range& known) {
      const auto str = utils::getString(value);
      return str && std::find(known.begin(), known.end(), *str) != known.end() ? str : std::nullopt;
    }
    template<std::size_t N> std::optional<std::string> diagnosticKnown(const json::value* value, const std::array<std::string_view, N>& known) { return diagnosticKnownString(value, known); }
  }
  bool isGatewayDeadlineResponse(const rt::Response& response, const rt::AbortSignal& requestSignal) {
    if (response.status() != 504) return false;
    const auto origin = refusalResponseOrigins.get(response);
    return origin == "gateway_deadline" || (!origin && requestSignal.aborted());
  }
  std::string formatErrorSnippet(const rt::Reason& error, double maxLen) { return formatErrorSnippet(std::string_view(rt::isError(error) ? rt::errorMessage(error).value_or("") : rt::reasonToString(error)), maxLen); }
  std::string formatErrorSnippet(const json::value& error, double maxLen) { return formatErrorSnippet(std::string_view(rt::toString(error)), maxLen); }
  std::string formatErrorSnippet(const json::value* error, double maxLen) { return formatErrorSnippet(std::string_view(rt::toString(error)), maxLen); }
  std::string formatErrorSnippet(std::string_view error, double maxLen) {
    const auto trimmed = rt::trim(error);
    if (trimmed.empty() || static_cast<double>(rt::length(trimmed)) <= maxLen) return trimmed;
    return rt::concat(rt::slice(trimmed, 0, maxLen), "...");
  }
  void logRedactedUpstreamError(const std::string& label, const rt::Reason& error) { rt::console::error(label, redactedUpstreamErrorDiagnostic(error)); }
  std::optional<std::string> normalizeProviderRequestId(std::string_view value) {
    const auto requestId = rt::trim(value);
    if (requestId.empty() || static_cast<double>(rt::length(requestId)) > MAX_PROVIDER_REQUEST_ID_CHARS) return std::nullopt;
    for (const auto code : rt::codePoints(requestId)) if (code < 0x20 || code == 0x7f) return std::nullopt;
    return requestId;
  }
  std::optional<std::string> normalizeProviderRequestId(const json::value* value) { return value && value->is_string() ? normalizeProviderRequestId(std::string_view(value->as_string())) : std::nullopt; }
  std::optional<std::string> normalizeProviderRequestId(const json::value& value) { return normalizeProviderRequestId(&value); }
  std::optional<std::string> providerRequestIdFromResponse(const rt::Response& response) {
    auto id = response.headers().get("X-Request-Id"); if (!id) id = response.headers().get("X-Api-Request-Id"); if (!id) id = response.headers().get("X-Oneapi-Request-Id");
    return id ? normalizeProviderRequestId(std::string_view(*id)) : std::nullopt;
  }
  rt::Response toCodexErrorResponse(const rt::Reason& error, const std::optional<std::string>& provider) {
    std::optional<rt::Response> response;
    if (error) try { std::rethrow_exception(error); }
    catch (const ApiKeyQuotaDispatchError& value) { response = quotaResponse(value, false); }
    catch (const ca::CodexError& value) { response = codexUpstreamErrorResponse(value); }
    catch (...) {}
    if (!response) response = unreachableUpstreamErrorResponse(error, provider);
    return withUpstreamProviderHeader(*response, provider);
  }
  rt::Response toPreHeaderErrorResponse(const rt::Reason& error, const std::string& terminalType, const std::optional<std::string>& provider) {
    if (terminalType == "cancelled") return withUpstreamProviderHeader(http::openaiError(499, "Request was cancelled.", "request_cancelled", {.type = "server_error", .param = std::optional<std::string>{}}), provider);
    const auto* codexError = ca::codexErrorOf(error);
    if (terminalType == "deadline" && !(codexError && codexError->code == "gateway_timeout")) return withUpstreamProviderHeader(http::openaiError(504, "Upstream request exceeded the gateway deadline.", "gateway_timeout", {.type = "server_error", .param = std::optional<std::string>{}}), provider);
    return toCodexErrorResponse(error, provider);
  }
  rt::Response toCerebrasErrorResponse(const rt::Reason& error) { return directErrorResponse<ce::CerebrasError>(error, "cerebras"); }
  rt::Headers cerebrasResponseHeaders(const std::optional<std::string>& providerRequestId, const std::optional<std::string>& warning) {
    rt::Headers result{{"x-uos-upstream", "cerebras"}};
    if (providerRequestId && !providerRequestId->empty()) result.set("x-uos-provider-request-id", *providerRequestId);
    if (warning && !warning->empty()) result.set("x-uos-warning", *warning);
    return result;
  }
  bool isAnswerBearingCompletion(const CompletionAnswerBearingOutput& output) { return !output.text.empty() || (output.refusal && !output.refusal->empty()) || output.toolCallCount > 0; }
  bool chatCompletionHasAnswerBearingOutput(const json::object& completion) { return chatFieldsHaveAnswerBearingOutput(completion, "message"); }
  bool chatChunkHasAnswerBearingOutput(const json::object& chunk) { return chatFieldsHaveAnswerBearingOutput(chunk, "delta"); }
  rt::Response streamCerebrasChatCompletion(const json::object& completion, bool includeUsage, const rt::Headers& headers) {
    auto id = utils::getString(rt::get(completion, "id"));
    if (!id) id = "chatcmpl_" + rt::replaceAll(rt::crypto::randomUUID(), "-", "");
    const auto created = rt::getNumber(completion, "created").value_or(rt::Math::floor(rt::dateNow() / 1000));
    const auto model = utils::getString(rt::get(completion, "model")).value_or(std::string(ce::CEREBRAS_GPT_OSS_120B_MODEL));
    std::string events;
    const auto appendEvent = [&](json::object value) { events += "data: " + rt::jsonStringify(value) + "\n\n"; };
    if (const auto* choices = rt::getArray(completion, "choices")) for (std::size_t choiceIndex = 0; choiceIndex < choices->size(); ++choiceIndex) {
      const auto& choice = (*choices)[choiceIndex]; if (!choice.is_object()) continue;
      const double index = rt::getNumber(choice, "index").value_or(static_cast<double>(choiceIndex));
      const auto* message = rt::getObject(choice, "message");
      const auto delta = cerebrasChatDelta(message ? *message : json::object{});
      appendEvent({{"id", *id}, {"object", "chat.completion.chunk"}, {"created", rt::makeNumber(created)}, {"model", model}, {"choices", json::array{json::object{{"index", rt::makeNumber(index)}, {"delta", delta}, {"finish_reason", nullptr}}}}});
      appendEvent({{"id", *id}, {"object", "chat.completion.chunk"}, {"created", rt::makeNumber(created)}, {"model", model}, {"choices", json::array{json::object{{"index", rt::makeNumber(index)}, {"delta", json::object{}}, {"finish_reason", utils::getString(rt::get(choice, "finish_reason")).value_or("stop")}}}}});
    }
    if (includeUsage) if (const auto* usage = rt::get(completion, "usage")) appendEvent({{"id", *id}, {"object", "chat.completion.chunk"}, {"created", rt::makeNumber(created)}, {"model", model}, {"choices", json::array{}}, {"usage", *usage}});
    events += "data: [DONE]\n\n";
    auto responseHeaders = headers; responseHeaders.set("Content-Type", "text/event-stream"); responseHeaders.set("Cache-Control", "no-cache");
    return rt::Response(events, {.status = 200, .headers = responseHeaders});
  }
  rt::Task<rt::Response> toOpenAiUpstreamErrorResponse(rt::Response upstream, std::string provider, rt::AbortSignal signal) {
    auto capture = captureErrorBody(upstream, signal, "Upstream error body captured");
    const auto captured = co_await std::move(capture);
    const auto text = captured.complete ? rt::decodeUtf8(captured.bytes) : std::string{};
    const auto details = captured.complete ? parseUpstreamErrorDetails(text, upstream.statusText()) : UpstreamErrorDetails{"Upstream returned an oversized or incomplete error response."};
    rt::Headers headers{{"x-uos-upstream", provider}};
    for (const auto key : {"x-uos-debug-scenario", "x-uos-warning", "Retry-After"}) if (const auto value = upstream.headers().get(key); value && !value->empty()) headers.set(key, *value);
    bool hasAuthWarning = false;
    if (const auto warning = upstream.headers().get("x-uos-warning")) for (const auto& value : rt::split(*warning, ",")) if (rt::trim(value) == ca::CODEX_AUTH_REAUTH_WARNING) hasAuthWarning = true;
    const auto message = hasAuthWarning && !rt::includes(text, ca::CODEX_AUTH_REAUTH_MESSAGE) ? details.message + " " + std::string(ca::CODEX_AUTH_REAUTH_MESSAGE) : details.message;
    http::OpenaiErrorOptions options{.type = hasAuthWarning && upstream.status() >= 500 ? "server_error" : upstreamStatusToErrorType(upstream.status(), details.type), .param = details.param, .headers = headers};
    co_return http::openaiError(upstream.status(), message, details.code.value_or("upstream_error"), options);
  }
  void cancelResponseBody(rt::Response response) {
    try { if (auto body = response.body()) { auto cancellation = body->cancel(); rt::spawn(std::move(cancellation), rt::ignoreRejection); } } catch (...) {}
  }
  rt::Task<rt::Response> toCerebrasUpstreamErrorResponse(rt::Response upstream, std::optional<rt::AbortSignal> signal) {
    UpstreamErrorDetail detail;
    try {
      auto capture = captureErrorBody(upstream, signal, "Cerebras upstream error body"); const auto captured = co_await std::move(capture);
      if (captured.complete && !captured.bytes.empty()) try { detail = parseUpstreamErrorDetail(rt::jsonParse(rt::decodeUtf8(captured.bytes))); } catch (...) {}
    } catch (...) {}
    cancelResponseBody(upstream);
    auto headers = cerebrasResponseHeaders(ce::getCerebrasProviderRequestId(upstream));
    if (const auto retry = upstream.headers().get("Retry-After"); retry && !retry->empty()) headers.set("Retry-After", *retry);
    if (upstream.status() == 429) for (const auto header : provider::cerebras_rate_limits::CEREBRAS_RATE_LIMIT_HEADERS) if (const auto value = upstream.headers().get(header)) headers.set(header, *value);
    co_return http::openaiError(upstream.status(), detail.message.value_or("Cerebras upstream returned an error."), detail.code.value_or("cerebras_upstream_error"), {.type = upstream.status() == 408 ? "server_error" : upstreamStatusToErrorType(upstream.status()), .headers = headers});
  }
  rt::Response toDeepSeekErrorResponse(const rt::Reason& error) { return directErrorResponse<ds::DeepSeekError>(error, "deepseek"); }
  rt::Headers deepseekResponseHeaders(const std::optional<std::string>& providerRequestId) { rt::Headers result{{"x-uos-upstream", "deepseek"}}; if (providerRequestId && !providerRequestId->empty()) result.set("x-uos-provider-request-id", *providerRequestId); return result; }
  rt::Headers lithosResponseHeaders(const std::optional<std::string>& providerRequestId) { rt::Headers result{{"x-uos-upstream", "lithos"}}; if (providerRequestId && !providerRequestId->empty()) result.set("x-uos-provider-request-id", *providerRequestId); return result; }
  rt::Headers openRouterResponseHeaders(const std::optional<std::string>& providerRequestId) { rt::Headers result{{"x-uos-upstream", "openrouter"}}; if (providerRequestId && !providerRequestId->empty()) result.set("x-uos-provider-request-id", *providerRequestId); return result; }
  rt::Response toLithosErrorResponse(const rt::Reason& error) { const auto response = directErrorResponse<li::LithosError>(error, "lithos"); return response.status() == 504 ? markGatewayDeadlineResponse(response) : response; }
  rt::Task<rt::Response> toLithosUpstreamErrorResponse(rt::Response upstream, std::optional<rt::AbortSignal> signal, std::optional<json::object> bodyDiagnostic) {
    LithosUpstreamErrorDetail detail;
    try { auto capture = captureErrorBody(upstream, signal, "LithosAI upstream error body"); const auto captured = co_await std::move(capture); if (captured.complete && !captured.bytes.empty()) detail = parseLithosUpstreamErrorDetail(rt::decodeUtf8(captured.bytes)); } catch (...) {}
    cancelResponseBody(upstream);
    auto headers = lithosResponseHeaders(li::getLithosProviderRequestId(upstream));
    if (upstream.status() == 429) {
      for (const auto header : li::LITHOS_RATE_LIMIT_HEADERS) if (const auto value = upstream.headers().get(header)) headers.set(header, *value);
      if (const auto retry = upstream.headers().get("retry-after-ms")) headers.set("retry-after-ms", *retry);
    }
    if (const auto retry = upstream.headers().get("Retry-After"); retry && !retry->empty()) headers.set("Retry-After", *retry);
    if (const auto retry = upstream.headers().get("x-should-retry")) headers.set("x-should-retry", *retry);
    else if (upstream.status() == 400 || upstream.status() == 401 || upstream.status() == 402 || upstream.status() == 404) headers.set("x-should-retry", "false");
    const auto code = lithosUpstreamErrorCode(upstream.status(), detail);
    rt::console::warn("[ai.ubq.fi] lithos_upstream_error", rt::jsonStringify(json::object{{"status", upstream.status()}, {"code", code}, {"message", rt::orNull(detail.message).toJson().value()}, {"request", bodyDiagnostic ? json::value(*bodyDiagnostic) : json::value(nullptr)}}));
    const auto response = http::openaiError(upstream.status(), detail.message.value_or("LithosAI upstream returned an error."), code, {.type = lithosUpstreamErrorType(upstream.status()), .headers = headers});
    co_return upstream.status() == 504 ? markUpstreamRefusalResponse(response) : response;
  }
  json::object deepSeekChatBodyDiagnostic(const json::object& body) {
    const auto* rawMessages = rt::getArray(body, "messages"); const json::array empty;
    const auto& messages = rawMessages ? *rawMessages : empty;
    double lastUser = -1;
    for (std::size_t index = 0; index < messages.size(); ++index) if (utils::isRecord(messages[index]) && utils::getString(rt::get(messages[index], "role")) == "user") lastUser = static_cast<double>(index);
    const std::size_t omitted = messages.size() > 16 ? messages.size() - 16 : 0;
    std::vector<std::string_view> knownModels(ds::DEEPSEEK_OFFICIAL_MODEL_IDS.begin(), ds::DEEPSEEK_OFFICIAL_MODEL_IDS.end()); knownModels.insert(knownModels.end(), li::LITHOS_MODEL_IDS.begin(), li::LITHOS_MODEL_IDS.end());
    const auto* tools = rt::getArray(body, "tools"); const auto* stream = rt::get(body, "stream");
    const auto tokens = rt::getNumber(body, "max_tokens");
    json::array digest;
    for (std::size_t index = omitted; index < messages.size(); ++index) {
      const auto& message = messages[index]; const auto* calls = rt::getArray(message, "tool_calls");
      digest.push_back(json::object{{"index", rt::makeNumber(static_cast<double>(index))}, {"role", rt::orNull(diagnosticKnown(rt::get(message, "role"), std::array<std::string_view, 6>{"system", "developer", "user", "assistant", "tool", "function"})).toJson().value()}, {"reasoning", diagnosticReasoningLabel(rt::get(message, "reasoning_content"))}, {"tool_calls", rt::makeNumber(calls ? static_cast<double>(calls->size()) : 0)}, {"content", diagnosticContentLabel(rt::get(message, "content"))}, {"after_last_user", static_cast<double>(index) > lastUser}});
    }
    return {{"model", rt::orNull(diagnosticKnownString(rt::get(body, "model"), knownModels)).toJson().value()}, {"reasoning_effort", rt::orNull(diagnosticKnown(rt::get(body, "reasoning_effort"), std::array<std::string_view, 8>{"none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra"})).toJson().value()}, {"stream", stream && stream->is_bool() && stream->as_bool()}, {"tools", rt::makeNumber(tools ? static_cast<double>(tools->size()) : 0)}, {"tool_choice", rt::orNull(diagnosticKnown(rt::get(body, "tool_choice"), std::array<std::string_view, 3>{"auto", "none", "required"})).toJson().value()}, {"max_tokens", rt::orNull(tokens && rt::Number::isSafeInteger(*tokens) ? tokens : std::nullopt).toJson().value()}, {"message_count", rt::makeNumber(static_cast<double>(messages.size()))}, {"last_user_index", rt::makeNumber(lastUser)}, {"omitted_message_count", rt::makeNumber(static_cast<double>(omitted))}, {"messages", std::move(digest)}};
  }
  rt::Task<rt::Response> toDeepSeekUpstreamErrorResponse(rt::Response upstream, std::optional<rt::AbortSignal> signal, std::optional<json::object> bodyDiagnostic) {
    UpstreamErrorDetail detail;
    try { auto capture = captureErrorBody(upstream, signal, "DeepSeek upstream error body"); const auto captured = co_await std::move(capture); if (captured.complete && !captured.bytes.empty()) try { detail = parseUpstreamErrorDetail(rt::jsonParse(rt::decodeUtf8(captured.bytes))); } catch (...) {} } catch (...) {}
    cancelResponseBody(upstream);
    auto headers = deepseekResponseHeaders(ds::getDeepSeekProviderRequestId(upstream));
    if (const auto retry = upstream.headers().get("Retry-After"); retry && !retry->empty()) headers.set("Retry-After", *retry);
    rt::console::warn("[ai.ubq.fi] deepseek_upstream_error", rt::jsonStringify(json::object{{"status", upstream.status()}, {"code", rt::orNull(detail.code).toJson().value()}, {"message", rt::orNull(detail.message).toJson().value()}, {"request", bodyDiagnostic ? json::value(*bodyDiagnostic) : json::value(nullptr)}}));
    co_return http::openaiError(upstream.status(), detail.message.value_or("DeepSeek upstream returned an error."), detail.code.value_or("deepseek_upstream_error"), {.type = upstream.status() == 408 ? "server_error" : upstreamStatusToErrorType(upstream.status()), .headers = headers});
  }
  rt::Response apiKeyQuotaDispatchErrorResponse(const ApiKeyQuotaDispatchError& error) { return quotaResponse(error, true); }
}
