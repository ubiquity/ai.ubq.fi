#pragma once
// Port of src/sentinel/replay-model.ts.
// Sentinel replay constants, record types and byte primitives, split out of src/sentinel_replay_capture.ts.
//
// Byte arrays the TS shares between holders (an accepted request body, a decrypted plaintext body
// that callers zero) are `std::shared_ptr<rt::Bytes>`. `SentinelCompatibilityHeaders` is a JSON
// object whose members are strings, like `SentinelUpstreamSafeHeaders`.
#include "uos/request.hpp"
#include "uos/sentinel/replay_limits.hpp"
#include "uos/sentinel/upstream_capture.hpp"

#include "uos/rt/http.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/text_codec.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace uos::sentinel::replay_model {
  inline constexpr double SENTINEL_REPLAY_TTL_MS = 48 * 60 * 60 * 1'000;
  /**
   * Status rows are bounded non-sensitive metadata (request id, status, reason,
   * fingerprint, expiry). They outlive their payload by one additional replay TTL
   * so a request whose encrypted evidence has expired can still be reported as
   * `expired` instead of decaying into `unknown` at exactly the same instant.
   */
  inline constexpr double SENTINEL_REPLAY_STATUS_TTL_MS = 2 * SENTINEL_REPLAY_TTL_MS;
  inline constexpr double SENTINEL_REPLAY_CHUNK_BYTES = replay_limits::SENTINEL_REPLAY_STORAGE_CHUNK_BYTES;
  inline constexpr double SENTINEL_REPLAY_MAX_BODY_BYTES = static_cast<double>(request::MAX_ACCEPTED_JSON_BODY_BYTES);
  inline constexpr double SENTINEL_REPLAY_MAX_BUFFERED_OBSERVATION_BYTES = 1 * 1'024 * 1'024;
  /** Bounded observed downstream terminal/error body retained with a failure. */
  inline constexpr double SENTINEL_REPLAY_MAX_DOWNSTREAM_BODY_BYTES = replay_limits::SENTINEL_REPLAY_MAX_DOWNSTREAM_BYTES;
  inline constexpr double SENTINEL_REPLAY_EXPORT_PAGE_LIMIT = 1;
  inline const rt::KvKey SENTINEL_REPLAY_MANIFEST_PREFIX{"uos_ai", "sentinel_replay", "v1", "manifest"};
  inline const rt::KvKey SENTINEL_REPLAY_DEDUPE_PREFIX{"uos_ai", "sentinel_replay", "v1", "dedupe"};
  inline const rt::KvKey SENTINEL_REPLAY_CHUNK_PREFIX{"uos_ai", "sentinel_replay", "v1", "chunk"};
  /** Per-request capture status/correlation row, written with the capture. */
  inline const rt::KvKey SENTINEL_REPLAY_REQUEST_PREFIX{"uos_ai", "sentinel_replay", "v1", "request"};

  // `Readonly<Record<string, string>>`: every member value is a JSON string.
  using SentinelCompatibilityHeaders = json::object;

  struct SentinelFailureObservation {
    double status;
    std::optional<bool> stream;
    bool completed;
    std::optional<std::string> terminal_type;
    std::optional<std::string> failure_kind;
    std::optional<std::string> synthetic_terminal_type;
    std::string provider_route;
    friend bool operator==(const SentinelFailureObservation&, const SentinelFailureObservation&) = default;
  };

  struct SentinelClientFailureObservation {
    double status;
    bool stream;
    bool completed;
    std::optional<std::string> terminal_type;
    std::optional<std::string> failure_kind;
    bool framing_valid;
    std::string provider_route;
    /** Client-visible error code when the observed downstream body carried one. */
    std::optional<std::string> error_code;
    /** Validation discriminator (`param`) when the observed body carried one. */
    std::optional<std::string> error_param;
    /** Bounded base64 of the observed downstream terminal/error body, or null. */
    std::optional<std::string> terminal_body_base64;
    /** Whether that retained body was cut at the fixed bound. */
    bool terminal_body_truncated;
    friend bool operator==(const SentinelClientFailureObservation&, const SentinelClientFailureObservation&) = default;
  };

  struct SentinelClientBodyObservation {
    bool stream;
    bool completed;
    std::optional<std::string> terminal_type;
    std::optional<std::string> failure_kind;
    bool framing_valid;
    rt::Nullable<std::string> error_code{};
    rt::Nullable<std::string> error_param{};
    rt::Nullable<std::string> terminal_body_base64{};
    std::optional<bool> terminal_body_truncated{};
    friend bool operator==(const SentinelClientBodyObservation&, const SentinelClientBodyObservation&) = default;
  };

  struct AcceptedSentinelReplayInput {
    std::string endpoint;
    std::string method;
    std::shared_ptr<rt::Bytes> body;
    std::optional<std::string> content_type;
    SentinelCompatibilityHeaders compatibility_headers;
    std::string request_id;
    std::string git_sha;
    std::string deno_revision;
    /** Sealed immutable v2 upstream trace; absent means request-only evidence. */
    std::optional<upstream_capture::SentinelUpstreamTrace> upstream{};
    /** Request-owned passive upstream recorder carried until the snapshot handoff. */
    std::optional<upstream_capture::SentinelUpstreamRecorder> upstreamRecorder{};
    /** Received bytes can exist without a parsed request. This status is never serialized. */
    std::optional<std::string> bodyStatus{};
  };

  /** Why a body the gateway accepted was not carried into a capture. */
  // "body_over_limit" | "body_unavailable" | "non_post"
  using SentinelReplayBodyOmissionReason = std::string;

  // A mutable object the request's raw-body observer fills in later: callers hold it by
  // std::shared_ptr (`SentinelReplayCaptureCandidate | null` is a possibly null pointer).
  // `body` is the route's own buffer (null pointer = null).
  struct SentinelReplayCaptureCandidate {
    std::string endpoint;
    std::string method;
    std::shared_ptr<rt::Bytes> body;
    std::optional<std::string> content_type;
    SentinelCompatibilityHeaders compatibility_headers;
    std::string request_id;
    std::string git_sha;
    std::string deno_revision;
    /** Why no accepted body was captured, when that happened. */
    std::optional<SentinelReplayBodyOmissionReason> body_omitted_reason;
  };

  /**
   * Why a request produced no encrypted payload at all. In addition to the body
   * omissions, an authenticated request rejected before capture setup (quota
   * admission) still publishes an explicit status row rather than looking like a
   * request that never happened.
   */
  // SentinelReplayBodyOmissionReason | "rejected_before_capture"
  using SentinelReplayCaptureOmissionReason = std::string;

  /** Why a stored capture cannot be replayed in full. */
  // "request_body_omitted" | "upstream_trace_empty" | "upstream_trace_truncated" | "upstream_attempt_pending"
  // | "upstream_timing_unavailable" | "upstream_response_headers_unavailable"
  // | "downstream_terminal_body_unavailable" | "observed_serving_model_unavailable" | "provider_state_not_reproducible"
  using SentinelReplayUnavailableReason = std::string;

  inline constexpr std::array<std::string_view, 9> SENTINEL_REPLAY_UNAVAILABLE_REASONS{
    "request_body_omitted",
    "upstream_trace_empty",
    "upstream_trace_truncated",
    "upstream_attempt_pending",
    "upstream_timing_unavailable",
    "upstream_response_headers_unavailable",
    "downstream_terminal_body_unavailable",
    "observed_serving_model_unavailable",
    "provider_state_not_reproducible",
  };

  /** Request-declared settings read back from the recorded request bytes. */
  struct SentinelReplaySettings {
    std::string source;  // "recorded_request_body" | "unavailable"
    std::string provider_route;
    std::optional<std::string> model_requested;
    std::optional<std::string> reasoning_requested;
    std::optional<bool> stream_requested;
    std::optional<bool> stream_observed;
    friend bool operator==(const SentinelReplaySettings&, const SentinelReplaySettings&) = default;
  };

  /** Observed downstream terminal/error outcome carried with the capture. */
  struct SentinelReplayDownstream {
    std::optional<std::string> terminal_type;
    std::optional<std::string> failure_kind;
    std::optional<std::string> error_code;
    std::optional<std::string> error_param;
    std::optional<std::string> body_base64;
    bool body_truncated;
    friend bool operator==(const SentinelReplayDownstream&, const SentinelReplayDownstream&) = default;
  };

  // The decoded plaintext keeps the typed fields of its (validated) metadata. A version-2
  // `client_observation` has no error_code / error_param / terminal_body_* members: they read
  // as null / false here (the TS object leaves them undefined).
  struct SentinelReplayPlaintext {
    /** 2 = request/upstream only; 3 = adds settings, coverage and downstream outcome. */
    double version;  // 2 | 3
    double captured_at_ms;
    std::string endpoint;
    std::string method;
    std::optional<std::string> content_type;
    SentinelCompatibilityHeaders compatibility_headers;
    std::string failure_signature;
    SentinelFailureObservation observation;
    SentinelClientFailureObservation client_observation;
    std::string request_id;
    std::string git_sha;
    std::string deno_revision;
    upstream_capture::SentinelUpstreamTrace upstream;
    /** Version-3 additions; absent on a version-2 capture. */
    std::optional<SentinelReplaySettings> settings{};
    std::optional<std::string> capture_status{};  // "ready" | "incomplete"
    std::optional<std::string> replay_coverage{};  // "full" | "partial" | "unavailable"
    std::optional<std::vector<SentinelReplayUnavailableReason>> unavailable{};
    std::optional<std::string> body_sha256{};
    std::optional<double> body_bytes{};
    std::optional<SentinelReplayDownstream> downstream{};
    std::shared_ptr<rt::Bytes> body;
  };

  struct SentinelReplayManifest {
    double version = 1;
    std::string capture_id;
    std::string fingerprint;
    std::string case_group_digest;
    double captured_at_ms;
    double expires_at_ms;
    std::string algorithm;  // "AES-256-GCM"
    std::string compression;  // "gzip"
    std::string iv;
    double chunk_count;
    double ciphertext_bytes;
    /** Additive retention accounting: encoded KV charge when this build stored it. */
    std::optional<double> stored_bytes{};
    /** Additive owner lookup: request id, so eviction can publish a truthful status. */
    std::optional<std::string> request_id{};
    friend bool operator==(const SentinelReplayManifest&, const SentinelReplayManifest&) = default;
  };

  struct ExportedSentinelReplayCapture {
    SentinelReplayManifest manifest;
    std::vector<std::string> chunks;
    friend bool operator==(const ExportedSentinelReplayCapture&, const ExportedSentinelReplayCapture&) = default;
  };

  /**
   * Per-request capture status. It exists so a failure is discoverable by its
   * request id even when nothing was stored: a missing replay key, a failed
   * persist or a retention eviction must never look like an empty history.
   */
  // "ready" | "incomplete" | "disabled" | "failed" | "evicted" | "expired" | "unknown" | "status_not_retained"
  using SentinelReplayCaptureStatus = std::string;

  struct SentinelReplayCaptureStatusRow {
    double version = 1;
    std::string request_id;
    SentinelReplayCaptureStatus status;
    std::optional<std::string> reason;
    double captured_at_ms;
    std::optional<rt::KvKey> manifest_key;
    std::optional<std::string> fingerprint;
    std::optional<double> expires_at_ms;
    friend bool operator==(const SentinelReplayCaptureStatusRow&, const SentinelReplayCaptureStatusRow&) = default;
  };

  // JSON forms (members in declaration order, which is the order the TS writes them; value_to is strict).
  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelFailureObservation& value);
  SentinelFailureObservation tag_invoke(const json::value_to_tag<SentinelFailureObservation>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelClientFailureObservation& value);
  SentinelClientFailureObservation tag_invoke(const json::value_to_tag<SentinelClientFailureObservation>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelClientBodyObservation& value);
  SentinelClientBodyObservation tag_invoke(const json::value_to_tag<SentinelClientBodyObservation>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelReplaySettings& value);
  SentinelReplaySettings tag_invoke(const json::value_to_tag<SentinelReplaySettings>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelReplayDownstream& value);
  SentinelReplayDownstream tag_invoke(const json::value_to_tag<SentinelReplayDownstream>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelReplayManifest& value);
  SentinelReplayManifest tag_invoke(const json::value_to_tag<SentinelReplayManifest>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ExportedSentinelReplayCapture& value);
  ExportedSentinelReplayCapture tag_invoke(const json::value_to_tag<ExportedSentinelReplayCapture>&, const json::value& value);

  // The stored status row (its manifest key is a KV key, so the row is written as a KV value).
  rt::KvValue toKvValue(const SentinelReplayCaptureStatusRow& row);

  // ---- the module's `export { ... }` list, in its order ----------------------------------------
  inline constexpr double AES_GCM_IV_BYTES = 12;
  inline const rt::Regex CAPTURE_ID{"^[A-Za-z0-9_-]+$"};
  inline constexpr std::string_view CASE_GROUP_NAMESPACE_V1 = "uos-sentinel-replay-v1:case-group";
  inline constexpr std::array<std::string_view, 14> COMPATIBILITY_HEADER_NAMES{
    "accept",
    "openai-beta",
    "openai-organization",
    "openai-project",
    "originator",
    "user-agent",
    "x-codex-client-version",
    "x-stainless-arch",
    "x-stainless-lang",
    "x-stainless-os",
    "x-stainless-package-version",
    "x-stainless-retry-count",
    "x-stainless-runtime",
    "x-stainless-runtime-version",
  };
  inline const std::unordered_set<std::string_view> COMPATIBILITY_HEADER_NAME_SET{COMPATIBILITY_HEADER_NAMES.begin(), COMPATIBILITY_HEADER_NAMES.end()};
  inline constexpr double ENVELOPE_VERSION = 1;
  /** v2 fingerprint frame namespace; the outer crypto transport stays v1. */
  inline constexpr std::string_view FINGERPRINT_NAMESPACE_V2 = "uos-sentinel-replay-v2:fingerprint";
  inline const rt::Regex HEX_DIGEST{"^[0-9a-f]{64}$"};
  // `TEXT_ENCODER.encode("uos-sentinel-replay-v1")`
  inline const rt::Bytes KEY_DERIVATION_SALT = [] {
    constexpr std::string_view salt = "uos-sentinel-replay-v1";
    return rt::Bytes(salt.begin(), salt.end());
  }();
  inline const rt::Regex KV_CURSOR{"^[A-Za-z0-9_-]+={0,2}$"};
  /** Limits are shared with export/decode and the offline reader; see replay-limits.ts. */
  inline constexpr double MAX_CAPTURE_ID_CHARS = 128;
  inline constexpr double MAX_REPLAY_CHUNKS = replay_limits::SENTINEL_REPLAY_MAX_STORED_CHUNKS;
  inline constexpr double MAX_REPLAY_CIPHERTEXT_BYTES = replay_limits::SENTINEL_REPLAY_MAX_CIPHERTEXT_BYTES;
  inline constexpr double MAX_REPLAY_METADATA_BYTES = replay_limits::SENTINEL_REPLAY_MAX_METADATA_BYTES;
  inline constexpr double MAX_REPLAY_PLAINTEXT_BYTES = replay_limits::SENTINEL_REPLAY_MAX_PLAINTEXT_BYTES;
  inline constexpr double MAX_SSE_EVENT_CHARS = 16 * 1'024 * 1'024;
  inline constexpr double REPLAY_KEY_BYTES = 32;
  /** Current private plaintext: adds request settings, coverage and downstream outcome. */
  inline constexpr double REPLAY_PLAINTEXT_VERSION = 3;
  /** Private plaintext metadata cut over to version 2 with required upstream evidence. */
  inline constexpr double REPLAY_PLAINTEXT_VERSION_V2 = 2;
  // `new TextDecoder()`, shared by the replay modules (non-streaming decodes only).
  extern rt::TextDecoder TEXT_DECODER;
  inline const rt::TextEncoder TEXT_ENCODER{};

  rt::Bytes cloneBytes(std::span<const std::uint8_t> value);

  rt::Bytes concatBytes(std::span<const rt::Bytes> parts);
  rt::Bytes concatBytes(std::initializer_list<std::span<const std::uint8_t>> parts);

  rt::Bytes randomBytes(double length);

  rt::KvKey requestStatusKey(const std::string& requestId);

  std::string stableHeaderText(const SentinelCompatibilityHeaders& headers);
}
