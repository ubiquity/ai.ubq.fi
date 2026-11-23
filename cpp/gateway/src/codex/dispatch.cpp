// Codex dispatch primitives and request preparation, split out of src/codex.ts.
#include "uos/codex/dispatch.hpp"

#include "uos/bounded_response_body.hpp"
#include "uos/codex/auth_refresh.hpp"
#include "uos/codex/capacity_routing.hpp"
#include "uos/config.hpp"
#include "uos/inference_deadline.hpp"
#include "uos/kv.hpp"
#include "uos/models/codex_models_availability.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/provider/health.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/url.hpp"
#include "uos/upstream_auth.hpp"
#include "uos/utils.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

namespace uos::codex::dispatch {
  namespace {
    using auth::CodexError;
    using ::uos::types::CodexAuthState;

    // `reason instanceof E`
    template <class E> bool reasonIs(const rt::Reason& reason) noexcept {
      if (!reason) return false;
      try {
        std::rethrow_exception(reason);
      } catch (const E&) {
        return true;
      } catch (...) {
      }
      return false;
    }

    // `reason === null || reason === undefined` for a caught or abort reason.
    bool isNullishReason(const rt::Reason& reason) {
      if (!reason) return true;
      const std::optional<json::value> thrown = rt::thrownValue(reason);
      return thrown.has_value() && thrown->is_null();
    }

    // `reason instanceof Error && reason.name === name`
    bool errorNamed(const rt::Reason& reason, std::string_view name) {
      if (!rt::isError(reason)) return false;
      const std::optional<std::string> errorName = rt::errorName(reason);
      return errorName.has_value() && *errorName == name;
    }

    std::function<double()> dateNowFunction() { return [] { return rt::dateNow(); }; }
  }

  std::vector<std::string> codexModelsBaseUrls(const std::optional<std::string>& clientVersion) {
    // Strip trailing slashes without the backtracking `\/+$` pattern: the scan is
    // linear and produces exactly what `replace(/\/+$/, "")` produced.
    const std::string& configured = ::uos::config::config.codexBaseUrl;
    std::size_t end = configured.size();
    while (end > 0 && configured[end - 1] == '/') end -= 1;
    const std::string base = configured.substr(0, end);
    rt::OrderedSet<std::string> urls;

    // Prefer the Codex-specific models endpoint when available. It requires `client_version`.
    if (rt::endsWith(base, "/codex")) {
      rt::URL codexUrl(base + "/models");
      if (rt::truthy(clientVersion)) codexUrl.searchParams().set("client_version", *clientVersion);
      urls.add(codexUrl.href());
    } else {
      urls.add(base + "/models");
    }

    std::vector<std::string> list;
    for (const std::string& url : urls) list.push_back(url);
    return list;
  }

  rt::Task<rt::Response> fetchCodexModelsWithAuth(CodexAuthState auth, std::string url, std::optional<std::string> clientVersion) {
    return fetchCodexModelsWithAuth(std::move(auth), std::move(url), std::move(clientVersion), std::nullopt, std::nullopt);
  }

  rt::Task<rt::Response> fetchCodexModelsWithAuth(CodexAuthState auth, std::string url, std::optional<std::string> clientVersion,
                                                  std::optional<std::string> ifNoneMatch, std::optional<rt::AbortSignal> signal) {
    rt::Headers headers;
    headers.set("Authorization", "Bearer " + auth.access_token);
    headers.set("ChatGPT-Account-ID", auth.account_id);
    headers.set("originator", auth::CODEX_ORIGINATOR);
    headers.set("user-agent", "codex_cli_rs/" + clientVersion.value_or(std::string(auth::CODEX_CLIENT_VERSION)) + " (ai.ubq.fi)");
    headers.set("Accept", "application/json");
    if (rt::truthy(ifNoneMatch)) headers.set("If-None-Match", *ifNoneMatch);

    rt::Reason failure;
    try {
      rt::RequestInit init;
      init.method = "GET";
      init.headers = headers;
      init.signal = signal;
      init.redirect = "manual";
      rt::Promise<rt::Response> fetching(rt::fetch(rt::RequestInfo(url), std::move(init)));
      rt::Response response = co_await fetching.wait();
      co_return response;
    } catch (...) {
      failure = std::current_exception();
    }
    throw CodexError("Codex upstream request failed: upstream unreachable.", "codex_upstream_unreachable", 502, failure);
  }

  std::vector<CodexAuthState> orderCodexAuthAccounts(const std::vector<CodexAuthState>& accounts, double startIndex) {
    if (accounts.empty()) return {};
    const double length = static_cast<double>(accounts.size());
    const double normalizedStart = rt::mod(rt::mod(rt::Math::trunc(startIndex), length) + length, length);
    std::vector<CodexAuthState> ordered;
    ordered.reserve(accounts.size());
    for (std::size_t offset = 0; offset < accounts.size(); ++offset) {
      const CodexAuthState* account = rt::element(accounts, rt::mod(normalizedStart + static_cast<double>(offset), length));
      // Only a non-finite startIndex reads past the accounts (JS yields `undefined` entries,
      // which this vector cannot hold).
      if (account == nullptr) throw rt::TypeError("orderCodexAuthAccounts: a non-finite startIndex selects no account");
      ordered.push_back(*account);
    }
    return ordered;
  }

  std::vector<auth::CodexAuthAccountEntry> randomizedAuthEntries(const auth::CodexAuthPoolEntry& poolEntry) {
    std::array<std::uint8_t, 1> entropy{};
    rt::crypto::getRandomValues(std::span<std::uint8_t>(entropy));
    std::vector<auth::CodexAuthAccountEntry> entries;
    for (const CodexAuthState& account : orderCodexAuthAccounts(poolEntry.pool.accounts, static_cast<double>(entropy[0])))
      entries.push_back(auth::CodexAuthAccountEntry{poolEntry, account, std::nullopt});
    return entries;
  }

