#include "uos/provider/claude_code_usage.hpp"

#include "uos/provider/claude_code.hpp"
#include "uos/provider/claude_code_usage_schema.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <array>
#include <utility>

namespace uos::provider::claude_code_usage {
  namespace {
    using claude_code_quota::ClaudeCodeQuotaObservation;
    using claude_code_quota::ClaudeCodeQuotaWindow;
    using claude_code_quota::sanitizeClaudeCodeErrorMessage;

    constexpr double USAGE_TIMEOUT_MS = 8'000;
    constexpr double ERROR_BODY_MAX_BYTES = 4'096;

    rt::Task<std::string> boundedText(rt::Response response) {
      std::string text;
      try {
        text = co_await response.text();
      } catch (...) {
        text = "";
      }
      if (static_cast<double>(rt::length(text)) > ERROR_BODY_MAX_BYTES) co_return rt::slice(text, 0, ERROR_BODY_MAX_BYTES);
      co_return text;
    }

    std::optional<std::string> errorMessage(const std::string& text) {
      try {
        const json::value payload = !text.empty() ? rt::jsonParse(text) : json::value(nullptr);
        if (utils::isRecord(payload)) {
          const json::value* error = rt::get(payload, "error");
          if (utils::isRecord(error)) {
            const json::value* message = rt::get(*error, "message");
            if (message != nullptr && message->is_string()) return sanitizeClaudeCodeErrorMessage(std::string(message->get_string()));
          }
          const json::value* message = rt::get(payload, "message");
          if (message != nullptr && message->is_string()) return sanitizeClaudeCodeErrorMessage(std::string(message->get_string()));
        }
      } catch (const rt::JsError&) {
        // A non-JSON error body is still useful as bounded operator text.
      }
      return sanitizeClaudeCodeErrorMessage(text);
    }

    std::optional<double> retryAfterMs(const std::optional<std::string>& value, double nowMs) {
      if (!value || value->empty()) return std::nullopt;
      const double seconds = rt::stringToNumber(*value);
      if (rt::Number::isFinite(seconds) && seconds >= 0) return rt::Math::trunc(seconds * 1'000);
      const double at = rt::Date::parse(*value);
      if (rt::Number::isFinite(at) && at > 0) return rt::Math::max(0.0, at - nowMs);
      return std::nullopt;
    }

    ClaudeCodeUsageErrorKind failureKind(double status, const std::optional<std::string>& message) {
      static const rt::Regex SCOPE("user:profile|(?:missing|required|insufficient).*scope|scope.*(?:missing|required|insufficient)", "i");
      if ((message && !message->empty() && SCOPE.test(*message)) || status == 403) return "scope_missing";
      if (status == 401) return "auth_invalid";
      if (status == 429) return "rate_limited";
      if (status >= 500) return "upstream_error";
      return "http_error";
    }

    ClaudeCodeUsageFailure failure(ClaudeCodeUsageErrorKind kind, std::optional<double> status, double observedAtMs, std::optional<std::string> message,
                                   std::optional<double> retryAfter = std::nullopt) {
      return {.kind = std::move(kind), .status = status, .observed_at_ms = observedAtMs, .message = std::move(message), .retry_after_ms = retryAfter};
    }
  }

  std::optional<ClaudeCodeQuotaObservation> parseClaudeCodeUsagePayload(const json::value* payload, double observedAtMs) {
    const std::optional<claude_code_usage_schema::ClaudeCodeUsageSnapshot> snapshot =
      claude_code_usage_schema::parseClaudeCodeUsageSnapshot(payload, rt::Math::trunc(observedAtMs));
    if (!snapshot) return std::nullopt;
    const double snapshotObservedAtMs = *rt::getNumber(*snapshot, "observed_at_ms");
    const json::object& snapshotPayload = *rt::getObject(*snapshot, "payload");
    const json::array& meters = *rt::getArray(*snapshot, "meters");
    claude_code_quota::ClaudeCodeQuotaWindows windows;
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 2> LEGACY = {{{"five_hour", "5h"}, {"seven_day", "7d"}}};
    for (const auto& [key, id] : LEGACY) {
      const json::value* window = rt::get(snapshotPayload, key);
      const json::object* meter = nullptr;
      for (const json::value& item : meters)
        if (const json::value* kind = rt::get(item, "kind"); kind != nullptr && kind->is_string() && kind->get_string() == key) {
          meter = &item.get_object();
          break;
        }
      if (window != nullptr && rt::truthy(*window)) {
        const json::value* resetsAt = rt::get(*window, "resets_at");
        const double reset = resetsAt != nullptr && resetsAt->is_null() ? rt::Number::NaN : rt::Date::parse(rt::toString(resetsAt));
        const json::value* utilization = rt::get(*window, "utilization");
        std::optional<double> resetAtMs;
        if (meter != nullptr) resetAtMs = rt::getNumber(*meter, "reset_at_ms");
        if (!resetAtMs && rt::Number::isSafeInteger(reset) && reset > 0) resetAtMs = reset;
        windows.set(std::string(id), ClaudeCodeQuotaWindow{
                                       .status = std::nullopt,
                                       .utilization = utilization != nullptr && utilization->is_null()
                                                        ? std::nullopt
                                                        : std::optional<double>(rt::Math::min(1.0, rt::Math::max(0.0, rt::toNumber(utilization) / 100))),
                                       .reset_at_ms = resetAtMs,
                                       .observed_at_ms = snapshotObservedAtMs,
                                     });
      }
    }
    return ClaudeCodeQuotaObservation{
      .windows = std::move(windows),
      .standard = {},
      .representative_claim = std::nullopt,
      .observed_at_ms = snapshotObservedAtMs,
      .usage_snapshot = snapshot,
    };
  }
  std::optional<ClaudeCodeQuotaObservation> parseClaudeCodeUsagePayload(const json::value& payload, double observedAtMs) {
    return parseClaudeCodeUsagePayload(&payload, observedAtMs);
  }
  std::optional<ClaudeCodeQuotaObservation> parseClaudeCodeUsagePayload(const json::value* payload) { return parseClaudeCodeUsagePayload(payload, rt::dateNow()); }
  std::optional<ClaudeCodeQuotaObservation> parseClaudeCodeUsagePayload(const json::value& payload) { return parseClaudeCodeUsagePayload(&payload, rt::dateNow()); }

