#pragma once
// Port of src/provider/lithos-rate-limits.ts.
// LithosAI rate-limit handling, split out of src/provider/lithos-handlers.ts.
//
// `usageContext: UsageContext | undefined` is `const openai_telemetry::UsageContext*` (nullptr is
// undefined); the recorders write through its shared response-telemetry state.
#include "uos/openai_telemetry.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/prelude.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace uos::provider::lithos_rate_limits {
  struct LithosRateLimitWait;

  /** The ordered tiers to try after a request model refuses, or an empty list at the bottom. */
  std::vector<std::string> lithosFailoverLadderFor(const std::string& modelRaw);

  /** True when `target` is a legal failover destination for `modelRaw`. */
  bool lithosIsLadderTarget(const std::string& modelRaw, const std::string& target);

  /** The ladder tier this request model must start on right now, or null once the window has passed. */
  std::optional<std::string> lithosFailoverTargetAt(const std::string& modelRaw, double nowMs);

  /** Opens (or extends) the refused rung's own window, inferred from its legal next target unless supplied. */
  // `refusedRungOverride?: string`: std::nullopt is undefined.
  void lithosOpenFailoverWindow(const std::string& modelRaw, double nowMs, double waitMs, const std::string& target,
                                const std::optional<std::string>& refusedRungOverride = std::nullopt);

  /** Test seam: drop every window so fixtures cannot leak into each other. */
  void clearLithosFailoverWindows();

  /** Records the final tier that served a request, or clears it when the requested tier served. */
  // `model: string | null`: std::nullopt is null.
  void recordLithosFailoverModel(const openai_telemetry::UsageContext* usageContext, const std::optional<std::string>& model);

  /** One sanitized rate-limit telemetry field: a bounded string, a number, or nothing. */
  // `Readonly<Record<string, string | number | null>>`: a JSON object of string, number or null members.
  using LithosRateLimitLogFields = json::object;

  inline constexpr double LITHOS_REFUSAL_WAIT_CAP_MS = 75'000;
  inline constexpr double LITHOS_REFUSAL_WAIT_TOTAL_CAP_MS = 120'000;
  /** Absorbed pauses per request, so a stream of tiny windows cannot loop forever. */
  inline constexpr double LITHOS_REFUSAL_WAIT_MAX_WAITS = 2;

  /**
   * The absorb budget one route may spend on refusals that name their own reset
   * window. The two routes genuinely differ: a buffered response is held silently
   * behind whatever proxy sits in front of the gateway, so its budget stays under
   * the edge read bound; a streamed response opens its SSE stream first and is
   * held by `: keepalive` frames, so it can follow the client's own ~10-minute
   * tolerance and cover the three-and-a-half-minute vendor saturations measured on
   * 2026-09-24 and 2026-09-25. Both budgets keep the same 75-second per-wait cap,
   * because the vendor's 4,000,000-token/minute bucket cannot name a longer
   * window than its own ~60-second refill.
   */
  struct LithosRefusalWaitPolicy {
    double maxWaits;
    double totalCapMs;
    friend bool operator==(const LithosRefusalWaitPolicy&, const LithosRefusalWaitPolicy&) = default;
  };

  inline constexpr LithosRefusalWaitPolicy LITHOS_BUFFERED_REFUSAL_WAIT_POLICY{
    .maxWaits = LITHOS_REFUSAL_WAIT_MAX_WAITS,
    .totalCapMs = LITHOS_REFUSAL_WAIT_TOTAL_CAP_MS,
  };

  inline constexpr LithosRefusalWaitPolicy LITHOS_STREAMED_REFUSAL_WAIT_POLICY{.maxWaits = 5, .totalCapMs = 300'000};

  /**
   * A waterfall hop has another provider behind it. Spending the vendor's reset
   * window here prevents that hop from ever being reached, so a refusal is
   * returned immediately and the waterfall advances. Direct LithosAI requests
   * keep the buffered/streamed policies above.
   */
  inline constexpr LithosRefusalWaitPolicy LITHOS_IMMEDIATE_FAILOVER_WAIT_POLICY{.maxWaits = 0, .totalCapMs = 0};

  /** The pause this refusal asks for, bounded by the per-wait, count and total caps. */
  // `LithosRateLimitWait | null`: std::nullopt is null.
  std::optional<LithosRateLimitWait> lithosRefusalWait(const rt::Headers& headers, double waitedMs, double waits,
                                                       const LithosRefusalWaitPolicy& policy = LITHOS_BUFFERED_REFUSAL_WAIT_POLICY);

  /** Records the elapsed absorbed wait total so a terminal can report it. */
  void recordLithosRateLimitWaitMs(const openai_telemetry::UsageContext* usageContext, double elapsedMs);

  /** One absorbed pause, logged with the header it came from. */
  void logLithosRateLimitWait(const LithosRateLimitLogFields& fields);

  /** Abort-aware sleep: an abandoned request never keeps waiting for a provider window; the observer sees elapsed time on either outcome. */
  // Returns the promise the TS builds (its synchronous part, including `onElapsed(0)`, runs when
  // called). `onElapsed?: (elapsedMs: number) => void`: the two-argument overload has none.
  rt::Promise<void> waitForLithosRetry(double milliseconds, rt::AbortSignal signal);
  rt::Promise<void> waitForLithosRetry(double milliseconds, rt::AbortSignal signal, std::function<void(double elapsedMs)> onElapsed);

  LithosRateLimitLogFields lithosRateLimitSnapshot(const rt::Headers& headers);

  /** One refusal, with the vendor's budgets as it reported them. */
  void logLithosRateLimitRefusal(const LithosRateLimitLogFields& fields);

  void logLithosRateLimitFailover(const LithosRateLimitLogFields& fields);

  /** One refusal hint parsed from the vendor's own headers. */
  struct LithosRateLimitWait {
    double waitMs;
    std::string source;
    friend bool operator==(const LithosRateLimitWait&, const LithosRateLimitWait&) = default;
  };

  /** True when the vendor explicitly forbids retrying the tier that returned these headers. */
  bool lithosRetryProhibited(const rt::Headers& headers);

  /**
   * The wait one 429 asks for, in the vendor's own precedence: `retry-after-ms`
   * first, then `retry-after`, then the later of the two `x-ratelimit-reset-*`
   * deltas (the budget that refills last is the one that binds). A vendor
   * instruction never to retry wins over every hint, and a refusal that names no
   * window at all returns null so it is relayed instead of guessed at. Exported
   * because it is the whole header contract in one pure function.
   */
  std::optional<LithosRateLimitWait> lithosRateLimitWait(const rt::Headers& headers, double nowMs);
}