  void cancelResponseBody(const rt::Response& response) {
    try {
      std::optional<rt::ByteStream> body = response.body();
      if (body) rt::spawn(body->cancel(), rt::ignoreRejection);
    } catch (...) {
      // Best effort before retrying another account.
    }
  }

  rt::Task<void> recordCodexResponseHealth(std::string accountId, rt::Response response) {
    return recordCodexResponseHealth(std::move(accountId), std::move(response), std::nullopt, std::nullopt);
  }

  rt::Task<void> recordCodexResponseHealth(std::string accountId, rt::Response response, std::optional<CodexAuthState> auth) {
    return recordCodexResponseHealth(std::move(accountId), std::move(response), std::move(auth), std::nullopt);
  }

  rt::Task<void> recordCodexResponseHealth(std::string accountId, rt::Response response, std::optional<CodexAuthState> auth,
                                           std::optional<std::string> successfulResponseEvent) {
    std::optional<std::string> providerRequestId = response.headers().get("X-Request-Id");
    if (!providerRequestId) providerRequestId = response.headers().get("X-Api-Request-Id");
    if (!providerRequestId) providerRequestId = response.headers().get("X-Oneapi-Request-Id");
    const double status = response.status();
    if (status == 401 || (status == 403 && auth.has_value() && auth::accessTokenExpired(*auth))) {
      rt::Promise<void> recording = ::uos::provider::health::recordCodexProviderHealth(accountId, "auth_invalid", status, dateNowFunction(), providerRequestId);
      co_await recording.wait();
    } else if (status == 429) {
      // An exhausted account is the strongest capacity observation there is.
      ::uos::provider::capacity_events::triggerProviderCapacitySample();
      rt::Promise<void> recording = ::uos::provider::health::recordCodexProviderHealth(accountId, "quota_exhausted", status, dateNowFunction(), providerRequestId);
      co_await recording.wait();
    } else if (status >= 500) {
      ::uos::provider::capacity_events::ProviderCapacityDowntimeEventInput downtime{.failure_kind = "upstream_error", .status = status, .observed_at_ms = rt::dateNow()};
      rt::spawn(::uos::provider::capacity_events::recordProviderCapacityDowntimeEvent(std::move(downtime)));
      // An upstream failure is a capacity observation: sample the bucket now.
      ::uos::provider::capacity_events::triggerProviderCapacitySample();
      rt::Promise<void> recording = ::uos::provider::health::recordCodexProviderHealth(accountId, "upstream_error", status, dateNowFunction(), providerRequestId);
      co_await recording.wait();
    } else if (response.ok() && successfulResponseEvent.has_value()) {
      // A served request is a capacity observation too: it keeps the bucket's
      // history current on a healthy gateway, still one probe per bucket.
      ::uos::provider::capacity_events::triggerProviderCapacitySample();
      rt::Promise<void> recording =
        ::uos::provider::health::recordCodexProviderHealth(accountId, *successfulResponseEvent, status, dateNowFunction(), providerRequestId);
      co_await recording.wait();
    } else if (!response.ok()) {
      rt::Promise<void> recording = ::uos::provider::health::recordCodexProviderHealth(accountId, "reachable", status, dateNowFunction(), providerRequestId);
      co_await recording.wait();
    }
  }

  rt::Task<void> recordCodexThrownHealth(std::string accountId, rt::Reason error) {
    const CodexError* codexError = auth::codexErrorOf(error);
    if (codexError != nullptr &&
        (dynamic_cast<const auth::CodexAuthFollowerStaleError*>(codexError) ||
         codexError->code == "codex_auth_refresh_failed" || codexError->code == "refresh_token_reused" || codexError->code == "codex_auth_refresh_unreachable" ||
         codexError->code == "codex_auth_owner_unavailable" || codexError->code == "codex_auth_owner_conflict")) {
      co_return;
    }
    const bool isProviderTransportFailure = codexError != nullptr && (codexError->code == "gateway_timeout" || codexError->code == "codex_upstream_unreachable");
    rt::Promise<void> recording = ::uos::provider::health::recordCodexProviderHealth(accountId, "upstream_error", std::nullopt);
    co_await recording.wait();
    if (isProviderTransportFailure) {
      ::uos::provider::capacity_events::ProviderCapacityDowntimeEventInput downtime{
        .failure_kind = "unreachable",
        .status = codexError->status >= 500 && codexError->status <= 599 ? std::optional<double>(codexError->status) : std::nullopt,
        .observed_at_ms = rt::dateNow()};
      rt::spawn(::uos::provider::capacity_events::recordProviderCapacityDowntimeEvent(std::move(downtime)));
      ::uos::provider::capacity_events::triggerProviderCapacitySample();
    }
  }

  rt::Task<rt::Response> fetchCodexResponseWithAuth(CodexAuthState auth, std::string url, std::string serializedBody, rt::Headers baseHeaders) {
    return fetchCodexResponseWithAuth(std::move(auth), std::move(url), std::move(serializedBody), std::move(baseHeaders), std::nullopt, {}, {});
  }

  rt::Task<rt::Response> fetchCodexResponseWithAuth(CodexAuthState auth, std::string url, std::string serializedBody, rt::Headers baseHeaders,
                                                    std::optional<rt::AbortSignal> signal) {
    return fetchCodexResponseWithAuth(std::move(auth), std::move(url), std::move(serializedBody), std::move(baseHeaders), std::move(signal), {}, {});
  }

