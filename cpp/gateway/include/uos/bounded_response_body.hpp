#pragma once
/**
 * Read an upstream response body under fixed byte and time limits.
 *
 * The returned bytes are useful only when `complete` is true unless a caller
 * deliberately chooses to retain a partial body.  Incomplete reads are
 * cancelled without awaiting the upstream cancellation so a stalled peer
 * cannot extend the gateway request's latency.
 */
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/prelude.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace uos::bounded_response_body {
  struct BoundedResponseBody {
    std::vector<std::uint8_t> bytes;
    bool complete = false;
  };

  struct ReadBoundedResponseBodyOptions {
    /** An optional request-level cancellation signal. */
    std::optional<rt::AbortSignal> signal;
    /** Defaults to the shared 64 KiB upstream-error ceiling. */
    std::optional<double> maxBytes;
    /** Defaults to the shared one-second upstream-error deadline. */
    std::optional<double> timeoutMs;
    /** Passed to `ReadableStreamDefaultReader.cancel` for observability. */
    std::optional<json::value> cancellationReason;
  };

  inline constexpr double BOUNDED_RESPONSE_BODY_MAX_BYTES = 64 * 1024;
  inline constexpr double BOUNDED_RESPONSE_BODY_TIMEOUT_MS = 1'000;

  /**
   * Read at most `maxBytes` before the deadline, returning the bytes accumulated
   * so far and whether the upstream body ended normally.  Readers are released
   * exactly once; incomplete streams are cancelled best-effort in the
   * background to preserve the bounded caller latency.
   */
  [[nodiscard]] rt::Task<BoundedResponseBody> readBoundedResponseBody(rt::Response response, ReadBoundedResponseBodyOptions options);
  // `readBoundedResponseBody(response)` with the default `{}` options (an overload
  // rather than a default argument, so no options temporary is built inside a
  // caller's co_await expression).
  [[nodiscard]] rt::Task<BoundedResponseBody> readBoundedResponseBody(rt::Response response);
}