  rt::Task<ClaudeCodeUsageResult> fetchClaudeCodeUsage(claude_code::ClaudeCodeAuthHeaders credential, FetchClaudeCodeUsageOptions options) {
    const std::function<double()> clock = options.now ? options.now : std::function<double()>([] { return rt::dateNow(); });
    if (credential.kind != "oauth") {
      co_return failure("unsupported_credential", std::nullopt, rt::Math::trunc(clock()),
                        std::string("Claude Console API keys do not expose the OAuth usage endpoint; gateway traffic continues to update the meter."));
    }
    const rt::AbortSignal timeout = rt::AbortSignal::timeout(USAGE_TIMEOUT_MS);
    std::vector<rt::AbortSignal> sources;
    if (options.signal) {
      sources.push_back(*options.signal);
      sources.push_back(timeout);
    }
    const rt::AbortSignal signal = options.signal ? rt::AbortSignal::any(sources) : timeout;
    const rt::Headers headers{
      {"Authorization", "Bearer " + credential.secret},
      {"Accept", "application/json"},
      {"Content-Type", "application/json"},
      {"anthropic-beta", std::string(claude_code::CLAUDE_CODE_OAUTH_BETA)},
      {"anthropic-version", std::string(claude_code::CLAUDE_CODE_ANTHROPIC_VERSION)},
      {"User-Agent", std::string(CLAUDE_CODE_USAGE_USER_AGENT)},
    };
    std::optional<rt::Response> received;
    rt::Reason fetchError;
    try {
      const rt::Fetch fetcher = options.fetcher ? options.fetcher : rt::globalFetch();
      rt::RequestInit init;
      init.method = "GET";
      init.headers = headers;
      init.redirect = "manual";
      init.signal = signal;
      rt::RequestInfo input = std::string(CLAUDE_CODE_USAGE_URL);
      auto call = fetcher(std::move(input), std::move(init));
      received = co_await std::move(call);
    } catch (...) {
      fetchError = std::current_exception();
    }
    if (fetchError) {
      const std::optional<std::string> message = rt::isError(fetchError) ? sanitizeClaudeCodeErrorMessage(rt::errorMessage(fetchError).value_or("")) : std::nullopt;
      co_return failure("unreachable", std::nullopt, rt::Math::trunc(clock()), message);
    }
    const rt::Response response = *received;
    const double observedAtMs = rt::Math::trunc(clock());
    if (!response.ok()) {
      rt::Promise<std::string> reading(boundedText(response));
      const std::string text = co_await reading.wait();
      const std::optional<std::string> message = errorMessage(text);
      const double status = response.status();
      ClaudeCodeUsageErrorKind kind = failureKind(status, message);
      const std::optional<std::string> reported =
        kind == "scope_missing" && !message ? std::optional<std::string>("Claude OAuth credential cannot read usage because the user:profile scope is missing.") : message;
      const std::optional<double> retryAfter = kind == "rate_limited" ? retryAfterMs(response.headers().get("retry-after"), observedAtMs) : std::nullopt;
      co_return failure(std::move(kind), status, observedAtMs, reported, retryAfter);
    }
    json::value payload;
    bool unreadable = false;
    try {
      payload = co_await response.json();
    } catch (...) {
      unreadable = true;
    }
    if (unreadable) co_return failure("invalid_response", response.status(), observedAtMs, std::string("Claude usage returned an unreadable JSON payload."));
    std::optional<ClaudeCodeQuotaObservation> observation = parseClaudeCodeUsagePayload(payload, observedAtMs);
    if (!observation)
      co_return failure("invalid_response", response.status(), observedAtMs, std::string("Claude usage returned an invalid or unrecognized meter payload."));
    co_return ClaudeCodeUsageSuccess{.status = static_cast<double>(response.status()), .observed_at_ms = observedAtMs, .observation = std::move(*observation)};
  }
  rt::Task<ClaudeCodeUsageResult> fetchClaudeCodeUsage(claude_code::ClaudeCodeAuthHeaders credential) {
    return fetchClaudeCodeUsage(std::move(credential), FetchClaudeCodeUsageOptions());
  }
}