  rt::Task<rt::Response> fetchCodexResponseWithAuth(CodexAuthState auth, std::string url, std::string serializedBody, rt::Headers baseHeaders,
                                                    std::optional<rt::AbortSignal> signal, std::function<rt::Task<void>()> beforeTransport,
                                                    std::function<void()> onDispatch) {
    rt::Headers headers(baseHeaders);
    headers.set("Authorization", "Bearer " + auth.access_token);
    headers.set("ChatGPT-Account-ID", auth.account_id);
    const rt::AbortController deadline;
    const rt::Timeout deadlineTimer = rt::setTimeout(
      [deadline] { deadline.abort(rt::makeReason(rt::DOMException("Codex response headers timed out.", "TimeoutError"))); },
      ::uos::inference_deadline::BUFFERED_INFERENCE_DEADLINE_MS);
    const auto clearDeadline = rt::scopeExit([deadlineTimer] { rt::clearTimeout(deadlineTimer); });
    const rt::AbortSignal transportSignal = signal ? rt::AbortSignal::any({*signal, deadline.signal()}) : deadline.signal();
    rt::Reason failure;
    try {
      if (beforeTransport) {
        rt::Promise<void> preparing(beforeTransport());
        co_await preparing.wait();
      } else {
        co_await rt::microtask();
      }
      if (transportSignal.aborted()) {
        if (isNullishReason(transportSignal.reason())) throw rt::DOMException("The request was aborted.", "AbortError");
        rt::rethrow(transportSignal.reason());
      }
      if (onDispatch) onDispatch();
      rt::RequestInit init;
      init.method = "POST";
      init.headers = headers;
      init.body = rt::BodyInit(std::move(serializedBody));
      init.signal = transportSignal;
      init.redirect = "manual";
      rt::Promise<rt::Response> fetching(rt::fetch(rt::RequestInfo(url), std::move(init)));
      rt::Response response = co_await fetching.wait();
      co_return response;
    } catch (...) {
      failure = std::current_exception();
    }
    if (reasonIs<::uos::api_key_policy::ApiKeyQuotaDispatchError>(failure)) std::rethrow_exception(failure);
    if (reasonIs<CodexBankedResetRetryFenceError>(failure) || reasonIs<CodexActiveAccountFenceError>(failure)) std::rethrow_exception(failure);
    const bool timedOut = deadline.signal().aborted() || errorNamed(failure, "TimeoutError") ||
                          (signal.has_value() && signal->aborted() && errorNamed(signal->reason(), "TimeoutError"));
    if (timedOut) {
      throw CodexError("Codex upstream exceeded the gateway deadline before response headers were received.", "gateway_timeout", 504, failure);
    }
    if (signal.has_value() && signal->aborted()) {
      if (isNullishReason(signal->reason())) std::rethrow_exception(failure);
      rt::rethrow(signal->reason());
    }
    throw CodexError("Codex upstream request failed: upstream unreachable.", "codex_upstream_unreachable", 502, failure);
  }

  namespace {
    // `status: 401 | 404 | 429 | 503`
    std::string routingErrorType(double status) {
      if (status == 429) return "rate_limit_error";
      if (status >= 500) return "server_error";
      return "invalid_request_error";
    }
  }

  double codexRetryAfterSeconds(double retryAtMs) { return codexRetryAfterSeconds(retryAtMs, rt::dateNow()); }

  double codexRetryAfterSeconds(double retryAtMs, double nowMs) {
    return rt::Math::min(CODEX_CLIENT_RETRY_AFTER_MAX_SECONDS, rt::Math::max(1.0, rt::Math::ceil((retryAtMs - nowMs) / 1000)));
  }

  rt::Response routingErrorResponse(double status, const std::string& message, const std::string& code, std::optional<double> retryAtMs) {
    rt::Headers headers{{"Content-Type", "application/json"}};
    if ((status == 429 || status == 503) && retryAtMs.has_value()) {
      headers.set("Retry-After", rt::numberToString(codexRetryAfterSeconds(*retryAtMs)));
    }
    json::object error;
    error["message"] = message;
    error["type"] = routingErrorType(status);
    error["code"] = code;
    error["param"] = nullptr;
    json::object payload;
    payload["error"] = std::move(error);
    rt::ResponseInit init;
    init.status = status;
    init.headers = headers;
    const rt::Response response(rt::BodyInit(rt::jsonStringify(payload)), init);
    if (code == auth::CODEX_QUOTA_BLOCKED_ERROR_CODE || code == auth::CODEX_SUBSCRIPTIONS_UNAVAILABLE_ERROR_CODE || code == auth::CODEX_UPSTREAM_DEGRADED_ERROR_CODE) {
      auth::codexRoutingErrors.set(response, code);
    }
    return response;
  }

  rt::Response upstreamTimeoutCircuitResponse(std::optional<double> retryAtMs) {
    return routingErrorResponse(503, "Codex upstream is temporarily unavailable after response-header timeouts; retry later.",
                                std::string(auth::CODEX_UPSTREAM_DEGRADED_ERROR_CODE), retryAtMs);
  }

  rt::Response codexModelUnavailableResponse(const std::string& model, const std::optional<std::string>& detail) {
    const std::string base = "The model '" + model + "' does not exist or is not available through this gateway. Use /v1/models for supported models.";
    return routingErrorResponse(404, detail.has_value() ? base + " Upstream detail: " + *detail : base, "model_not_found");
  }

  namespace {
    using Dispatch = std::optional<::uos::api_key_policy::ApiKeyProviderDispatch>;

    struct DispatchGeneration {
      rt::Promise<Dispatch> claim = rt::Promise<Dispatch>::resolve(std::nullopt);
      Dispatch dispatch;
      bool transportStarted = false;
      std::optional<rt::Promise<void>> cancellation;
    };

    struct DispatchCoordinatorState {
      FetchCodexResponsesOptions::BeforeDispatch beforeDispatch;
      std::shared_ptr<DispatchGeneration> current;
    };
  }

