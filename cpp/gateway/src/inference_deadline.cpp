#include "uos/inference_deadline.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/number.hpp"
#include <memory>
#include <utility>
#include <vector>

namespace uos::inference_deadline {
  namespace {
    double streamFirstEventDeadlineMs = STREAM_FIRST_EVENT_DEADLINE_MS;
  }

  rt::AbortSignal createInferenceSignal(rt::AbortSignal requestSignal, double timeoutMs) {
    rt::AbortSignal timeout = rt::AbortSignal::timeout(timeoutMs);
    return rt::AbortSignal::any({requestSignal, timeout});
  }

  StreamDeadline createStreamFirstEventDeadline(rt::AbortSignal requestSignal, std::optional<double> timeoutMsArgument) {
    const double timeoutMs = timeoutMsArgument ? *timeoutMsArgument : streamFirstEventDeadlineMs;
    const rt::AbortController deadline;
    const double deadlineAtMs = rt::performanceNow() + timeoutMs;
    const auto active = std::make_shared<bool>(true);
    const rt::Timeout timer = rt::setTimeout(
      [active, deadline] {
        if (!*active) return;
        deadline.abort(rt::makeReason(rt::DOMException("Upstream response headers or first SSE event timed out.", "TimeoutError")));
      },
      timeoutMs);
    return StreamDeadline{
      .signal = rt::AbortSignal::any({requestSignal, deadline.signal()}),
      .abort =
        [timer, deadline](rt::Reason reason) {
          // Releasing the timer keeps an abandoned attempt from holding its budget
          // handle for the rest of the deadline. There is deliberately no
          // active-state guard, so abort-after-clear still aborts the signal.
          rt::clearTimeout(timer);
          deadline.abort(std::move(reason));
        },
      .clear =
        [active, timer] {
          if (!*active) return;
          *active = false;
          rt::clearTimeout(timer);
        },
      .remainingMs = [deadlineAtMs] { return rt::Math::max(0, deadlineAtMs - rt::performanceNow()); },
    };
  }

  StreamDeadline createStreamSemanticDeadline(rt::AbortSignal requestSignal, std::optional<double> timeoutMsArgument) {
    const double timeoutMs = timeoutMsArgument ? *timeoutMsArgument : streamFirstEventDeadlineMs;
    const rt::AbortController deadline;
    const double deadlineAtMs = rt::performanceNow() + timeoutMs;
    const auto active = std::make_shared<bool>(true);
    const rt::Timeout timer = rt::setTimeout(
      [active, deadline] {
        if (!*active) return;
        deadline.abort(rt::makeReason(rt::DOMException("Upstream response timed out before semantic output.", "TimeoutError")));
      },
      timeoutMs);
    return StreamDeadline{
      .signal = rt::AbortSignal::any({requestSignal, deadline.signal()}),
      .abort =
        [timer, deadline](rt::Reason reason) {
          // Releasing the timer keeps an abandoned attempt from holding its budget
          // handle for the rest of the deadline. There is deliberately no
          // active-state guard, so abort-after-clear still aborts the signal.
          rt::clearTimeout(timer);
          deadline.abort(std::move(reason));
        },
      .clear =
        [active, timer] {
          if (!*active) return;
          *active = false;
          rt::clearTimeout(timer);
        },
      .remainingMs = [deadlineAtMs] { return rt::Math::max(0, deadlineAtMs - rt::performanceNow()); },
    };
  }

  namespace {
    double paidProviderFirstHeadersDeadlineMs = PAID_PROVIDER_FIRST_HEADERS_DEADLINE_MS;
  }

  StreamDeadline createPaidProviderAttemptDeadline(std::optional<rt::AbortSignal> requestSignal) {
    const rt::AbortController deadline;
    const double deadlineAtMs = rt::performanceNow() + paidProviderFirstHeadersDeadlineMs;
    const auto active = std::make_shared<bool>(true);
    const rt::Timeout timer = rt::setTimeout(
      [active, deadline] {
        if (!*active) return;
        deadline.abort(rt::makeReason(rt::DOMException("Paid provider response headers timed out.", "TimeoutError")));
      },
      paidProviderFirstHeadersDeadlineMs);
    return StreamDeadline{
      .signal = requestSignal ? rt::AbortSignal::any({*requestSignal, deadline.signal()}) : deadline.signal(),
      .abort =
        [timer, deadline](rt::Reason reason) {
          // Releasing the timer keeps an abandoned attempt from holding its budget
          // handle for the rest of the deadline. There is deliberately no
          // active-state guard, so abort-after-clear still aborts the signal.
          rt::clearTimeout(timer);
          deadline.abort(std::move(reason));
        },
      .clear =
        [active, timer] {
          if (!*active) return;
          *active = false;
          rt::clearTimeout(timer);
        },
      .remainingMs = [deadlineAtMs] { return rt::Math::max(0, deadlineAtMs - rt::performanceNow()); },
    };
  }

  void setStreamFirstEventDeadlineMsForTest(std::optional<double> timeoutMs) {
    streamFirstEventDeadlineMs = timeoutMs ? *timeoutMs : STREAM_FIRST_EVENT_DEADLINE_MS;
  }

  void setPaidProviderFirstHeadersDeadlineMsForTest(std::optional<double> timeoutMs) {
    paidProviderFirstHeadersDeadlineMs = timeoutMs ? *timeoutMs : PAID_PROVIDER_FIRST_HEADERS_DEADLINE_MS;
  }
}
