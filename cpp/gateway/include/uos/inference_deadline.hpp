#pragma once
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include <functional>
#include <optional>

namespace uos::inference_deadline {
  /** Reference timeout guidance for direct OpenAI clients. */
  inline constexpr double OPENAI_DEFAULT_REQUEST_TIMEOUT_MS = 10 * 60'000;
  inline constexpr double OPENAI_FLEX_REQUEST_TIMEOUT_MS = 15 * 60'000;

  /**
   * Origin wall-clock budget for one inference attempt: provider dispatch, response
   * headers and the first SSE event must all arrive inside it. Once semantic
   * output starts, STREAM_INACTIVITY_DEADLINE_MS bounds the gaps between later
   * events instead. These 30-minute origin budgets do not override a proxy's
   * silent-read timeout. A proxied request can still receive Cloudflare 524 while
   * the origin attempt remains within its budget.
   */
  inline constexpr double STREAM_FIRST_EVENT_DEADLINE_MS = 1'800'000;
  inline constexpr double STREAM_FAILOVER_RESERVE_MS = 15'000;
  inline constexpr double STREAM_INACTIVITY_DEADLINE_MS = 1'800'000;

  /**
   * Bounds dispatch plus response headers for exactly one paid-tier attempt, so a
   * stalled provider cannot consume the whole 30-minute stream deadline before
   * the next tier is tried.
   *
   * Measured first-headers latency for successful Surplus `gpt-6-astra` requests:
   * p50 4.3 s, p90 14.4 s, max 111.8 s. Observed stalls exceeded 60 s with zero
   * bytes and every one succeeded in about 2 s on immediate retry, so this is
   * transient saturation rather than a slow success. 120 s clears the slowest
   * measured success with margin and still bounds a stall at 15x below
   * STREAM_FIRST_EVENT_DEADLINE_MS, which is what lets the waterfall reach the
   * next enabled paid tier instead of abandoning the request.
   */
  inline constexpr double PAID_PROVIDER_FIRST_HEADERS_DEADLINE_MS = 120'000;

  /**
   * Buffered inference shares the origin stream first-event budget. A silent
   * buffered response remains subject to Cloudflare's proxy read timeout, which
   * this origin deadline does not extend. The caller's request signal still caps
   * the whole request.
   */
  inline constexpr double INFERENCE_DEADLINE_MS = STREAM_FIRST_EVENT_DEADLINE_MS;
  inline constexpr double BUFFERED_INFERENCE_DEADLINE_MS = INFERENCE_DEADLINE_MS;

  struct StreamDeadline;

  rt::AbortSignal createInferenceSignal(rt::AbortSignal requestSignal, double timeoutMs = BUFFERED_INFERENCE_DEADLINE_MS);

  /**
   * Bounds the period before the first upstream SSE event, including provider
   * dispatch and response headers. Once the caller observes that first event it
   * must clear this deadline; the stream reader then owns renewable inactivity.
   *
   * `timeoutMs` std::nullopt (undefined) takes the current first-event deadline
   * (STREAM_FIRST_EVENT_DEADLINE_MS unless setStreamFirstEventDeadlineMsForTest).
   */
  StreamDeadline createStreamFirstEventDeadline(rt::AbortSignal requestSignal, std::optional<double> timeoutMs = std::nullopt);

  // Readonly<{ signal; abort: (reason?: unknown) => void; clear: () => void; remainingMs: () => number }>.
  // `abort(nullptr)` is `abort()` (an undefined reason).
  struct StreamDeadline {
    rt::AbortSignal signal;
    std::function<void(rt::Reason reason)> abort;
    std::function<void()> clear;
    std::function<double()> remainingMs;
  };

  /**
   * Bounds one provider attempt until semantic Responses output or a valid
   * terminal event. Each failover attempt gets its own controller and timer.
   */
  StreamDeadline createStreamSemanticDeadline(rt::AbortSignal requestSignal, std::optional<double> timeoutMs = std::nullopt);

  /**
   * Bounds one paid-provider attempt until response headers arrive. The caller
   * clears the timer once the attempt settles, so the delivered response body is
   * never tied to this deadline; request-level streaming keeps its own deadlines.
   */
  StreamDeadline createPaidProviderAttemptDeadline(std::optional<rt::AbortSignal> requestSignal = std::nullopt);

  // `timeoutMs: number | null`; null restores the default.
  void setStreamFirstEventDeadlineMsForTest(std::optional<double> timeoutMs);
  void setPaidProviderFirstHeadersDeadlineMsForTest(std::optional<double> timeoutMs);
}