  CodexProviderDispatchCoordinator createCodexProviderDispatchCoordinator(FetchCodexResponsesOptions::BeforeDispatch beforeDispatch) {
    const auto state = std::make_shared<DispatchCoordinatorState>();
    state->beforeDispatch = std::move(beforeDispatch);
    CodexProviderDispatchCoordinator coordinator;
    coordinator.claim = [state]() -> rt::Task<void> {
      if (!state->current || state->current->transportStarted) {
        const auto generation = std::make_shared<DispatchGeneration>();
        rt::Promise<Dispatch> claimed = rt::Promise<Dispatch>::resolve(std::nullopt);
        // A promise-returning hook hands back a promise (an object); the void arm hands back nothing.
        if (const auto* promised = std::get_if<1>(&state->beforeDispatch)) {
          claimed = rt::Promise<Dispatch>((*promised)());
        } else if (const auto* plain = std::get_if<2>(&state->beforeDispatch)) {
          (*plain)();
          claimed = rt::Promise<Dispatch>::resolve(std::nullopt);
        }
        generation->claim = claimed.then([generation](const Dispatch& dispatch) {
          generation->dispatch = dispatch;
          return dispatch;
        });
        state->current = generation;
      }
      return state->current->claim.then([](const Dispatch&) {}).wait();
    };
    coordinator.markTransportStarted = [state]() {
      if (!state->current || state->current->transportStarted) return;
      if (state->current->dispatch) state->current->dispatch->markTransportStarted();
      state->current->transportStarted = true;
    };
    coordinator.cancelBeforeTransport = [state]() -> rt::Task<void> {
      if (!state->current || state->current->transportStarted || !state->current->dispatch) return rt::Promise<void>::resolve().wait();
      if (!state->current->cancellation) state->current->cancellation = rt::Promise<void>(state->current->dispatch->cancelBeforeTransport());
      return state->current->cancellation->wait();
    };
    return coordinator;
  }

  namespace {
    constexpr std::string_view CODEX_PROMPT_CACHE_OPTIONS_IGNORED_WARNING = "prompt_cache_options_ignored";
    constexpr std::string_view CODEX_PROMPT_CACHE_RETENTION_IGNORED_WARNING = "prompt_cache_retention_ignored";
    constexpr std::string_view CODEX_PROMPT_CACHE_BREAKPOINT_IGNORED_WARNING = "prompt_cache_breakpoint_ignored";
    constexpr std::string_view CODEX_MAX_OUTPUT_TOKENS_IGNORED_WARNING = "max_output_tokens_ignored";

    rt::Task<std::string> deterministicCodexSessionIdentity(std::string cacheScope, std::string promptCacheKey) {
      std::string material = "uos-codex-prompt-cache-session-v2";
      material += '\0';
      material = rt::concat(material, cacheScope);
      material += '\0';
      material = rt::concat(material, promptCacheKey);
      rt::Promise<std::string> hashing(::uos::utils::sha256Hex(std::move(material)));
      const std::string digest = co_await hashing.wait();
      const char digit = digest[16];
      const int nibble = digit <= '9' ? digit - '0' : digit - 'a' + 10;
      const char variant = "0123456789abcdef"[(nibble & 0b0011) | 0b1000];
      co_return rt::slice(digest, 0, 8) + "-" + rt::slice(digest, 8, 12) + "-8" + rt::slice(digest, 13, 16) + "-" + variant + rt::slice(digest, 17, 20) + "-" +
        rt::slice(digest, 20, 32);
    }

    // `{ value, removed }`: the replacement value when a breakpoint was removed, else std::nullopt (the
    // input itself). Runs on an explicit stack: request bodies are untrusted nesting.
    std::optional<json::value> stripCodexPromptCacheBreakpoints(const json::value& root) {
      static constexpr std::array<std::string_view, 2> KEYS{"content", "output"};
      struct Frame {
        const json::value* node;
        std::size_t position = 0;
        std::vector<std::optional<json::value>> elements{};
        std::optional<json::object> next{};
      };
      std::vector<Frame> stack;
      // Pushes a frame for an array or record and returns true; a primitive is returned unchanged.
      const auto open = [&stack](const json::value& value) {
        if (value.is_array()) {
          stack.push_back(Frame{&value});
          return true;
        }
        if (!::uos::utils::isRecord(value)) return false;
        Frame frame{&value};
        const json::object& object = value.get_object();
        if (rt::hasOwn(object, "prompt_cache_breakpoint")) {
          frame.next = rt::objectSpread(object);
          rt::objectDelete(*frame.next, "prompt_cache_breakpoint");
        }
        stack.push_back(std::move(frame));
        return true;
      };
      if (!open(root)) return std::nullopt;
      std::optional<json::value> carried;
      bool hasCarried = false;
      for (;;) {
        if (stack.size() > rt::kJsonMaxDepth) throw rt::RangeError("Maximum call stack size exceeded.");
        Frame& frame = stack.back();
        if (hasCarried) {
          hasCarried = false;
          if (frame.node->is_array()) {
            frame.elements.push_back(std::move(carried));
          } else if (carried.has_value()) {
            if (!frame.next) frame.next = rt::objectSpread(frame.node->get_object());
            rt::objectSet(*frame.next, KEYS[frame.position - 1], std::move(*carried));
          }
          carried.reset();
        }
        bool descended = false;
        if (frame.node->is_array()) {
          const json::array& array = frame.node->get_array();
          while (frame.elements.size() < array.size()) {
            const json::value& element = array[frame.elements.size()];
            if (open(element)) {
              descended = true;
              break;
            }
            frame.elements.push_back(std::nullopt);
          }
        } else {
          const json::object& object = frame.node->get_object();
          while (frame.position < KEYS.size()) {
            const std::string_view key = KEYS[frame.position];
            frame.position += 1;
            if (!rt::hasOwn(object, key)) continue;
            if (open(*rt::get(object, key))) {
              descended = true;
              break;
            }
          }
        }
        if (descended) continue;
        std::optional<json::value> replacement;
        Frame& finished = stack.back();
        if (finished.node->is_array()) {
          bool removed = false;
          for (const std::optional<json::value>& element : finished.elements)
            if (element.has_value()) removed = true;
          if (removed) {
            const json::array& array = finished.node->get_array();
            json::array next;
            for (std::size_t index = 0; index < array.size(); ++index) next.push_back(finished.elements[index] ? *finished.elements[index] : array[index]);
            replacement = json::value(std::move(next));
          }
        } else if (finished.next) {
          // `next` is set by exactly the branches that remove a breakpoint.
          replacement = json::value(std::move(*finished.next));
        }
        stack.pop_back();
        if (stack.empty()) return replacement;
        carried = std::move(replacement);
        hasCarried = true;
      }
    }

