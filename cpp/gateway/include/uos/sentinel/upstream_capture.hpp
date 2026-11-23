#pragma once
/**
 * Request-owned passive upstream recorder plus strict trace parser and
 * canonicalizer for private v2 upstream evidence (frozen contract:
 * upstream-format-contract-v3.md, docs/contracts.md section 12).
 *
 * One recorder exists per accepted gateway request and is carried through the
 * existing UsageContext and provider options. It never intercepts fetch
 * globally, never clones/tees a response, never reads ahead or drains a stream
 * for capture, never records authentication material (headers, URLs, account
 * or request IDs), and never adds an upstream request or product
 * configuration. Recording is best effort: no recording failure may replace a
 * downstream response or error.
 *
 * Internal retained buffers are zeroable byte copies; base64 is produced only
 * when a snapshot is sealed. Attempts are bounded (8/256/131072) with
 * permanent truncation flags, exactly as the frozen contract specifies.
 */
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uos::sentinel::upstream_capture {
  inline constexpr std::int64_t SENTINEL_UPSTREAM_MAX_ATTEMPTS = 8;
  /**
   * Long local streams (a 35-second reasoning generation, an interrupted
   * Codex turn) routinely exceeded the original 128 KiB/256-chunk ceiling, so a
   * capture could be exported but not replayed. These bounds keep capture
   * finite while covering ordinary long local requests; exceeding either sets
   * the matching permanent truncation flag and can never be reported as full
   * replay coverage.
   */
  inline constexpr std::int64_t SENTINEL_UPSTREAM_MAX_CHUNKS = 4'096;
  inline constexpr std::int64_t SENTINEL_UPSTREAM_MAX_BYTES = 4 * 1'024 * 1'024;
  /** A trace may not claim more than seven days of elapsed timing. */
  inline constexpr std::int64_t SENTINEL_UPSTREAM_MAX_TIMING_MS = 7LL * 24 * 60 * 60 * 1'000;
  inline constexpr std::int64_t SENTINEL_UPSTREAM_MAX_HEADER_VALUE_CHARS = 512;

  /**
   * Fixed safe upstream response header allowlist. Authentication material,
   * cookies, provider account identifiers and request IDs are excluded by
   * construction; only bounded wire-shape and rate-limit metadata is retained.
   */
  inline constexpr auto SENTINEL_UPSTREAM_SAFE_RESPONSE_HEADER_NAMES = std::to_array<std::string_view>({
    "cache-control",
    "content-length",
    "content-type",
    "openai-processing-ms",
    "retry-after",
    "x-ratelimit-limit-requests",
    "x-ratelimit-limit-tokens",
    "x-ratelimit-remaining-requests",
    "x-ratelimit-remaining-tokens",
    "x-ratelimit-reset-requests",
    "x-ratelimit-reset-tokens",
  });

  // "chatgpt_codex" | "surplus" | "metered" | "cerebras" | "deepseek" | "lithos" | "openrouter" | "claude_code"
  using SentinelUpstreamProvider = std::string;

  // "pending" | "fetch_error" | "eof" | "read_error" | "cancelled"
  using SentinelUpstreamTerminal = std::string;

  // "text/event-stream" | "application/json" | "other"
  using SentinelUpstreamContentType = std::string;

  // Readonly<Record<string, string>>: every member value is a JSON string.
  using SentinelUpstreamSafeHeaders = json::object;

  /**
   * One recorded upstream attempt. Version-1 traces carry exactly the first five
   * fields; version-2 traces additionally carry the observed response headers
   * and attempt/chunk timing. Absent (undefined) timing fields mean the attempt
   * predates timing capture and must be reported as unavailable, never guessed.
   */
  struct SentinelUpstreamAttempt {
    SentinelUpstreamProvider provider;
    std::optional<std::int64_t> status;
    std::optional<SentinelUpstreamContentType> content_type;
    std::vector<std::string> chunks_base64;
    SentinelUpstreamTerminal terminal;
    std::optional<SentinelUpstreamSafeHeaders> headers;
    std::optional<bool> headers_truncated;
    std::optional<std::vector<double>> chunk_times_ms;
    std::optional<double> started_at_ms;
    rt::Nullable<double> headers_at_ms;
    rt::Nullable<double> ended_at_ms;

    friend bool operator==(const SentinelUpstreamAttempt&, const SentinelUpstreamAttempt&) = default;
  };

  struct SentinelUpstreamTrace {
    std::int64_t version{};  // 1 | 2
    std::vector<SentinelUpstreamAttempt> attempts;
    bool attempts_truncated{};
    bool bytes_truncated{};
    bool chunks_truncated{};

    friend bool operator==(const SentinelUpstreamTrace&, const SentinelUpstreamTrace&) = default;
  };

  // `json::value_from(attempt)` / `json::value_from(trace)`: the frozen JS object,
  // members in the order the module freezes them; undefined members are absent.
  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelUpstreamAttempt& attempt);
  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelUpstreamTrace& trace);

  /**
   * Handle for one actual dispatched upstream attempt. The response wrapper
   * retains consumed chunks only while the recorder is unsealed and forwards
   * the original stream without read-ahead.
   */
  struct SentinelUpstreamAttemptHandle {
    std::function<rt::Response(rt::Response response)> wrap;
    std::function<void()> recordFetchError;
  };

  // A handle object: copies share the recorder they were created by.
  struct SentinelUpstreamRecorder {
    std::function<SentinelUpstreamAttemptHandle(SentinelUpstreamProvider provider)> startAttempt;
    std::function<SentinelUpstreamTrace()> snapshotAndSeal;
    std::function<void()> dispose;
  };

  SentinelUpstreamTrace emptySentinelUpstreamTrace();

  /**
   * Normalize an upstream Content-Type before its semicolon without retaining
   * arbitrary text: exactly the three frozen literals, else `other`.
   */
  std::optional<SentinelUpstreamContentType> normalizeSentinelUpstreamContentType(const std::optional<std::string>& raw);

  /**
   * Strictly parse and validate one upstream trace. Crypto-authenticated JSON is
   * still untrusted input here: exact keys, literal enums, canonical padded
   * base64, aggregate byte/chunk/attempt bounds and cross-field consistency are
   * all enforced. Returns a deep immutable copy.
   */
  SentinelUpstreamTrace parseSentinelUpstreamTrace(const json::value& value);
  // The same check of an already typed trace (its `json::value_from` form).
  SentinelUpstreamTrace parseSentinelUpstreamTrace(const SentinelUpstreamTrace& value);

  bool isSentinelUpstreamTrace(const json::value& value);
  bool isSentinelUpstreamTrace(const SentinelUpstreamTrace& value);

  struct NormalizedSentinelUpstreamSafeHeaders {
    SentinelUpstreamSafeHeaders headers;
    bool truncated{};
  };

  /**
   * Normalize upstream response headers to the fixed safe allowlist. A value that
   * is too long to retain is dropped and reported through `truncated` rather than
   * silently shortened.
   */
  NormalizedSentinelUpstreamSafeHeaders normalizeSentinelUpstreamSafeHeaders(const rt::Headers& headers);

  /**
   * Canonical upstream JSON for the v2 fingerprint frame: recursively sorted
   * object keys, preserved array order, JSON.stringify primitive/string
   * encoding, no whitespace. Callers must strictly validate before
   * canonicalizing.
   */
  std::string canonicalSentinelUpstreamJson(const SentinelUpstreamTrace& trace);

  SentinelUpstreamRecorder createSentinelUpstreamRecorder();
}
