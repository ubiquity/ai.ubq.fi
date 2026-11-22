// Port of src/provider/lithos-rate-limits.ts.
#include "uos/provider/lithos_rate_limits.hpp"
#include "uos/rt/memory.hpp"

#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace uos::provider::lithos_rate_limits {
  namespace {
    /**
     * The failover ladder behind a requested tier, deepest capability first:
     * `-ultra` refuses -> `-fast` answers -> the family's normal tier answers.
     *
     * Every tier owns its own rate-limit bucket, probed live 2026-09-25: consuming
     * 250,006 tokens on `-fast` left `-ultra` reporting `x-ratelimit-remaining-tokens:
     * 4000000`, and each tier refills at 4,000,000 tokens/minute. A hop therefore
     * moves real capacity alongside capability, and the ladder is no longer a single
     * sibling pair. The next tier below a refusal is tried immediately (no wait),
     * because its bucket is independent; only when the WHOLE ladder refuses does the
     * request wait on the vendor's own reset window.
     *
     * `-ultra-chat` is deliberately absent: LithosAI told the owner on 2026-09-24
     * that it is tuned for short context windows and loses accuracy on the
     * large-context sessions this route serves.
     */
    // `ReadonlyMap<string, readonly string[]>`, built on first use.
    const rt::OrderedMap<std::string, std::vector<std::string>>& LITHOS_FAILOVER_LADDERS() {
      static const rt::OrderedMap<std::string, std::vector<std::string>> ladders{
        {"deepseek-ai/DeepSeek-V4.1-Flash-ultra", {"deepseek-ai/DeepSeek-V4.1-Flash-fast", "deepseek-ai/DeepSeek-V4.1-Flash"}},
        {"deepseek-ai/DeepSeek-V4.1-Flash-fast", {"deepseek-ai/DeepSeek-V4.1-Flash"}},
        {"moonshotai/Kimi-K3-ultra", {"moonshotai/Kimi-K3-fast", "moonshotai/Kimi-K3"}},
        {"moonshotai/Kimi-K3-fast", {"moonshotai/Kimi-K3"}},
      };
      return ladders;
    }

    /** Canonical spellings for ladder rungs, using the same trim/case contract as model dispatch. */
    // Derived from LITHOS_FAILOVER_LADDERS on first use; lookups only, so the order is not kept.
    const std::unordered_map<std::string, std::string>& LITHOS_CANONICAL_FAILOVER_MODELS() {
      static const std::unordered_map<std::string, std::string> models = [] {
        std::unordered_map<std::string, std::string> out;
        for (const auto& [model, ladder] : LITHOS_FAILOVER_LADDERS()) {
          out.insert_or_assign(rt::toLowerCase(model), model);
          for (const std::string& rung : ladder) out.insert_or_assign(rt::toLowerCase(rung), rung);
        }
        return out;
      }();
      return models;
    }

    std::string lithosCanonicalFailoverModelFor(const std::string& modelRaw) {
      std::string trimmedModel = rt::trim(modelRaw);
      const std::unordered_map<std::string, std::string>& models = LITHOS_CANONICAL_FAILOVER_MODELS();
      const auto found = models.find(rt::toLowerCase(trimmedModel));
      return found != models.end() ? found->second : trimmedModel;
    }

    /**
     * The in-process failover windows: a tier whose refusal named a reset instant
     * is skipped until that instant passes. Each refused rung has its own deadline,
     * shared by every request whose ladder contains it, so recovered capacity is
     * eligible again without inheriting another rung's reset. Scoped to models with
     * a ladder, so a bottom tier never accumulates state.
     *
     * The state is per process on purpose: it mirrors what this gateway instance has
     * been told by the vendor, and a restart re-learns it from the first refusal.
     */
    // `new Map<string, number>()`: get/set/delete/clear only, so the order is never observed.
    std::unordered_map<std::string, double>& lithosFailoverWindows() {
      static std::unordered_map<std::string, double> windows;
      return windows;
    }

    void reportWindowMemory() {
      const auto& windows = lithosFailoverWindows();
      std::size_t bytes = 0;
      for (const auto& entry : windows) bytes += sizeof(entry) + rt::memory::bytes(entry.first);
      rt::memory::setCache(rt::memory::Cache::lithos_rate_windows, windows.size(), bytes);
    }

    const rt::Regex& DIGITS_ONLY() {
      static const rt::Regex regex("^\\d+$");
      return regex;
    }

    std::optional<std::string> trimmedHeader(const rt::Headers& headers, std::string_view name) {
      std::optional<std::string> raw = headers.get(name);
      if (!raw) return std::nullopt;
      return rt::trim(*raw);
    }

    rt::Reason lithosRetryAbortError(const rt::AbortSignal& signal) {
      const rt::Reason& reason = signal.reason();
      if (rt::isError(reason)) return reason;
      // Not an Error and no thrown value: `undefined`.
      if (!rt::thrownValue(reason)) {
        return rt::makeReason(rt::DOMException("The request was aborted while waiting for the LithosAI rate limit to reset.", "AbortError"));
      }
      rt::JsError error = rt::withCause(rt::JsError("The request was aborted while waiting for the LithosAI rate limit to reset."), reason);
      error.name = "AbortError";
      return rt::makeReason(std::move(error));
    }

    /** The duration units the vendor's reset headers use, in milliseconds. */
    // `ReadonlyMap<string, number>`; lookups only.
    const std::unordered_map<std::string, double>& LITHOS_DURATION_UNITS_MS() {
      static const std::unordered_map<std::string, double> units{
        {"ms", 1},
        {"s", 1'000},
        {"m", 60'000},
        {"h", 3'600'000},
      };
      return units;
    }

    /** Digits and the decimal point a duration magnitude may carry. */
    bool isDurationDigit(char character) { return (character >= '0' && character <= '9') || character == '.'; }

    std::optional<double> lithosIntegerHeader(const std::optional<std::string>& raw) {
      if (!raw) return std::nullopt;
      const std::string value = rt::trim(*raw);
      if (!DIGITS_ONLY().test(value)) return std::nullopt;
      const double parsed = rt::stringToNumber(value);
      return std::isfinite(parsed) ? std::optional<double>(parsed) : std::nullopt;
    }

    /**
     * `x-ratelimit-reset-*` carry deltas like `3.23s`, `1s`, `250ms` or `1m30s`.
     * Scanned rather than matched: the shapes are tiny and a linear scan keeps the
     * parse free of the backtracking a repeated unit pattern invites.
     */
    // Every character the scan steps over is ASCII (digits, `.` and the unit letters) and any
    // other character ends it with null, so byte offsets equal the TS's UTF-16 offsets.
    std::optional<double> lithosDurationMs(const std::optional<std::string>& raw) {
      if (!raw) return std::nullopt;
      static const rt::Regex WHITESPACE_RUNS("\\s+", "g");
      const std::string value = rt::replace(rt::trim(*raw), WHITESPACE_RUNS, "");
      if (value.empty()) return std::nullopt;
      double total = 0;
      std::size_t cursor = 0;
      while (cursor < value.size()) {
        std::size_t digitsEnd = cursor;
        while (digitsEnd < value.size() && isDurationDigit(value[digitsEnd])) digitsEnd += 1;
        if (digitsEnd == cursor) return std::nullopt;
        const double magnitude = rt::stringToNumber(std::string_view(value).substr(cursor, digitsEnd - cursor));
        if (!std::isfinite(magnitude)) return std::nullopt;
        const std::string_view rest = std::string_view(value).substr(digitsEnd);
        const std::string unitKey(rest.starts_with("ms") ? rest.substr(0, 2) : rest.substr(0, std::min<std::size_t>(rest.size(), 1)));
        const auto unit = LITHOS_DURATION_UNITS_MS().find(unitKey);
        if (unit == LITHOS_DURATION_UNITS_MS().end()) return std::nullopt;
        total += magnitude * unit->second;
        cursor = digitsEnd + unitKey.size();
      }
      return std::isfinite(total) ? std::optional<double>(total) : std::nullopt;
    }

    /** `retry-after` is either an integer number of seconds or an HTTP date. */
    std::optional<double> lithosRetryAfterMs(const std::optional<std::string>& raw, double nowMs) {
      const std::optional<double> seconds = lithosIntegerHeader(raw);
      if (seconds) {
        const double waitMs = *seconds * 1'000;
        return std::isfinite(waitMs) ? std::optional<double>(waitMs) : std::nullopt;
      }
      if (!raw) return std::nullopt;
      const double parsed = rt::Date::parse(rt::trim(*raw));
      const double waitMs = parsed - nowMs;
      return std::isfinite(waitMs) && waitMs > 0 ? std::optional<double>(waitMs) : std::nullopt;
    }

    void logLithosRateLimitEvent(std::string_view event, const LithosRateLimitLogFields& fields) {
      try {
        rt::console::info(event, rt::jsonStringify(json::value(fields)));
      } catch (...) {
        // Telemetry must never change routing or delivery.
      }
    }
  }

  std::vector<std::string> lithosFailoverLadderFor(const std::string& modelRaw) {
    const std::vector<std::string>* ladder = LITHOS_FAILOVER_LADDERS().get(lithosCanonicalFailoverModelFor(modelRaw));
    return ladder ? *ladder : std::vector<std::string>{};
  }

  bool lithosIsLadderTarget(const std::string& modelRaw, const std::string& target) {
    const std::vector<std::string> ladder = lithosFailoverLadderFor(modelRaw);
    return std::find(ladder.begin(), ladder.end(), lithosCanonicalFailoverModelFor(target)) != ladder.end();
  }

  std::optional<std::string> lithosFailoverTargetAt(const std::string& modelRaw, double nowMs) {
    if (!std::isfinite(nowMs)) return std::nullopt;
    const std::string canonicalModel = lithosCanonicalFailoverModelFor(modelRaw);
    std::vector<std::string> rungs{canonicalModel};
    for (std::string& rung : lithosFailoverLadderFor(canonicalModel)) rungs.push_back(std::move(rung));
    std::unordered_map<std::string, double>& windows = lithosFailoverWindows();
    for (const std::string& rung : rungs) {
      const auto deadline = windows.find(rung);
      if (deadline != windows.end() && nowMs < deadline->second) continue;
      if (windows.erase(rung)) reportWindowMemory();
      return rung == canonicalModel ? std::nullopt : std::optional<std::string>(rung);
    }
    return std::nullopt;
  }

  void lithosOpenFailoverWindow(const std::string& modelRaw, double nowMs, double waitMs, const std::string& target,
                                const std::optional<std::string>& refusedRungOverride) {
    const std::string canonicalModel = lithosCanonicalFailoverModelFor(modelRaw);
    const std::vector<std::string> ladder = lithosFailoverLadderFor(canonicalModel);
    const std::int64_t targetIndex = rt::indexOf(ladder, lithosCanonicalFailoverModelFor(target));
    const double deadlineMs = nowMs + waitMs;
    if (targetIndex == -1 || !std::isfinite(nowMs) || !std::isfinite(waitMs) || waitMs <= 0 || !std::isfinite(deadlineMs)) return;
    const std::string inferredRefusedRung = targetIndex == 0 ? canonicalModel : ladder[static_cast<std::size_t>(targetIndex - 1)];
    const std::string refusedRung = !refusedRungOverride ? inferredRefusedRung : lithosCanonicalFailoverModelFor(*refusedRungOverride);
    const std::int64_t refusedIndex = refusedRung == canonicalModel ? -1 : rt::indexOf(ladder, refusedRung);
    if (refusedRung != canonicalModel && refusedIndex == -1) return;
    if (refusedIndex >= targetIndex) return;
    std::unordered_map<std::string, double>& windows = lithosFailoverWindows();
    const auto current = windows.find(refusedRung);
    const double unexpiredDeadlineMs = current != windows.end() && current->second > nowMs ? current->second : deadlineMs;
    windows.insert_or_assign(refusedRung, rt::Math::max(deadlineMs, unexpiredDeadlineMs));
    reportWindowMemory();
  }

  void clearLithosFailoverWindows() { lithosFailoverWindows().clear(); reportWindowMemory(); }

  void recordLithosFailoverModel(const openai_telemetry::UsageContext* usageContext, const std::optional<std::string>& model) {
    if (usageContext && usageContext->responseTelemetry) usageContext->responseTelemetry->rateLimitFailoverModel = model;
  }

  std::optional<LithosRateLimitWait> lithosRefusalWait(const rt::Headers& headers, double waitedMs, double waits, const LithosRefusalWaitPolicy& policy) {
    if (waits >= policy.maxWaits) return std::nullopt;
    std::optional<LithosRateLimitWait> planned = lithosRateLimitWait(headers, rt::dateNow());
    if (!planned) return std::nullopt;
    if (planned->waitMs > LITHOS_REFUSAL_WAIT_CAP_MS) return std::nullopt;
    if (waitedMs + planned->waitMs > policy.totalCapMs) return std::nullopt;
    return planned;
  }

  void recordLithosRateLimitWaitMs(const openai_telemetry::UsageContext* usageContext, double elapsedMs) {
    if (usageContext && usageContext->responseTelemetry) usageContext->responseTelemetry->rateLimitWaitMs = elapsedMs;
  }

  void logLithosRateLimitWait(const LithosRateLimitLogFields& fields) { logLithosRateLimitEvent("[ai.ubq.fi] lithos_rate_limit_wait", fields); }

  rt::Promise<void> waitForLithosRetry(double milliseconds, rt::AbortSignal signal) {
    return waitForLithosRetry(milliseconds, std::move(signal), std::function<void(double)>());
  }

  rt::Promise<void> waitForLithosRetry(double milliseconds, rt::AbortSignal signal, std::function<void(double elapsedMs)> onElapsed) {
    if (milliseconds <= 0) {
      if (onElapsed) onElapsed(0);
      return rt::Promise<void>::resolve();
    }
    if (signal.aborted()) {
      if (onElapsed) onElapsed(0);
      return rt::Promise<void>::reject(lithosRetryAbortError(signal));
    }
    const double startedAt = rt::performanceNow();
    const auto reportElapsed = [onElapsed, startedAt] {
      if (onElapsed) onElapsed(rt::Math::max(0.0, rt::Math::round(rt::performanceNow() - startedAt)));
    };
    // The timer and the abort listener refer to each other, as the TS closures do. The
    // listener reaches the timer (and the signal) weakly, so neither keeps the other alive
    // once it fired or was removed.
    struct RetryWait {
      rt::Timeout timer;
      rt::EventListener onAbort;
    };
    auto wait = std::make_shared<RetryWait>();
    return rt::newPromise<void>([&](const rt::Resolve<void>& resolve, const rt::Reject& reject) {
      wait->timer = rt::setTimeout(
        [wait, signal, reportElapsed, resolve] {
          signal.removeEventListener("abort", wait->onAbort);
          reportElapsed();
          resolve();
        },
        milliseconds);
      const std::weak_ptr<RetryWait> weakWait = wait;
      const std::weak_ptr<rt::detail::AbortSignalState> weakSignal = signal.state();
      wait->onAbort = rt::EventListener([weakWait, weakSignal, reportElapsed, reject] {
        if (const std::shared_ptr<RetryWait> pending = weakWait.lock()) rt::clearTimeout(pending->timer);
        reportElapsed();
        if (const std::shared_ptr<rt::detail::AbortSignalState> state = weakSignal.lock()) reject(lithosRetryAbortError(rt::AbortSignal(state)));
      });
      signal.addEventListener("abort", wait->onAbort, {.once = true});
    });
  }

  LithosRateLimitLogFields lithosRateLimitSnapshot(const rt::Headers& headers) {
    const auto count = [&headers](std::string_view name) -> json::value {
      const std::optional<std::string> raw = trimmedHeader(headers, name);
      return raw && rt::truthy(*raw) && DIGITS_ONLY().test(*raw) ? rt::makeNumber(rt::stringToNumber(*raw)) : json::value(nullptr);
    };
    const auto text = [&headers](std::string_view name) -> json::value {
      const std::optional<std::string> raw = trimmedHeader(headers, name);
      return raw && rt::truthy(*raw) ? json::value(rt::slice(*raw, 0, 40)) : json::value(nullptr);
    };
    const std::optional<LithosRateLimitWait> window = lithosRateLimitWait(headers, rt::dateNow());
    LithosRateLimitLogFields fields;
    fields["window_ms"] = window ? rt::makeNumber(window->waitMs) : json::value(nullptr);
    fields["window_source"] = window ? json::value(window->source) : json::value(nullptr);
    fields["retry_after"] = text("retry-after");
    fields["retry_after_ms"] = count("retry-after-ms");
    fields["remaining_tokens"] = count("x-ratelimit-remaining-tokens");
    fields["limit_tokens"] = count("x-ratelimit-limit-tokens");
    fields["reset_tokens"] = text("x-ratelimit-reset-tokens");
    fields["remaining_requests"] = count("x-ratelimit-remaining-requests");
    fields["limit_requests"] = count("x-ratelimit-limit-requests");
    fields["reset_requests"] = text("x-ratelimit-reset-requests");
    fields["should_retry"] = text("x-should-retry");
    return fields;
  }

  void logLithosRateLimitRefusal(const LithosRateLimitLogFields& fields) { logLithosRateLimitEvent("[ai.ubq.fi] lithos_rate_limit_refusal", fields); }

  void logLithosRateLimitFailover(const LithosRateLimitLogFields& fields) { logLithosRateLimitEvent("[ai.ubq.fi] lithos_rate_limit_failover", fields); }

  bool lithosRetryProhibited(const rt::Headers& headers) {
    const std::optional<std::string> raw = trimmedHeader(headers, "x-should-retry");
    return raw && rt::toLowerCase(*raw) == "false";
  }

  std::optional<LithosRateLimitWait> lithosRateLimitWait(const rt::Headers& headers, double nowMs) {
    if (lithosRetryProhibited(headers)) return std::nullopt;
    const std::optional<double> retryAfterMs = lithosIntegerHeader(headers.get("retry-after-ms"));
    if (retryAfterMs) return LithosRateLimitWait{.waitMs = *retryAfterMs, .source = "retry-after-ms"};
    const std::optional<double> retryAfter = lithosRetryAfterMs(headers.get("retry-after"), nowMs);
    if (retryAfter) return LithosRateLimitWait{.waitMs = *retryAfter, .source = "retry-after"};
    std::optional<LithosRateLimitWait> latest;
    for (const std::string_view header : {std::string_view("x-ratelimit-reset-tokens"), std::string_view("x-ratelimit-reset-requests")}) {
      const std::optional<double> waitMs = lithosDurationMs(headers.get(header));
      if (!waitMs) continue;
      if (!latest || *waitMs > latest->waitMs) latest = LithosRateLimitWait{.waitMs = *waitMs, .source = std::string(header)};
    }
    if (latest) return latest;
    return std::nullopt;
  }
}