    /**
     * Request fields the gateway accepts from Codex CLI clients but never forwards
     * upstream, paired with the warning each one raises.
     */
    constexpr std::array<std::pair<std::string_view, std::string_view>, 4> IGNORED_CODEX_REQUEST_FIELDS{{
      {"prompt_cache_options", CODEX_PROMPT_CACHE_OPTIONS_IGNORED_WARNING},
      {"prompt_cache_retention", CODEX_PROMPT_CACHE_RETENTION_IGNORED_WARNING},
      {"max_output_tokens", CODEX_MAX_OUTPUT_TOKENS_IGNORED_WARNING},
      {"max_completion_tokens", CODEX_MAX_OUTPUT_TOKENS_IGNORED_WARNING},
    }};

    void stripIgnoredCodexRequestFields(json::object& prepared, std::vector<std::string>& warnings) {
      for (const auto& [field, warning] : IGNORED_CODEX_REQUEST_FIELDS) {
        if (!rt::hasOwn(prepared, field)) continue;
        rt::objectDelete(prepared, field);
        const std::string text(warning);
        if (!rt::includes(warnings, text)) warnings.push_back(text);
      }
    }

    // `{ ...value }` of a utils.isRecord value: an array spreads its elements under index keys.
    json::object spreadRecord(const json::value& value) {
      if (value.is_object()) return rt::objectSpread(value.get_object());
      json::object spread;
      const json::array& array = value.get_array();
      for (std::size_t index = 0; index < array.size(); ++index) rt::objectDefine(spread, std::to_string(index), array[index]);
      return spread;
    }
  }

  rt::Task<PreparedCodexSubscriptionRequest> prepareCodexSubscriptionRequest(json::value body, std::optional<std::string> cacheScope) {
    std::vector<std::string> warnings;
    json::value preparedBody = body;
    std::optional<std::string> promptCacheKey;

    if (::uos::utils::isRecord(body)) {
      json::object prepared = spreadRecord(body);
      const std::optional<std::string_view> key = rt::getString(rt::get(body, "prompt_cache_key"));
      promptCacheKey = key.has_value() && rt::length(rt::trim(*key)) > 0 ? std::optional<std::string>(std::string(*key)) : std::nullopt;
      stripIgnoredCodexRequestFields(prepared, warnings);
      if (rt::hasOwn(prepared, "input")) {
        std::optional<json::value> stripped = stripCodexPromptCacheBreakpoints(*rt::get(prepared, "input"));
        if (stripped.has_value()) {
          rt::objectSet(prepared, "input", std::move(*stripped));
          warnings.push_back(std::string(CODEX_PROMPT_CACHE_BREAKPOINT_IGNORED_WARNING));
        }
      }
      preparedBody = std::move(prepared);
    }

    std::optional<std::string> nativeSessionIdentity;
    if (promptCacheKey.has_value() && cacheScope.has_value() && !cacheScope->empty()) {
      rt::Promise<std::string> deriving(deterministicCodexSessionIdentity(*cacheScope, *promptCacheKey));
      nativeSessionIdentity = co_await deriving.wait();
    }
    PreparedCodexSubscriptionRequest result;
    result.serializedBody = rt::jsonStringify(preparedBody);
    result.body = std::move(preparedBody);
    result.conversationIdentity = nativeSessionIdentity.has_value() ? *nativeSessionIdentity : rt::crypto::randomUUID();
    result.nativeSessionIdentity = nativeSessionIdentity;
    result.warnings = std::move(warnings);
    co_return result;
  }

  void reportCodexResponseTiming(const std::function<void()>& callback) {
    try {
      if (callback) callback();
    } catch (...) {
      // Observability must not affect inference routing or delivery.
    }
  }

  std::string codexStatusClass(double status) {
    if (status >= 200 && status < 300) return "2xx";
    if (status == 401) return "401";
    if (status == 403) return "403";
    if (status == 429) return "429";
    if (status >= 400 && status < 500) return "invalid_request_4xx";
    if (status >= 500 && status < 600) return "5xx";
    return "other_http";
  }

  std::string codexErrorClass(const rt::Reason& error) {
    if (const CodexError* codexError = auth::codexErrorOf(error)) {
      if (dynamic_cast<const auth::CodexAuthFollowerStaleError*>(codexError)) return std::string(upstream_auth::FOLLOWER_STALE);
      if (codexError->code == "gateway_timeout") return "timeout";
      if (codexError->code == "codex_upstream_unreachable" || codexError->code == "codex_auth_refresh_unreachable") {
        return "network_failure";
      }
      return codexStatusClass(codexError->status);
    }
    return errorNamed(error, "TimeoutError") ? "timeout" : "network_failure";
  }

  bool isCodexSiblingTransportFailure(const rt::Reason& error) {
    const CodexError* codexError = auth::codexErrorOf(error);
    return codexError != nullptr && (codexError->code == "gateway_timeout" || codexError->code == "codex_upstream_unreachable");
  }

  rt::Reason dispatchFailureAsError(const rt::Reason& error, const std::function<rt::Reason()>& fallback) {
    if (rt::isError(error)) return error;
    if (isNullishReason(error)) return fallback();
    return rt::makeReason(rt::withCause(rt::JsError("Codex upstream dispatch failed with a non-Error value."), error));
  }

  void logCodexRouting(const std::string& event, const json::object& fields) {
    try {
      json::object payload;
      rt::objectDefine(payload, "event", json::value(event));
      for (const json::key_value_pair* member : rt::objectEntries(fields)) rt::objectDefine(payload, member->key(), member->value());
      rt::console::info("[ai.ubq.fi] codex_routing", rt::jsonStringify(payload));
    } catch (...) {
      // Routing telemetry must never alter provider selection.
    }
  }

  namespace {
    // waitForCodexRetry's `abortReason()`.
    rt::Reason retryAbortReason(const rt::AbortSignal* signal) {
      rt::Reason reason = signal != nullptr ? signal->reason() : rt::Reason();
      if (isNullishReason(reason)) reason = rt::makeReason(rt::DOMException("The request was aborted.", "AbortError"));
      if (errorNamed(reason, "TimeoutError")) {
        return rt::makeReason(CodexError("Codex upstream exceeded the gateway deadline while waiting to retry.", "gateway_timeout", 504, reason));
      }
      return reason;
    }
  }

  rt::Task<void> waitForCodexRetry(double milliseconds, std::optional<rt::AbortSignal> signal, std::function<rt::Task<void>(double milliseconds)> sleep) {
    if (signal.has_value() && signal->aborted()) std::rethrow_exception(retryAbortReason(&*signal));
    if (milliseconds <= 0) co_return;
    if (!signal.has_value()) {
      rt::Promise<void> sleeping(sleep(milliseconds));
      co_await sleeping.wait();
      co_return;
    }
    const rt::AbortSignal abortSignal = *signal;
    rt::EventListener onAbort;
    const auto cleanup = rt::scopeExit([&] { abortSignal.removeEventListener("abort", onAbort); });
    rt::Promise<void> aborted = rt::newPromise<void>([&onAbort, abortSignal](auto, rt::Reject reject) {
      onAbort = rt::EventListener([abortSignal, reject] { reject(auth_refresh::abortReasonAsError(retryAbortReason(&abortSignal))); });
      abortSignal.addEventListener("abort", onAbort, rt::AddEventListenerOptions{.once = true});
      if (abortSignal.aborted()) onAbort();
    });
    // The sleeper can finish after the abort race rejects. Call it from the owner
    // that will stay with the child even if the waiting frame is destroyed.
    auto sleeper = std::make_shared<decltype(sleep)>(std::move(sleep));
    auto sleepTask = (*sleeper)(milliseconds);
    rt::Promise<void> sleeping(rt::detail::keepAlive(std::move(sleeper), std::move(sleepTask)));
    std::vector<rt::Promise<void>> racers{sleeping, aborted};
    rt::Promise<void> raced = rt::race(std::move(racers));
    co_await raced.wait();
  }

  rt::Task<void> awaitPendingCodexProbeTransitions() {
    if (auth::codexProbeTransitionsInFlight.size()) {
      std::vector<rt::Promise<void>> transitions;
      for (const rt::Promise<void>& transition : auth::codexProbeTransitionsInFlight.values()) transitions.push_back(transition);
      auto settled = rt::allSettled(std::move(transitions));
      co_await settled.wait();
    }
  }

  std::optional<std::string> requestedCodexModel(const json::value& body) { return requestedCodexModel(&body); }

  std::optional<std::string> requestedCodexModel(const json::value* body) {
    return ::uos::utils::isRecord(body) ? ::uos::utils::getString(rt::get(body, "model")) : std::nullopt;
  }

  void applyNativeSessionHeaders(rt::Headers& headers, const std::optional<std::string>& nativeSessionIdentity) {
    if (!nativeSessionIdentity.has_value()) return;
    headers.set("session-id", *nativeSessionIdentity);
    headers.set("thread-id", *nativeSessionIdentity);
    headers.set("x-client-request-id", *nativeSessionIdentity);
  }

  namespace {
    /** The standard re-authentication response for a pool whose credentials all failed. */
    rt::Response allCodexCredentialsInvalidResponse() {
      return auth::withCodexAuthWarning(
        routingErrorResponse(401, std::string(auth::CODEX_AUTH_REAUTH_MESSAGE) + " All configured Codex credentials are invalid.", "codex_auth_invalid"),
        std::string(auth::CODEX_AUTH_REAUTH_WARNING));
    }
  }

  std::optional<rt::Response> initialCodexSelectionResponse(const routing_state::RouteSelection& selection) {
    if (std::holds_alternative<routing_state::RouteSelectionRoutingUnavailable>(selection)) {
      return routingErrorResponse(503, "Codex routing state is temporarily unavailable; retry the request.", "codex_auth_missing");
    }
    if (std::holds_alternative<routing_state::RouteSelectionCredentialsInvalid>(selection)) return allCodexCredentialsInvalidResponse();
    if (const auto* blocked = std::get_if<routing_state::RouteSelectionUpstreamBlocked>(&selection)) return upstreamTimeoutCircuitResponse(blocked->retryAtMs);
    if (const auto* unavailable = std::get_if<routing_state::RouteSelectionModelUnavailable>(&selection)) {
      return codexModelUnavailableResponse(unavailable->model, unavailable->detail);
    }
    return std::nullopt;
  }

  namespace {
    /** The exact upstream rejection that proves one subscription cannot serve one model. */
    const rt::Regex& codexModelUnsupportedDetailPattern() {
      static const rt::Regex pattern("^The '([^']+)' model is not supported when using Codex with a ChatGPT account\\.$", "");
      return pattern;
    }
  }

  std::optional<ParseCodexModelUnsupportedDetailResult> parseCodexModelUnsupportedDetail(const json::value& payload) {
    return parseCodexModelUnsupportedDetail(&payload);
  }

  std::optional<ParseCodexModelUnsupportedDetailResult> parseCodexModelUnsupportedDetail(const json::value* payload) {
    if (!::uos::utils::isRecord(payload)) return std::nullopt;
    const std::optional<std::string> detail = ::uos::utils::getString(rt::get(payload, "detail"));
    if (!detail.has_value()) return std::nullopt;
    const std::optional<rt::RegexMatch> match = codexModelUnsupportedDetailPattern().exec(*detail);
    if (!match.has_value()) return std::nullopt;
    const std::optional<std::string> model = (*match)[1];
    if (!rt::truthy(model)) return std::nullopt;
    return ParseCodexModelUnsupportedDetailResult{.model = *model, .detail = *detail};
  }

  namespace {
    /**
     * A 400 probe must never extend the caller's latency: the clone is read under
     * the shared byte and time bounds, and an incomplete read is not evidence.
     */
    constexpr double CODEX_MODEL_UNSUPPORTED_PROBE_TIMEOUT_MS = 250;

    rt::Task<std::optional<std::string>> boundedResponseText(rt::Response response) {
      try {
        ::uos::bounded_response_body::ReadBoundedResponseBodyOptions options;
        options.timeoutMs = CODEX_MODEL_UNSUPPORTED_PROBE_TIMEOUT_MS;
        options.cancellationReason = json::value("Codex model availability probe");
        rt::Promise<::uos::bounded_response_body::BoundedResponseBody> reading(::uos::bounded_response_body::readBoundedResponseBody(response, std::move(options)));
        const ::uos::bounded_response_body::BoundedResponseBody body = co_await reading.wait();
        if (!body.complete) co_return std::nullopt;
        co_return rt::decodeUtf8(body.bytes);
      } catch (...) {
      }
      co_return std::nullopt;
    }
  }

  rt::Task<std::optional<ParseCodexModelUnsupportedDetailResult>> codexModelUnsupportedFromResponse(rt::Response response) {
    if (response.status() != 400) co_return std::nullopt;
    std::optional<rt::Response> clone;
    try {
      clone = response.clone();
    } catch (...) {
    }
    if (!clone.has_value()) co_return std::nullopt;
    rt::Promise<std::optional<std::string>> reading(boundedResponseText(*clone));
    const std::optional<std::string> text = co_await reading.wait();
    if (!rt::truthy(text)) co_return std::nullopt;
    try {
      co_return parseCodexModelUnsupportedDetail(rt::jsonParse(*text));
    } catch (...) {
    }
    co_return std::nullopt;
  }

  namespace {
    /**
     * Prove which account answered a response through the durable active row it was
     * admitted under. A learned rejection skips that account for that model, so a
     * wrong attribution would take capacity from a capable account: both the
     * admission generation and the opaque account identity must match.
     */
    rt::Task<std::optional<std::string>> codexResponseAccountId(rt::Response response) {
      const std::optional<double> activeGeneration = auth::getCodexResponseActiveTelemetry(response).activeGeneration;
      if (!activeGeneration.has_value() || !auth::getCodexResponseSlot(response).has_value()) co_return std::nullopt;
      try {
        rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
        const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
        if (!kv) co_return std::nullopt;
        rt::KvGetOptions strong;
        strong.consistency = rt::KvConsistencyLevel::strong;
        rt::Task<rt::KvEntryMaybe<>> reading = kv->get(routing_state::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, strong);
        const rt::KvEntryMaybe<> entry = co_await std::move(reading);
        const std::optional<routing_state::CodexActiveAccountSelection> active = routing_state::parseCodexActiveAccountSelection(entry.value.toJson());
        if (!active.has_value() || active->generation != *activeGeneration) co_return std::nullopt;
        rt::Promise<auth::CodexAuthPoolEntry> loading(auth::getAuthPoolEntry(true, true));
        const auth::CodexAuthPoolEntry poolEntry = co_await loading.wait();
        if (!poolEntry.entry.has_value() || poolEntry.entry->versionstamp != active->pool_versionstamp) co_return std::nullopt;
        for (const CodexAuthState& auth : poolEntry.pool.accounts) {
          rt::Promise<capacity_routing::CodexRoutingAccountIdentity> identifying(capacity_routing::routingAccountIdentity(auth));
          const capacity_routing::CodexRoutingAccountIdentity identity = co_await identifying.wait();
          if (identity.accountIdHash == active->account_id_hash) co_return auth.account_id;
        }
      } catch (...) {
        // Attribution is best effort; without it the response is passed through.
      }
      co_return std::nullopt;
    }
  }

  rt::Task<std::optional<CodexModelUnavailableAttempt>> learnCodexModelUnavailable(rt::Response response) {
    rt::Promise<std::optional<ParseCodexModelUnsupportedDetailResult>> classifying(codexModelUnsupportedFromResponse(response));
    const std::optional<ParseCodexModelUnsupportedDetailResult> unsupported = co_await classifying.wait();
    if (!unsupported.has_value()) co_return std::nullopt;
    rt::Promise<std::optional<std::string>> attributing(codexResponseAccountId(response));
    const std::optional<std::string> accountId = co_await attributing.wait();
    if (!accountId.has_value()) co_return std::nullopt;
    ::uos::models::codex_models_availability::CodexModelUnsupportedOptions options;
    options.detail = unsupported->detail;
    rt::Promise<void> recording(::uos::models::codex_models_availability::recordCodexModelUnsupported(*accountId, unsupported->model, std::move(options)));
    co_await recording.wait();
    co_return CodexModelUnavailableAttempt{.accountId = *accountId, .model = unsupported->model, .detail = unsupported->detail};
  }

  namespace {
    struct CodexModelUnavailableLoopState {
      double reselections;
      bool siblingAttempted;
    };

    /**
     * One bounded sibling attempt for a learned model-unavailable rejection. The
     * recorded account+model pair makes the following reselection skip the
     * ineligible account; when this request has already spent its attempt, the
     * graceful 404 is the final answer.
     */
    // `drivers` and `state` live in runCodexSerialAdmissionLoop's frame, which awaits this.
    rt::Task<std::optional<rt::Response>> advanceCodexModelUnavailable(const CodexSerialAdmissionDrivers* drivers, CodexModelUnavailableAttempt attempt,
                                                                      CodexModelUnavailableLoopState* state) {
      if (state->siblingAttempted || state->reselections >= auth::CODEX_ACTIVE_ADMISSION_RESEELECTION_LIMIT) {
        co_return codexModelUnavailableResponse(attempt.model, attempt.detail);
      }
      state->siblingAttempted = true;
      state->reselections += 1;
      rt::Promise<std::optional<rt::Response>> advancing(drivers->advanceReselection());
      std::optional<rt::Response> advanced = co_await advancing.wait();
      co_return advanced;
    }

    // `Readonly<{ kind: "return"; response: Response } | { kind: "continue" } | { kind: "exhausted" }>`
    enum class CodexAdmissionPassKind { return_, continue_, exhausted };
    struct CodexAdmissionPassOutcome {
      CodexAdmissionPassKind kind = CodexAdmissionPassKind::continue_;
      std::optional<rt::Response> response{};
    };

    /** The terminal-transport, queued-retry and reselection part of one dispatch-less pass. */
    rt::Task<CodexAdmissionPassOutcome> continueCodexAdmissionWithoutDispatch(const CodexSerialAdmissionDrivers* drivers, CodexModelUnavailableLoopState* state) {
      rt::Promise<std::optional<rt::Response>> terminal(drivers->terminalTransportResponse());
      const std::optional<rt::Response> terminalTransport = co_await terminal.wait();
      if (terminalTransport.has_value()) co_return CodexAdmissionPassOutcome{CodexAdmissionPassKind::return_, terminalTransport};
      if (drivers->hasQueuedRetry()) co_return CodexAdmissionPassOutcome{CodexAdmissionPassKind::continue_, std::nullopt};
      if (!drivers->reselectionRequested() || state->reselections >= auth::CODEX_ACTIVE_ADMISSION_RESEELECTION_LIMIT) {
        co_return CodexAdmissionPassOutcome{CodexAdmissionPassKind::exhausted, std::nullopt};
      }
      state->reselections += 1;
      rt::Promise<std::optional<rt::Response>> advancing(drivers->advanceReselection());
      const std::optional<rt::Response> advanced = co_await advancing.wait();
      if (advanced.has_value()) co_return CodexAdmissionPassOutcome{CodexAdmissionPassKind::return_, advanced};
      co_return CodexAdmissionPassOutcome{CodexAdmissionPassKind::continue_, std::nullopt};
    }

    using ClassifyModelUnavailable = std::function<rt::Task<std::optional<CodexModelUnavailableAttempt>>(rt::Response response)>;

    /** One dispatched response: an unchanged pass-through, or the bounded learned rejection. */
    rt::Task<CodexAdmissionPassOutcome> handleCodexDispatchedResponse(const CodexSerialAdmissionDrivers* drivers, ClassifyModelUnavailable classifyModelUnavailable,
                                                                      rt::Response dispatched, CodexModelUnavailableLoopState* state) {
      rt::Promise<std::optional<CodexModelUnavailableAttempt>> classifying(classifyModelUnavailable(dispatched));
      const std::optional<CodexModelUnavailableAttempt> unsupported = co_await classifying.wait();
      if (!unsupported.has_value()) co_return CodexAdmissionPassOutcome{CodexAdmissionPassKind::return_, dispatched};
      rt::Promise<std::optional<rt::Response>> advancing(advanceCodexModelUnavailable(drivers, *unsupported, state));
      const std::optional<rt::Response> advanced = co_await advancing.wait();
      if (!advanced.has_value()) co_return CodexAdmissionPassOutcome{CodexAdmissionPassKind::continue_, std::nullopt};
      co_return CodexAdmissionPassOutcome{CodexAdmissionPassKind::return_, advanced};
    }
  }

  rt::Task<rt::Response> runCodexSerialAdmissionLoop(CodexSerialAdmissionDrivers drivers) {
    const ClassifyModelUnavailable classifyModelUnavailable =
      drivers.classifyModelUnavailable ? drivers.classifyModelUnavailable : ClassifyModelUnavailable(&learnCodexModelUnavailable);
    CodexModelUnavailableLoopState state{.reselections = 0, .siblingAttempted = false};
    for (;;) {
      rt::Promise<std::optional<rt::Response>> retrying(drivers.runPendingShortRetry());
      std::optional<rt::Response> dispatched = co_await retrying.wait();
      if (!dispatched.has_value()) {
        rt::Promise<std::optional<rt::Response>> dispatching(drivers.dispatchActive());
        dispatched = co_await dispatching.wait();
      }
      CodexAdmissionPassOutcome outcome;
      if (!dispatched.has_value()) {
        rt::Promise<CodexAdmissionPassOutcome> continuing(continueCodexAdmissionWithoutDispatch(&drivers, &state));
        outcome = co_await continuing.wait();
      } else {
        rt::Promise<CodexAdmissionPassOutcome> handling(handleCodexDispatchedResponse(&drivers, classifyModelUnavailable, *dispatched, &state));
        outcome = co_await handling.wait();
      }
      if (outcome.kind == CodexAdmissionPassKind::return_) co_return *outcome.response;
      if (outcome.kind == CodexAdmissionPassKind::exhausted) break;
    }
    rt::Promise<rt::Response> exhausting(drivers.exhaustedResponse());
    rt::Response response = co_await exhausting.wait();
    co_return response;
  }
}
